// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/frame_trace.h"
#include "video_core/gpu.h"
#include "video_core/perf_counters.h"
#include "video_core/performance_overlay.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_overlay_memory.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/renderer_d3d12/renderer_d3d12_internal.h"
#include "video_core/shader_notify.h"

// Overlays drawn over the presented image: performance HUD, trace and shader indicators, load
// progress and the game menu (ClearRenderTargetView rects, no pipeline).

namespace D3D12 {

namespace {

/// Shared 3x5 overlay font, drawn as batched ClearRenderTargetView rects.
/// Digits 0-9 then '/'; rows top to bottom, bit 2 is the left column.
constexpr std::array<std::array<u8, 5>, 11> GLYPHS{{
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    {1, 1, 2, 4, 4},
}};

constexpr std::array<std::array<u8, 5>, 26> LETTER_GLYPHS{{
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7},
    {7,4,6,4,4}, {3,4,5,5,3}, {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2},
    {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2},
    {6,5,6,4,4}, {2,5,5,3,1}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
    {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2},
    {7,1,2,4,7},
}};

std::array<u8, 5> Glyph(char c) {
    if (c >= '0' && c <= '9') {
        return GLYPHS[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return LETTER_GLYPHS[c - 'A'];
    }
    switch (c) {
    case '/': return GLYPHS[10];
    case '.': return {0,0,0,0,2};
    case '%': return {5,1,2,4,5};
    case '-': return {0,0,7,0,0};
    case ':': return {0,2,0,2,0};
    case '<': return {1,2,4,2,1};
    case '>': return {4,2,1,2,4};
    default: return {};
    }
}

/// Width of `chars` glyphs of `cell` pixels, with a one-cell gap between them.
LONG TextWidth(size_t chars, LONG cell) {
    return chars == 0 ? 0 : static_cast<LONG>(chars) * 4 * cell - cell;
}

/// Shared bitmap text for shader progress and performance panels. Spaces advance normally.
void AppendText(std::vector<D3D12_RECT>& rects, std::string_view text, LONG x, LONG y, LONG cell) {
    for (const char c : text) {
        const auto glyph = Glyph(c);
        for (LONG row = 0; row < 5; ++row) {
            LONG column = 0;
            while (column < 3) {
                if (((glyph[row] >> (2 - column)) & 1) == 0) {
                    ++column;
                    continue;
                }
                const LONG start = column++;
                while (column < 3 && ((glyph[row] >> (2 - column)) & 1)) {
                    ++column;
                }
                rects.push_back({x + start * cell, y + row * cell, x + column * cell,
                                 y + (row + 1) * cell});
            }
        }
        x += 4 * cell;
    }
}

void ClearRects(CommandStream* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                const std::array<float, 4>& color, std::span<const D3D12_RECT> rects) {
    // The console keeps only part of one clear with a few hundred rects (the HUD showed its last
    // line alone), so the text goes in small batches, as the indicators always did.
    constexpr size_t BATCH = 64;
    for (size_t first = 0; first < rects.size(); first += BATCH) {
        const size_t count = std::min(BATCH, rects.size() - first);
        cmd->ClearRenderTargetView(rtv, color.data(), static_cast<UINT>(count),
                                   rects.data() + first);
    }
}

// Overlay colors: a near-black panel, dim and amber activity dots, light gray text.
constexpr std::array<float, 4> OVERLAY_PANEL{0.04f, 0.04f, 0.05f, 1.0f};
constexpr std::array<float, 4> OVERLAY_DIM{0.25f, 0.25f, 0.28f, 1.0f};
constexpr std::array<float, 4> OVERLAY_ACCENT{1.0f, 0.72f, 0.2f, 1.0f};
constexpr std::array<float, 4> OVERLAY_TEXT{0.82f, 0.82f, 0.84f, 1.0f};

} // Anonymous namespace

void ShowLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total) {
    // No RTTI in this build: the configured backend says which renderer this is.
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowLoadProgress(done, total);
    }
}

void RendererD3D12::DrawPerformanceOverlay(CommandStream* cmd,
                                          D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    auto& hud = performance_overlay;
    if (!VideoCore::performance_overlay_enabled.load(std::memory_order_relaxed)) {
        if (hud.sample_time != std::chrono::steady_clock::time_point{}) hud = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool first = hud.sample_time == std::chrono::steady_clock::time_point{};
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - hud.sample_time).count();
    if (first || elapsed_ms >= 500.0) {
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool cpu_valid = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto ticks = [](FILETIME time) {
            return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        const u64 cpu_ticks = ticks(kernel) + ticks(user);
        const u64 gpu_us = VideoCore::Perf::counters[
            static_cast<size_t>(VideoCore::Perf::Counter::GpuBusyUs)].load(std::memory_order_relaxed);
        // Reuse the HUD refresh and cached text. At most one lightweight OS query per second,
        // including failures; hiding the HUD resets this state and performs no queries.
        if (first || now - hud.memory_sample_time >= std::chrono::seconds{1}) {
            hud.memory_line = FormatOverlayResidentMemory(QueryOverlayResidentMemory());
            hud.memory_sample_time = now;
        }
        // The latest hitch for ten seconds, by the short name of its likely cause (the log has
        // the numbers). Indexed by HitchCause.
        static constexpr std::array<const char*, 14> HITCH_CAUSES{
            "PSO", "JIT", "DISCO", "LIMPIEZA", "RECURSOS", "TEXTURAS", "LECTURA GPU", "GPU",
            "CPU JUEGO", "MEMORIA", "RENDER", "JUEGO: INTERCAMBIO", "JUEGO: ESPERA GPU",
            "JUEGO: SERVICIO"};
        std::string hitch_line = "TIRON --";
        if (hitch.last_time != std::chrono::steady_clock::time_point{} &&
            now - hitch.last_time < std::chrono::seconds{10}) {
            hitch_line = fmt::format("TIRON {:.0f} MS {}", hitch.last_ms,
                                     HITCH_CAUSES[static_cast<size_t>(hitch.last_cause)]);
        }
        std::vector<std::string> lines{
            "FPS --", "FRAME -- MS MAX --", "CPU --", "GPUQ --", hud.memory_line, hitch_line};
        // The frontend's lines under ours (online play), amber when they say something is wrong.
        const std::size_t own_lines = lines.size();
        std::vector<bool> warning(own_lines, false);
        if (const auto source = VideoCore::overlay_extra_lines.load(std::memory_order_relaxed)) {
            for (auto& line : source()) {
                lines.push_back(std::move(line.text));
                warning.push_back(line.warning);
            }
        }
        if (!first && hud.frames != 0) {
            const double frames = static_cast<double>(hud.frames);
            lines[0] = fmt::format("FPS {:.1f}", frames * 1000.0 / elapsed_ms);
            lines[1] = fmt::format("FRAME {:.1f} MS MAX {:.1f}", hud.last_frame_ms, hud.max_frame_ms);
            // Process CPU includes all threads; show equivalent cores rather than an ambiguous %.
            if (cpu_valid && hud.cpu_valid && cpu_ticks >= hud.cpu_ticks) {
                const double cpu_ms = static_cast<double>(cpu_ticks - hud.cpu_ticks) / 10000.0;
                lines[2] = fmt::format("CPU {:.2f} CORES {:.1f} MS/F", cpu_ms / elapsed_ms,
                                       cpu_ms / frames);
            }
            if (scheduler.HasGpuTimestamps()) {
                const double gpu_ms = static_cast<double>(gpu_us - hud.gpu_us) / 1000.0;
                // Completed direct-queue time, delayed by frames in flight; not whole-GPU utilization.
                lines[3] = fmt::format("GPUQ {:.0f}% {:.1f} MS/F", gpu_ms * 100.0 / elapsed_ms,
                                       gpu_ms / frames);
            }
        }
        const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
        const LONG pad = 3 * cell;
        const LONG right = static_cast<LONG>(swapchain.Width()) - 8 * cell;
        const LONG top = 8 * cell;
        size_t chars = 0;
        for (const auto& line : lines) {
            chars = std::max(chars, line.size());
        }
        const LONG left = right - TextWidth(chars, cell) - 2 * pad;
        const size_t rows = lines.size() + (lines.size() > own_lines ? 1 : 0);
        hud.panel = {left, top, right, top + 2 * pad + static_cast<LONG>(rows) * 7 * cell - 2 * cell};
        hud.text.clear();
        hud.text.reserve(1536);
        hud.warning_text.clear();
        for (size_t row = 0; row < lines.size(); ++row) {
            // A gap of one line between ours and the frontend's.
            const LONG line_top = top + pad + static_cast<LONG>(row + (row >= own_lines ? 1 : 0)) * 7 * cell;
            AppendText(warning[row] ? hud.warning_text : hud.text, lines[row], left + pad, line_top, cell);
        }
        hud.sample_time = now;
        hud.cpu_ticks = cpu_ticks;
        hud.cpu_valid = cpu_valid;
        hud.gpu_us = gpu_us;
        hud.frames = 0;
        hud.max_frame_ms = 0;
    }
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&hud.panel, 1});
    ClearRects(cmd, rtv, OVERLAY_TEXT, hud.text);
    ClearRects(cmd, rtv, OVERLAY_ACCENT, hud.warning_text);
    DrawTraceIndicator(cmd, rtv);
}

void RendererD3D12::DrawTraceIndicator(CommandStream* cmd,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    using VideoCore::FrameTrace::CaptureState;
    const auto status = VideoCore::FrameTrace::GetCaptureStatus();
    if (status.state == CaptureState::Idle) {
        return;
    }
    auto& hud = performance_overlay;
    const u32 tenths = (status.vsyncs_remaining + 5) / 6;
    const u64 key = (static_cast<u64>(status.id) << 32) |
                    (static_cast<u64>(status.state) << 24) | tenths;
    const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
    const LONG top = hud.panel.bottom + 2 * cell;
    if (key != hud.trace_key || top != hud.trace_top) {
        std::string label;
        switch (status.state) {
        case CaptureState::Recording:
            label = fmt::format("T {} CAPTURANDO {:.1f} S", status.id, tenths / 10.0);
            break;
        case CaptureState::Saving: label = fmt::format("T {} GUARDANDO", status.id); break;
        case CaptureState::Saved: label = fmt::format("T {} GUARDADA", status.id); break;
        case CaptureState::Truncated: label = fmt::format("T {} TRUNCADA", status.id); break;
        case CaptureState::Armed: label = fmt::format("T {} ARMADA", status.id); break;
        default: return;
        }
        const LONG pad = 3 * cell;
        const LONG right = hud.panel.right;
        const LONG left = right - TextWidth(label.size(), cell) - 2 * pad;
        hud.trace_panel = {left, top, right, top + 2 * pad + 5 * cell};
        hud.trace_text.clear();
        hud.trace_text.reserve(256);
        AppendText(hud.trace_text, label, left + pad, top + pad, cell);
        hud.trace_key = key;
        hud.trace_top = top;
    }
    constexpr std::array<float, 4> saved_color{0.3f, 1.0f, 0.45f, 1.0f};
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&hud.trace_panel, 1});
    ClearRects(cmd, rtv, status.state == CaptureState::Saved ? saved_color : OVERLAY_ACCENT,
               hud.trace_text);
}

void RendererD3D12::DrawShaderIndicator(CommandStream* cmd,
                                        D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    // What is still compiling: the running total since the last idle moment kept climbing while
    // a single slow pipeline was in the driver, as if nothing ever finished.
    const int building = gpu.ShaderNotify().ShadersPending();
    if (building <= 0) {
        indicator_frames = 0;
        return;
    }
    ++indicator_frames;
    // 3-pixel cells at 1080p: the panel is 27 pixels tall, out of the way in the corner.
    const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
    const std::string count = std::to_string(building);
    const LONG pad = 3 * cell;
    const LONG dots_width = 5 * cell;
    const LONG width = pad + dots_width + 3 * cell + TextWidth(count.size(), cell) + pad;
    const LONG height = pad + 5 * cell + pad;
    const LONG right = static_cast<LONG>(swapchain.Width()) - 8 * cell;
    const LONG bottom = static_cast<LONG>(swapchain.Height()) - 8 * cell;
    const LONG left = right - width;
    const LONG top = bottom - height;
    const D3D12_RECT panel{left, top, right, bottom};
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&panel, 1});

    // Three dots on the middle row; the lit one moves every 8 presented frames.
    std::vector<D3D12_RECT> dim;
    std::vector<D3D12_RECT> lit;
    const LONG dot_y = top + pad + 2 * cell;
    for (LONG dot = 0; dot < 3; ++dot) {
        const LONG x = left + pad + dot * 2 * cell;
        (dot == static_cast<LONG>((indicator_frames / 8) % 3) ? lit : dim)
            .push_back({x, dot_y, x + cell, dot_y + cell});
    }
    ClearRects(cmd, rtv, OVERLAY_DIM, dim);
    ClearRects(cmd, rtv, OVERLAY_ACCENT, lit);

    std::vector<D3D12_RECT> text;
    AppendText(text, count, left + pad + dots_width + 3 * cell, top + pad, cell);
    ClearRects(cmd, rtv, OVERLAY_TEXT, text);
}

bool ResizeGameOutput(VideoCore::RendererBase& renderer, u32 width, u32 height) {
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Direct3D12) {
        return false;
    }
    return static_cast<RendererD3D12&>(renderer).ResizeOutput(width, height);
}

std::pair<u32, u32> GameMenuScreenSize(VideoCore::RendererBase& renderer) {
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Direct3D12) {
        return {};
    }
    return static_cast<RendererD3D12&>(renderer).ScreenSize();
}

void ShowGameMenu(VideoCore::RendererBase& renderer, MenuImage image) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowGameMenu(std::move(image));
    }
}

void HideGameMenu(VideoCore::RendererBase& renderer) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowGameMenu(std::nullopt);
    }
}

void ShowGameNotice(VideoCore::RendererBase& renderer, std::optional<MenuImage> image) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowGameNotice(std::move(image));
    }
}

void PostGameMotionView(VideoCore::RendererBase& renderer, std::optional<MenuImage> image) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).PostMotionView(std::move(image));
    }
}

MenuImage CaptureGameFrame(VideoCore::RendererBase& renderer) {
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Direct3D12) {
        return {};
    }
    return static_cast<RendererD3D12&>(renderer).CaptureFrame();
}

namespace {
std::mutex boot_screen_mutex;
BootScreen boot_screen_drawer;
} // Anonymous namespace

void SetBootScreen(BootScreen draw) {
    std::scoped_lock lock{boot_screen_mutex};
    boot_screen_drawer = std::move(draw);
}

BootScreen PresentDetail::TakeBootScreen() {
    std::scoped_lock lock{boot_screen_mutex};
    return std::exchange(boot_screen_drawer, nullptr);
}

void EndBootScreen(VideoCore::RendererBase& renderer) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).EndBootScreen();
    }
}

void RendererD3D12::StartBootScreen(BootScreen draw) {
    boot_screen = std::jthread{[this, draw = std::move(draw)](std::stop_token stop) {
        constexpr auto period = std::chrono::milliseconds{33};
        std::mutex mutex;
        std::condition_variable_any wake;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            MenuImage image;
            try {
                image = draw(ScreenSize());
            } catch (...) {
                // The screen keeps its last frame; the boot goes on without it.
                LOG_WARNING(Render, "D3D12: boot screen stopped drawing");
                return;
            }
            if (!image.pixels.empty()) ShowGameMenu(std::move(image));
            next += period;
            if (const auto now = std::chrono::steady_clock::now(); next < now) next = now;
            std::unique_lock lock{mutex};
            wake.wait_until(lock, stop, next, [] { return false; });
        }
    }};
}

void RendererD3D12::EndBootScreen() {
    if (!boot_screen.joinable()) return;
    boot_screen.request_stop();
    boot_screen.join();
    SetOverlay(game_menu, std::nullopt);
}

void ShowCpuLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowLoadProgress(done, total, "CPU JIT");
    }
}

void RendererD3D12::ShowLoadProgress(size_t done, size_t total, std::string_view phase) {
    if (present_failed || total == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (done != total && now - last_load_present < std::chrono::milliseconds{33}) {
        return;
    }
    last_load_present = now;
    try {
        FrameLease frame = present_manager.AcquireFrame();
        CommandStream* const cmd = scheduler.CommandList();
        frame->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = frame->rtv;
        constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);

        // A bar across the middle 40% of the screen and "done/total" centered under it.
        const LONG screen_width = static_cast<LONG>(swapchain.Width());
        const LONG screen_height = static_cast<LONG>(swapchain.Height());
        const LONG cell = std::max<LONG>(2, screen_height / 270);
        const LONG bar_width = screen_width * 2 / 5;
        const LONG bar_left = (screen_width - bar_width) / 2;
        const LONG bar_top = screen_height / 2 - cell;
        const LONG filled = static_cast<LONG>(static_cast<double>(bar_width) *
                                              static_cast<double>(std::min(done, total)) /
                                              static_cast<double>(total));
        const D3D12_RECT bar{bar_left, bar_top, bar_left + bar_width, bar_top + 2 * cell};
        const D3D12_RECT fill{bar_left, bar_top, bar_left + filled, bar_top + 2 * cell};
        ClearRects(cmd, rtv, OVERLAY_DIM, {&bar, 1});
        if (filled > 0) {
            ClearRects(cmd, rtv, OVERLAY_ACCENT, {&fill, 1});
        }
        const std::string label = fmt::format("{} {}/{}", phase, done, total);
        std::vector<D3D12_RECT> text;
        AppendText(text, label, (screen_width - TextWidth(label.size(), cell)) / 2,
                   bar_top + 6 * cell, cell);
        ClearRects(cmd, rtv, OVERLAY_TEXT, text);
        Present(std::move(frame));
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: shader cache progress not shown: {}", e.what());
    }
}

void RendererD3D12::SetOverlay(OverlayImage& overlay, std::optional<MenuImage> image) {
    if (image && (image->width == 0 || image->height == 0 ||
                  image->pixels.size() < static_cast<size_t>(image->width) * image->height * 4)) {
        image.reset();
    }
    overlay.image = std::move(image);
    overlay.dirty = overlay.image.has_value();
    if (!overlay.image) {
        // Frames in flight may still copy from it: the scheduler keeps it alive until they end.
        scheduler.DeferRelease(std::move(overlay.texture));
        overlay.texture_width = 0;
        overlay.texture_height = 0;
    }
}

void RendererD3D12::ShowGameNotice(std::optional<MenuImage> notice) {
    SetOverlay(game_notice, std::move(notice));
}

void RendererD3D12::PostMotionView(std::optional<MenuImage> view) {
    std::scoped_lock lock{motion_mutex};
    pending_motion = std::move(view);
}

void RendererD3D12::TakeMotionView() {
    std::optional<std::optional<MenuImage>> view;
    {
        std::scoped_lock lock{motion_mutex};
        view.swap(pending_motion);
    }
    if (view) {
        SetOverlay(game_motion, std::move(*view));
    }
}

void RendererD3D12::ShowGameMenu(std::optional<MenuImage> menu) {
    SetOverlay(game_menu, std::move(menu));
    if (present_failed) {
        return;
    }
    try {
        FrameLease frame = present_manager.AcquireFrame();
        // The guest is paused: blit its last frame again (the menu is drawn with it), or show the
        // menu on black when that frame is not a GPU image. A screen of its own hides that frame.
        const bool own_screen = game_menu.image && game_menu.image->fill_screen;
        if (own_screen || !blit_ready || last_framebuffers.empty() ||
            !CompositeLayers(last_framebuffers, *frame)) {
            CommandStream* const cmd = scheduler.CommandList();
            frame->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            cmd->ClearRenderTargetView(frame->rtv, black, 0, nullptr);
            DrawGameMenu(cmd, *frame);
        }
        Present(std::move(frame));
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: game menu not shown: {}", e.what());
    }
}

MenuImage RendererD3D12::CaptureFrame() {
    if (present_failed || !blit_ready || last_framebuffers.empty()) {
        return {};
    }
    try {
        // A frame of the ring, composited and read back but given back unpresented: the screen
        // keeps what it shows.
        FrameLease frame = present_manager.AcquireFrame();
        bare_composite = true;
        const bool drawn = CompositeLayers(last_framebuffers, *frame);
        bare_composite = false;
        if (!drawn) {
            return {};
        }
        StagingBufferRef readback = RecordFrameReadback(*frame);
        scheduler.Finish();
        MenuImage image{swapchain.Width(), swapchain.Height(), {}};
        const size_t row_bytes = static_cast<size_t>(image.width) * 4;
        image.pixels.resize(row_bytes * image.height);
        for (u32 y = 0; y < image.height; ++y) {
            std::memcpy(image.pixels.data() + y * row_bytes,
                        readback.mapped_span.data() + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                        row_bytes);
        }
        staging_pool.FreeDeferred(readback);
        return image;
    } catch (const std::exception& e) {
        bare_composite = false;
        LOG_ERROR(Render, "D3D12: game frame not captured: {}", e.what());
        return {};
    }
}

void RendererD3D12::DrawGameMenu(CommandStream* cmd, PresentFrame& frame) {
    if (game_menu.image && game_menu.image->fill_screen) {
        // Over the guest frame too, when the guest presents while the screen is up.
        frame.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const u32 rgb = game_menu.image->fill;
        const float color[4] = {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                                static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                                static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
        cmd->ClearRenderTargetView(frame.rtv, color, 0, nullptr);
    }
    // The motion view belongs to the game being played: any frontend image (the menu, Pulse
    // saving or resuming, the boot screen) hides it.
    if (!game_menu.image) {
        BlendOverlay(cmd, frame, game_motion);
    }
    CopyOverlay(cmd, frame, game_menu);
    CopyOverlay(cmd, frame, game_notice);
}

bool RendererD3D12::UploadOverlay(CommandStream* cmd, OverlayImage& overlay) {
    if (!overlay.image) {
        return false;
    }
    const MenuImage& image = *overlay.image;
    const D3D12_RESOURCE_STATES ready = overlay.blended
                                            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            : D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (overlay.dirty) {
        // A new image goes into a texture of its own once, so a present over the running game is
        // a GPU copy and never a CPU upload.
        if (!overlay.texture || overlay.texture_width != image.width ||
            overlay.texture_height != image.height) {
            scheduler.DeferRelease(std::move(overlay.texture));
            const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
            const D3D12_RESOURCE_DESC desc{
                .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                .Width = image.width,
                .Height = image.height,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .Format = Swapchain::FORMAT,
                .SampleDesc = {1, 0},
                .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            };
            ThrowIfFailed(device.Get()->CreateCommittedResource(
                              &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                              nullptr, IID_PPV_ARGS(&overlay.texture)),
                          "CreateCommittedResource (menu overlay)");
            overlay.texture_width = image.width;
            overlay.texture_height = image.height;
            if (overlay.blended) {
                if (overlay.srv.ptr == 0) {
                    overlay.srv = view_descriptors.Allocate();
                }
                device.Get()->CreateShaderResourceView(overlay.texture.Get(), nullptr, overlay.srv);
            }
        } else {
            D3D12_RESOURCE_BARRIER to_dest{};
            to_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_dest.Transition = {overlay.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  ready, D3D12_RESOURCE_STATE_COPY_DEST};
            cmd->ResourceBarrier(1, &to_dest);
        }
        const u32 row_pitch = Common::AlignUp(image.width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        const StagingBufferRef upload = staging_pool.Request(
            static_cast<size_t>(row_pitch) * image.height, MemoryUsage::Upload);
        for (u32 y = 0; y < image.height; ++y) {
            std::memcpy(upload.mapped_span.data() + static_cast<size_t>(y) * row_pitch,
                        image.pixels.data() + static_cast<size_t>(y) * image.width * 4,
                        static_cast<size_t>(image.width) * 4);
        }
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{
            .Offset = upload.offset,
            .Footprint = {Swapchain::FORMAT, image.width, image.height, 1, row_pitch},
        };
        const D3D12_TEXTURE_COPY_LOCATION from_upload{
            .pResource = upload.buffer,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = placed,
        };
        const D3D12_TEXTURE_COPY_LOCATION into_texture{
            .pResource = overlay.texture.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };
        cmd->CopyTextureRegion(&into_texture, 0, 0, 0, &from_upload, nullptr);
        D3D12_RESOURCE_BARRIER to_source{};
        to_source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_source.Transition = {overlay.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, ready};
        cmd->ResourceBarrier(1, &to_source);
        overlay.dirty = false;
    }
    return true;
}

void RendererD3D12::BlendOverlay(CommandStream* cmd, PresentFrame& frame, OverlayImage& overlay) {
    const auto& pipeline = blit_pipelines[static_cast<size_t>(Tegra::BlendMode::Premultiplied)];
    if (!pipeline || !overlay.blended) {
        CopyOverlay(cmd, frame, overlay);
        return;
    }
    if (!UploadOverlay(cmd, overlay)) {
        return;
    }
    const MenuImage& image = *overlay.image;
    const u32 screen_width = swapchain.Width();
    const u32 screen_height = swapchain.Height();
    const u32 width = std::min(image.width, screen_width);
    const u32 height = std::min(image.height, screen_height);
    const u32 x =
        image.placed ? std::min(image.x, screen_width - width) : (screen_width - width) / 2;
    const u32 y =
        image.placed ? std::min(image.y, screen_height - height) : (screen_height - height) / 2;
    // Descriptors first: taking them may flush the list, so the state is set after.
    constexpr u64 LINEAR_SAMPLER_KEY = 0;
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&LINEAR_SAMPLER_KEY, 1}, {&linear_sampler, 1});
    const D3D12_GPU_DESCRIPTOR_HANDLE source_table = descriptor_ring.Upload({&overlay.srv, 1});
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    frame.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = frame.rtv;
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // One texel per pixel: the viewport is the image's own rectangle.
    const D3D12_VIEWPORT viewport{
        .TopLeftX = static_cast<float>(x),
        .TopLeftY = static_cast<float>(y),
        .Width = static_cast<float>(image.width),
        .Height = static_cast<float>(image.height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    const D3D12_RECT scissor{static_cast<LONG>(x), static_cast<LONG>(y),
                             static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    // As RecordBlit: the bottom of the quad samples tex_offset, the top tex_offset + tex_scale.
    const std::array<float, 4> tex_scale_offset{1.0f, -1.0f, 0.0f, 1.0f};
    cmd->SetGraphicsRootSignature(blit_root_signature.Get());
    scheduler.SetPipelineState(pipeline.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 4, tex_scale_offset.data(), 0);
    const u32 runtime_data[12]{};
    cmd->SetGraphicsRoot32BitConstants(1, 12, runtime_data, 0);
    cmd->SetGraphicsRootDescriptorTable(2, source_table);
    cmd->SetGraphicsRootDescriptorTable(3, sampler_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void RendererD3D12::CopyOverlay(CommandStream* cmd, PresentFrame& frame, OverlayImage& overlay) {
    if (!UploadOverlay(cmd, overlay)) {
        return;
    }
    const MenuImage& image = *overlay.image;
    // Clipped to the screen; the frontend sizes the image to fit, this only guards a resize.
    const u32 screen_width = swapchain.Width();
    const u32 screen_height = swapchain.Height();
    const u32 width = std::min(image.width, screen_width);
    const u32 height = std::min(image.height, screen_height);
    const u32 x =
        image.placed ? std::min(image.x, screen_width - width) : (screen_width - width) / 2;
    const u32 y =
        image.placed ? std::min(image.y, screen_height - height) : (screen_height - height) / 2;
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = overlay.texture.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = frame.image.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    // A blended overlay copied (no present pipeline) rests as a shader resource.
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition = {overlay.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    if (overlay.blended) cmd->ResourceBarrier(1, &to_copy);
    frame.Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyTextureRegion(&dst, x, y, 0, &src, &box);
    frame.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (overlay.blended) {
        std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
        cmd->ResourceBarrier(1, &to_copy);
    }
}

} // namespace D3D12
