// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/page_table.h"
#include "common/pulse_diagnostics.h"
#include "core/hle/kernel/k_scheduler.h"
#include "jadyx/app/alloc_watch.h"
#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/settings/uwp_system_settings.h"

namespace Jadyx {

void UseGameAdvancedGraphics(BootConfig& config, u64 program_id) {
    const GameGraphics game = program_id != 0 ? LoadGameGraphics(program_id) : GameGraphics{};
    config.async_record = ResolveGraphics(LoadGraphicsSettings(), game).async_record;
    if (program_id == 0) return;
    const GameAdvancedGraphics& advanced = game.advanced;
    config.anisotropy = advanced.anisotropy;
    config.astc_drop_mip0 = !advanced.full_textures;
    if (advanced.texture_cap_mib != 0) config.texture_cap_mib = advanced.texture_cap_mib;
    config.gpu_accuracy = advanced.gpu_accuracy;
    config.pso_workers = advanced.shader_workers;
    config.console_mode = advanced.console_mode;
}

void ParseBootConfigFile(const std::string& install_path, BootConfig& config) {
        // Memory keys: their own chain, ahead of the one below, which is as deep as
        // MSVC lets an else-if chain nest.
        const auto parse_memory_key = [&config](const std::string& line) {
            if (line == "small_textures=1" || line == "small_textures=0") {
                config.small_textures = line == "small_textures=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "spare_heaps=1" || line == "spare_heaps=0") {
                config.spare_heaps = line == "spare_heaps=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "cpu_texture_writes=1" ||
                       line == "cpu_texture_writes=0") {
                config.cpu_texture_writes = line == "cpu_texture_writes=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "cached_staging=1" || line == "cached_staging=0") {
                config.cached_staging = line == "cached_staging=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line.starts_with("heap_compaction_mib=")) {
                const auto value = line.substr(20);
                unsigned long parsed{};
                const auto result = std::from_chars(
                    value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} &&
                    result.ptr == value.data() + value.size() && parsed <= 1024) {
                    config.heap_compaction_mib = static_cast<u32>(parsed);
                    WriteDiag("boot.cfg: heap compaction " + std::to_string(parsed) +
                              " MiB a frame");
                } else {
                    WriteDiag("boot.cfg: invalid heap_compaction_mib, ignored");
                }
            } else if (line == "gpu_unswizzle=1" || line == "gpu_unswizzle=0") {
                config.gpu_unswizzle = line == "gpu_unswizzle=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "guest_swap=1" || line == "guest_swap=0") {
                config.guest_swap = line == "guest_swap=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "memory_coordinator=1" ||
                       line == "memory_coordinator=0") {
                config.memory_coordinator = line == "memory_coordinator=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line.starts_with("memory_coordinator_floor_mib=")) {
                config.memory_coordinator_floor_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 29, nullptr, 10)),
                    256U, 2048U);
                WriteDiag("boot.cfg: memory coordinator floor " +
                          std::to_string(config.memory_coordinator_floor_mib) + " MiB");
            } else if (line.starts_with("guest_swap_low_mib=")) {
                config.guest_swap_low_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 19, nullptr, 10)),
                    64U, 2048U);
                WriteDiag("boot.cfg: guest swap low " +
                          std::to_string(config.guest_swap_low_mib) + " MiB");
            } else if (line.starts_with("guest_swap_high_mib=")) {
                config.guest_swap_high_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 20, nullptr, 10)),
                    64U, 3072U);
                WriteDiag("boot.cfg: guest swap high " +
                          std::to_string(config.guest_swap_high_mib) + " MiB");
            } else if (line.starts_with("guest_swap_watch_mib=")) {
                config.guest_swap_watch_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 21, nullptr, 10)),
                    64U, 4096U);
                WriteDiag("boot.cfg: guest swap watch " +
                          std::to_string(config.guest_swap_watch_mib) + " MiB");
            } else if (line.starts_with("guest_swap_idle_ms=")) {
                config.guest_swap_idle_ms = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 19, nullptr, 10)),
                    0U, 120000U);
                WriteDiag("boot.cfg: guest swap idle " +
                          std::to_string(config.guest_swap_idle_ms) + " ms");
            } else if (line.starts_with("guest_swap_probation_ms=")) {
                config.guest_swap_probation_ms = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 24, nullptr, 10)),
                    50U, 60000U);
                WriteDiag("boot.cfg: guest swap probation " +
                          std::to_string(config.guest_swap_probation_ms) + " ms");
            } else if (line.starts_with("guest_swap_batch_mib=")) {
                config.guest_swap_batch_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 21, nullptr, 10)),
                    0U, 2048U);
                WriteDiag("boot.cfg: guest swap batch " +
                          std::to_string(config.guest_swap_batch_mib) + " MiB");
            } else if (line.starts_with("guest_swap_reserve_mib=")) {
                config.guest_swap_reserve_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 23, nullptr, 10)),
                    0U, 256U);
                WriteDiag("boot.cfg: guest swap reserve " +
                          std::to_string(config.guest_swap_reserve_mib) + " MiB");
            } else if (line.starts_with("guest_swap_fill_pages=")) {
                config.guest_swap_fill_pages = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 22, nullptr, 10)),
                    1U, 16U);
                WriteDiag("boot.cfg: guest swap fill " +
                          std::to_string(config.guest_swap_fill_pages) + " pages");
            } else if (line == "guest_swap_prefetch=1" || line == "guest_swap_prefetch=0") {
                config.guest_swap_prefetch = line == "guest_swap_prefetch=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "guest_swap_pause_fill=1" ||
                       line == "guest_swap_pause_fill=0") {
                config.guest_swap_pause_fill = line == "guest_swap_pause_fill=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line.starts_with("oom_reserve_mib=")) {
                config.oom_reserve_mib = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 16, nullptr, 10)),
                    0U, 256U);
                WriteDiag("boot.cfg: driver memory reserve " +
                          std::to_string(config.oom_reserve_mib) + " MiB");
            } else {
                WriteDiag("boot.cfg: IGNORED unknown memory key " + line);
            }
        };
        const auto parse_pipeline_key = [&config](const std::string& line) {
            if (line == "async_pipelines=off") {
                config.async_pipelines = D3D12::AsyncPipelines::Off;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "async_pipelines=balanced") {
                config.async_pipelines = D3D12::AsyncPipelines::Balanced;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "async_pipelines=full") {
                config.async_pipelines = D3D12::AsyncPipelines::Full;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "pso_blobs=1" || line == "pso_blobs=0") {
                config.pso_blobs = line == "pso_blobs=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line.starts_with("wait_small_draws=")) {
                config.wait_small_draws = std::min(
                    static_cast<u32>(std::strtoul(line.c_str() + 17, nullptr, 10)),
                    1024U);
                WriteDiag("boot.cfg: draws of " +
                          std::to_string(config.wait_small_draws) +
                          " vertices or fewer wait for their pipeline");
            } else if (line.starts_with("pso_workers=")) {
                config.pso_workers = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 12, nullptr, 10)),
                    1U, 8U);
                WriteDiag("boot.cfg: " + std::to_string(config.pso_workers) +
                          " pipeline build workers");
            } else if (line.starts_with("pso_boot_workers=")) {
                config.pso_boot_workers = std::clamp(
                    static_cast<u32>(std::strtoul(line.c_str() + 17, nullptr, 10)),
                    1U, 12U);
                WriteDiag("boot.cfg: " + std::to_string(config.pso_boot_workers) +
                          " loading screen pipeline workers");
            } else {
                WriteDiag("boot.cfg: IGNORED unknown pipeline key " + line);
            }
        };
        // GPU queue and rendering switches (a lambda of their own: the main chain
        // is at the compiler's nesting limit).
        const auto parse_render_key = [&config](const std::string& line) {
            // "name=0" or "name=1".
            const auto set_switch = [&line](std::string_view name, bool& value,
                                            std::string_view what) {
                if (line.size() != name.size() + 2 || !line.starts_with(name) ||
                    line[name.size()] != '=' ||
                    (line.back() != '0' && line.back() != '1')) {
                    return false;
                }
                value = line.back() == '1';
                WriteDiag("boot.cfg: " + std::string(what) + (value ? " on" : " off"));
                return true;
            };
            if (set_switch("present_queue", config.present_queue,
                           "present queue of its own") ||
                set_switch("depth_bias_round", config.depth_bias_round,
                           "depth bias rounding") ||
                set_switch("unorm_round", config.unorm_round,
                           "UNORM8 outputs rounded ties down") ||
                set_switch("cube_sanitize", config.cube_sanitize,
                           "cube directions sanitized") ||
                set_switch("half_ftz", config.half_ftz, "fp16 denormal flush") ||
                set_switch("a2c_shader", config.a2c_shader,
                           "alpha to coverage in the shader") ||
                set_switch("small_bc_palette", config.small_bc_palette,
                           "small BC1-3 with the guest palette") ||
                set_switch("bc_gpu_decode", config.bc_gpu_decode,
                           "decoded BC textures on the GPU") ||
                set_switch("not_zeroed_heaps", config.not_zeroed_heaps,
                           "heaps created without zeroing") ||
                set_switch("depth_bias_shader", config.depth_bias_shader,
                           "depth bias in the shader") ||
                set_switch("memory_import_probe", config.memory_import_probe,
                           "memory import probe")) {
                return;
            }
            // "<name>=0|1|check": a mechanism off, on, or off with what it would do
            // counted.
            const auto set_mode = [&line](std::string_view name,
                                          VideoCore::FramePipeline::Mode& value,
                                          std::string_view what) {
                using VideoCore::FramePipeline::Mode;
                if (!line.starts_with(name) || line.size() <= name.size() ||
                    line[name.size()] != '=') {
                    return false;
                }
                const std::string_view arg = std::string_view(line).substr(name.size() + 1);
                if (arg == "0") {
                    value = Mode::Off;
                } else if (arg == "1") {
                    value = Mode::On;
                } else if (arg == "check") {
                    value = Mode::Check;
                } else {
                    return false;
                }
                WriteDiag("boot.cfg: " + std::string(what) +
                          (value == Mode::On      ? " on"
                           : value == Mode::Check ? " checked"
                                                  : " off"));
                return true;
            };
            if (set_mode("present_mailbox", config.present_mailbox,
                         "present mailbox") ||
                set_mode("vsync_nowait", config.vsync_nowait,
                         "vsync without waiting for the GPU thread") ||
                set_mode("swap_interval_hold", config.swap_interval_hold,
                         "swap interval 2 held against a passing 3") ||
                set_mode("pso_budget", config.pso_budget,
                         "pipeline wait budget per frame") ||
                set_mode("pso_loading", config.pso_loading,
                         "pipeline builds on loading screens") ||
                set_mode("cpu_write_defer", config.cpu_write_defer,
                         "guest CPU writes queued without the cache locks") ||
                set_mode("jit_read_table", config.jit_read_table,
                         "guest loads of GPU memory through a JIT load table")) {
                return;
            }
            if (line.starts_with("frame_gen=")) {
                const std::string value = line.substr(10);
                if (value == "off" || value == "0") {
                    config.frame_gen = D3D12::FrameGenMode::Off;
                } else if (value == "auto" || value == "1") {
                    config.frame_gen = D3D12::FrameGenMode::Auto;
                } else if (value == "30") {
                    config.frame_gen = D3D12::FrameGenMode::Target30;
                } else if (value == "60") {
                    config.frame_gen = D3D12::FrameGenMode::Target60;
                } else {
                    WriteDiag("boot.cfg: invalid frame_gen (off, auto, 30, 60), ignored");
                    return;
                }
                WriteDiag("boot.cfg: frame generation " + value);
            } else if (line == "border_color=exact" || line == "border_color=8bit" ||
                       line == "border_color=bw") {
                config.border_color = line == "border_color=8bit" ? 1
                                      : line == "border_color=bw" ? 2
                                                                  : 0;
                WriteDiag("boot.cfg: sampler border colors " + line.substr(13));
            } else {
                WriteDiag("boot.cfg: IGNORED unknown render key " + line);
            }
        };
        std::ifstream cfg{install_path + "\\boot.cfg"};
        for (std::string line; std::getline(cfg, line);) {
            // Outside the chain below, which is at the compiler's nesting limit.
            if (line.starts_with("guest_clock_scale=")) {
                const std::string_view value = std::string_view{line}.substr(18);
                u32 parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                    parsed >= 50 && parsed <= 100) {
                    config.guest_clock_percent = parsed;
                    WriteDiag("boot.cfg: guest clock at " + std::to_string(parsed) + " %");
                } else {
                    WriteDiag("boot.cfg: invalid guest_clock_scale (50-100), ignored");
                }
                continue;
            }
            if (line.starts_with("jit_prewarm_mib=")) {
                const std::string_view value = std::string_view{line}.substr(16);
                u32 parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                    parsed >= 8 && parsed <= 192) {
                    config.jit_prewarm_mib = parsed;
                    WriteDiag("boot.cfg: JIT prewarm fixed at " + std::to_string(parsed) +
                              " MiB per core");
                } else {
                    WriteDiag("boot.cfg: invalid jit_prewarm_mib (8-192), ignored");
                }
                continue;
            }
            if (line == "uniform_reuse=0" || line == "uniform_reuse=1") {
                config.uniform_reuse = line.back() == '1';
                WriteDiag(std::string("boot.cfg: unchanged streamed uniform reuse ") +
                          (config.uniform_reuse ? "on" : "off"));
                continue;
            }
            if (line == "cpu_pin=off" || line == "cpu_pin=cores" || line == "cpu_pin=split") {
                using Common::Windows::CpuPin;
                const auto value = line.substr(8);
                config.cpu_pin = value == "cores"   ? CpuPin::Cores
                                 : value == "split" ? CpuPin::Split
                                                    : CpuPin::Off;
                WriteDiag("boot.cfg: cpu_pin " + value);
                continue;
            }
            if (line == "cpu_pin=topology" || line.starts_with("cpu_pin=map:")) {
                const std::string_view map = line == "cpu_pin=topology" ?
                    Common::Windows::CpuPinTopology : std::string_view{line}.substr(12);
                if (Common::Windows::SetCpuPinMap(map)) {
                    config.cpu_pin = Common::Windows::CpuPin::Map;
                    WriteDiag("boot.cfg: cpu_pin map " + std::string(map));
                } else {
                    WriteDiag("boot.cfg: cpu_pin map does not parse, ignored: " + line);
                }
                continue;
            }
            if (line.starts_with("guest_yield_pause=")) {
                Kernel::KScheduler::SetEmptyYieldPause(static_cast<u32>(
                    std::strtoul(line.c_str() + 18, nullptr, 10)));
                WriteDiag("boot.cfg: " + line);
                continue;
            }
            if (line == "rt_early_out=0") {
                config.rt_early_out = false;
                WriteDiag("boot.cfg: attachments prepared again on every draw");
                continue;
            }
            if (line == "descriptor_defer=0" || line == "descriptor_defer=1") {
                config.descriptor_defer = line.back() == '1';
                WriteDiag(std::string("boot.cfg: draw descriptor tables written on the ") +
                          (config.descriptor_defer ? "recording" : "GPU") + " thread");
                continue;
            }
            if (line == "dma_flush_filter=0" || line == "dma_flush_filter=1") {
                config.dma_flush_filter = line.back() == '1';
                WriteDiag(std::string("boot.cfg: pushbuffer flush filter ") +
                          (config.dma_flush_filter ? "on" : "off"));
                continue;
            }
            if (line == "texture_replay=0" || line == "texture_replay=1" ||
                line == "texture_replay=check") {
                config.texture_replay = line == "texture_replay=1"       ? 1
                                        : line == "texture_replay=check" ? 2
                                                                         : 0;
                WriteDiag(std::string("boot.cfg: texture descriptor replay ") +
                          (config.texture_replay == 1   ? "on"
                           : config.texture_replay == 2 ? "checked"
                                                        : "off"));
                continue;
            }
            if (line == "stage_view_cache=0" || line == "stage_view_cache=1") {
                config.stage_view_cache = line.back() == '1';
                WriteDiag(std::string("boot.cfg: stage view and sampler reuse ") +
                          (config.stage_view_cache ? "on" : "off"));
                continue;
            }
            if (line.starts_with("submit_draws=")) {
                const std::string_view value = std::string_view{line}.substr(13);
                u32 parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                    parsed >= 128 && parsed <= 4096) {
                    D3D12::SetSubmitDraws(parsed);
                    WriteDiag("boot.cfg: command list submitted every " +
                              std::to_string(parsed) + " draws");
                } else {
                    WriteDiag("boot.cfg: invalid submit_draws (128-4096), ignored");
                }
                continue;
            }
            if (line.starts_with("present_queue=") ||
                line.starts_with("border_color=") ||
                line.starts_with("depth_bias_round=") ||
                line.starts_with("unorm_round=") ||
                line.starts_with("cube_sanitize=") || line.starts_with("half_ftz=") ||
                line.starts_with("a2c_shader=") ||
                line.starts_with("small_bc_palette=") ||
                line.starts_with("bc_gpu_decode=") ||
                line.starts_with("not_zeroed_heaps=") ||
                line.starts_with("depth_bias_shader=") ||
                line.starts_with("frame_gen=") ||
                line.starts_with("memory_import_probe=") ||
                line.starts_with("present_mailbox=") ||
                line.starts_with("vsync_nowait=") ||
                line.starts_with("swap_interval_hold=") ||
                line.starts_with("pso_budget=") ||
                line.starts_with("pso_loading=") ||
                line.starts_with("cpu_write_defer=") ||
                line.starts_with("jit_read_table=")) {
                parse_render_key(line);
                continue;
            }
            if (line.starts_with("async_pipelines=") ||
                line.starts_with("wait_small_draws=") ||
                line.starts_with("pso_workers=") ||
                line.starts_with("pso_boot_workers=") ||
                line.starts_with("pso_blobs=")) {
                parse_pipeline_key(line);
                continue;
            }
            if (line.starts_with("guest_swap") || line.starts_with("small_textures=") ||
                line.starts_with("memory_coordinator") ||
                line.starts_with("oom_reserve_mib=") ||
                line.starts_with("spare_heaps=") ||
                line.starts_with("gpu_unswizzle=") ||
                line.starts_with("cached_staging=") ||
                line.starts_with("cpu_texture_writes=") ||
                line.starts_with("heap_compaction_mib=")) {
                parse_memory_key(line);
                continue;
            }
            constexpr std::string_view key = "run_seconds=";
            constexpr std::string_view hunt_at_key = "hunt_at=";
            if (line.starts_with(hunt_at_key)) {
                config.hunt_at = static_cast<u32>(
                    std::strtoul(line.c_str() + hunt_at_key.size(), nullptr, 10));
                WriteDiag("boot.cfg: bottleneck capture at " +
                          std::to_string(config.hunt_at) + " s");
            } else if (line.starts_with("hunt_cs=")) {
                config.hunt_cs = std::strtoull(line.c_str() + 8, nullptr, 16);
                WriteDiag("boot.cfg: bottleneck capture when compute shader " +
                          line.substr(8) + " is warm");
            } else if (line.starts_with("cs_inputs=")) {
                config.cs_inputs = std::strtoull(line.c_str() + 10, nullptr, 16);
                WriteDiag("boot.cfg: storage buffers of compute shader " +
                          line.substr(10) + " dumped");
            } else if (line.starts_with("frame_trace_")) {
                // One branch for both keys: MSVC caps how deep this chain nests.
                if (line.starts_with("frame_trace_at=")) {
                    config.frame_trace_at = static_cast<u32>(
                        std::strtoul(line.c_str() + 15, nullptr, 10));
                    WriteDiag("boot.cfg: frame chain timeline at " +
                              std::to_string(config.frame_trace_at) + " s");
                } else if (line.starts_with("frame_trace_on_drop=")) {
                    config.frame_trace_on_drop = static_cast<u32>(
                        std::strtoul(line.c_str() + 20, nullptr, 10));
                    WriteDiag("boot.cfg: frame chain timelines on swap interval "
                              "rise: " + std::to_string(config.frame_trace_on_drop));
                }
            } else if (line.starts_with("quit_at=")) {
                config.quit_at =
                    static_cast<u32>(std::strtoul(line.c_str() + 8, nullptr, 10));
                WriteDiag("boot.cfg: session closes at " +
                          std::to_string(config.quit_at) + " s");
            } else if (line.starts_with(key)) {
                config.run_seconds =
                    static_cast<u32>(std::strtoul(line.c_str() + key.size(), nullptr, 10));
                WriteDiag("boot.cfg: run " + std::to_string(config.run_seconds) + " s");
            } else if (line == "descriptor_checks=1") {
                config.descriptor_checks = true;
                WriteDiag("boot.cfg: device removal checks after every draw descriptor");
            } else if (line == "dred=1") {
                config.dred = true;
                WriteDiag("boot.cfg: DRED breadcrumbs and page-fault tracking enabled");
            } else if (line == "gpu_breadcrumbs=1") {
                config.dred = true;
                config.gpu_breadcrumbs = true;
                WriteDiag("boot.cfg: GPU breadcrumbs around every command (and DRED)");
            } else if (line == "close_probe=1") {
                config.close_probe = true;
                WriteDiag("boot.cfg: every command also closed alone on a scratch list");
            } else if (line == "jit_dual_map=0") {
                config.jit_dual_map = false;
                WriteDiag("boot.cfg: JIT code memory keeps paged W^X");
            } else if (line == "pso_eviction=0") {
                config.pso_eviction = false;
                WriteDiag("boot.cfg: idle pipelines keep their PSOs");
            } else if (line == "xfb=storage") {
                config.xfb_storage = true;
                WriteDiag("boot.cfg: transform feedback through storage buffers");
            } else if (line == "indirect_macro_cpu=1") {
                config.indirect_macro_cpu = true;
                WriteDiag("boot.cfg: indirect draw macros read GPU-written arguments on the CPU");
            } else if (line == "occlusion_force_visible=1") {
                config.occlusion_force_visible = true;
                WriteDiag("boot.cfg: occlusion results reach the game as never zero");
            } else if (line == "uav_sync=1") {
                config.uav_sync = true;
                WriteDiag("boot.cfg: UAV barrier after every draw and dispatch");
            } else if (line == "rt_reclaim=0") {
                config.reclaim_superseded_images = false;
                WriteDiag("boot.cfg: superseded render targets wait for the GC");
            } else if (line == "mip_rt=0") {
                config.mip_render_targets = false;
                WriteDiag("boot.cfg: depth targets at a mip level use aliased images");
            } else if (line == "rt_inherit=0" || line == "rt_inherit=1") {
                config.render_target_inheritance = line.back() == '1';
                WriteDiag(std::string("boot.cfg: render targets at a new size ") +
                          (config.render_target_inheritance
                               ? "inherit the previous size on the GPU"
                               : "upload guest memory"));
            } else if (line == "min_max_filter=force") {
                config.force_min_max_filter = true;
                WriteDiag("boot.cfg: filtered min/max sampler reduction forced on");
            } else if (line == "min_max_filter=off") {
                config.skip_min_max_filter = true;
                WriteDiag("boot.cfg: filtered min/max sampler reduction not emulated");
            } else if (line == "pulse_resume=0") {
                config.pulse_resume = false;
                WriteDiag("boot.cfg: Pulse sessions are not resumed");
            } else if (line == "min_max_filter=emulate") {
                config.emulate_min_max_filter = true;
                WriteDiag("boot.cfg: filtered min/max sampler reduction always emulated");
            } else if (line == "airplane=1") {
                config.airplane_mode = true;
                WriteDiag("boot.cfg: airplane mode, the game sees no internet");
            } else if (line == "pso_prewarm=0") {
                config.pso_prewarm = false;
                WriteDiag("boot.cfg: dormant PSOs wait for their first draw");
            } else if (line == "gpu_capture_self_test=1") {
                config.full_gpu_capture = true;
                config.full_gpu_capture_self_test = true;
                WriteDiag("boot.cfg: full capture synthetic GPU gate enabled");
            } else if (line == "gpu_capture_full=1") {
                config.full_gpu_capture = true;
                config.material_capture_menu = true;
                WriteDiag("boot.cfg: full synchronous GPU capture enabled (menu)");
            } else if (line == "gpu_capture_targets=after") {
                config.full_gpu_capture_targets_after = true;
                WriteDiag("boot.cfg: full capture reads render targets after operations only");
            } else if (line == "buffer_write_origins=1") {
                config.buffer_write_origins = true;
                WriteDiag("boot.cfg: buffer GPU-write origins traced");
            } else if (line == "dump_guest_shaders=1") {
                config.dump_guest_shaders = true;
                WriteDiag("boot.cfg: guest shader dump enabled");
            } else if (line == "gpu_profile=1") {
                config.gpu_profile = true;
                WriteDiag("boot.cfg: detailed D3D12 GPU-thread profiling enabled");
            } else if (line == "upload_copy=0" || line == "upload_copy=1") {
                config.upload_copy = line.back() == '1';
                WriteDiag(std::string("boot.cfg: short uniform write-combined copy ") +
                          (config.upload_copy ? "on" : "off"));
            } else if (line == "draw_sampler_reuse=0" || line == "draw_sampler_reuse=1") {
                config.draw_sampler_reuse = line.back() == '1';
                WriteDiag(std::string("boot.cfg: draw-local sampler resolution reuse ") +
                          (config.draw_sampler_reuse ? "on" : "off"));
            } else if (line == "ps_scratch=auto" || line == "ps_scratch=uav" ||
                       line == "ps_scratch=native") {
                config.ps_scratch = line == "ps_scratch=uav"      ? 1
                                    : line == "ps_scratch=native" ? 2
                                                                  : 0;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "cpu_probe=1") {
                config.cpu_probe = true;
                WriteDiag("boot.cfg: CPU probe before the guest");
            } else if (line == "timer_profile=1") {
                config.timer_profile = true;
                WriteDiag("boot.cfg: HostTiming requested waits/sleeps/spin elapsed profiling enabled");
            } else if (line == "cpu_profile=1") {
                config.cpu_profile = true;
            } else if (line == "profile=1") {
                config.profile = true;
                WriteDiag("boot.cfg: sampling profiler on");
            } else if (line == "profile=full") {
                // The fine timers read the clock about 15 times a draw: they slow the
                // GPU thread down, so they are not part of the plain profile.
                config.profile = true;
                config.gpu_profile = true;
                config.cpu_profile = true;
                WriteDiag("boot.cfg: sampling profiler with GPU and CPU timers");
            } else if (line.starts_with("profile_hz=") ||
                       line.starts_with("profile_window=")) {
                const bool hz = line.starts_with("profile_hz=");
                const std::string value = line.substr(line.find('=') + 1);
                const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
                if (hz && parsed >= 10 && parsed <= 1000) {
                    config.profile_hz = static_cast<u32>(parsed);
                } else if (!hz && parsed >= 1 && parsed <= 600) {
                    config.profile_window = static_cast<u32>(parsed);
                } else {
                    WriteDiag("boot.cfg: invalid " + line + ", ignored");
                }
            } else if (line.starts_with("idle_spin=")) {
                if (const auto policy = Common::SpinPolicy::Parse(line.substr(10))) {
                    config.idle_spin = *policy;
                    WriteDiag("boot.cfg: idle cores " + policy->Describe());
                } else {
                    WriteDiag("boot.cfg: invalid idle_spin, ignored");
                }
            } else if (line.starts_with("texture_cap_mib=") ||
                       line.starts_with("texture_cap_line_mib=")) {
                // One branch for both: another else-if passes the compiler's nesting limit.
                const bool cap_line = line.starts_with("texture_cap_line_mib=");
                const auto value = line.substr(cap_line ? 21 : 16);
                unsigned long parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed <= 1048576) {
                    if (cap_line) {
                        config.texture_cap_line_mib = static_cast<u32>(parsed);
                    } else {
                        config.texture_cap_mib = static_cast<u32>(parsed);
                    }
                    WriteDiag(std::string("boot.cfg: texture cap ") + (cap_line ? "line " : "") +
                              std::to_string(parsed) + " MiB");
                } else {
                    WriteDiag(std::string("boot.cfg: invalid ") +
                              (cap_line ? "texture_cap_line_mib" : "texture_cap_mib") + ", ignored");
                }
            } else if (line.starts_with("memory_limit_mib=")) {
                const auto value = line.substr(17);
                unsigned long parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed <= 1048576) {
                    config.memory_limit_mib = static_cast<u32>(parsed);
                } else {
                    WriteDiag("boot.cfg: invalid memory_limit_mib, ignored");
                }
            } else if (line == "pulse_store=1") {
                config.pulse_store = true;
                WriteDiag("boot.cfg: Pulse store gate");
            } else if (line.starts_with("pulse_cycles=") ||
                       line.starts_with("pulse_seed=")) {
                const bool cycles = line.starts_with("pulse_cycles=");
                const std::string_view value =
                    std::string_view{line}.substr(line.find('=') + 1);
                u32 parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                    (cycles && parsed > 100000)) {
                    WriteDiag("boot.cfg: invalid " + line + ", ignored");
                } else if (cycles) {
                    config.pulse_cycles = parsed;
                    WriteDiag("boot.cfg: Pulse gate, " + std::to_string(parsed) + " cycles");
                } else {
                    config.pulse_seed = parsed;
                }
            } else if (line.starts_with("jit_cache_mib=")) {
                const std::string_view value = std::string_view{line}.substr(14);
                u32 parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                    parsed >= 32 && parsed <= 512) {
                    config.jit_cache_mib = parsed;
                } else {
                    WriteDiag("boot.cfg: invalid jit_cache_mib (32-512), ignored");
                }
            } else if (line == "jit_prewarm=record") {
                config.jit_prewarm = BootConfig::JitPrewarm::Record;
            } else if (line == "jit_prewarm=1") {
                config.jit_prewarm = BootConfig::JitPrewarm::Warm;
            } else if (line == "jit_prewarm=0") {
                config.jit_prewarm = BootConfig::JitPrewarm::Off;
                WriteDiag("boot.cfg: JIT prewarm off");
            } else if (line == "audio_profile=1") {
                config.audio_profile = true;
                WriteDiag("boot.cfg: XAudio2 profiling enabled");
            } else if (line == "audio=null") {
                config.null_audio = true;
                WriteDiag("boot.cfg: timed Null audio");
            } else if (line == "audio=xaudio2") {
                config.null_audio = false;
                WriteDiag("boot.cfg: XAudio2 audio");
            } else if (line.starts_with("audio=")) {
                config.null_audio = false;
                WriteDiag("boot.cfg: unknown audio backend '" + line.substr(6) +
                          "', using XAudio2");
            } else if (line.starts_with("upscaling=")) {
                config.upscaling =
                    ParseUpscaling(std::string_view{line}.substr(10));
                WriteDiag(config.upscaling
                              ? "boot.cfg: image upscaling " + line.substr(10)
                              : "boot.cfg: unknown upscaling '" + line.substr(10) +
                                    "', using the saved one");
            } else if (line == "async_present=0" || line == "async_present=1") {
                config.async_present = line.back() == '1';
                WriteDiag(std::string("boot.cfg: presentation thread ") +
                          (config.async_present ? "on" : "off"));
            } else if (line == "nv_event_stall=0" || line == "nv_event_stall=1") {
                config.nv_event_stall = line.back() == '1';
                WriteDiag(std::string("boot.cfg: failing NV events ") +
                          (config.nv_event_stall ? "stall the guest" : "flush the GPU"));
            } else if (line == "nv_release_fence=0" || line == "nv_release_fence=1") {
                config.nv_release_fence = line.back() == '1';
                WriteDiag(std::string("boot.cfg: display release fence ") +
                          (config.nv_release_fence ? "on" : "off"));
            } else if (line == "gpu_fence=immediate" || line == "gpu_fence=safe" ||
                       line == "gpu_fence=wait" || line == "gpu_fence=balanced" ||
                       line == "gpu_fence=accurate") {
                const std::string value = line.substr(10);
                config.gpu_fence = value == "immediate"  ? GpuFence::Immediate
                                   : value == "safe"     ? GpuFence::Safe
                                   : value == "wait"     ? GpuFence::Wait
                                   : value == "balanced" ? GpuFence::Balanced
                                                         : GpuFence::Accurate;
                WriteDiag("boot.cfg: GPU fence " + value);
            } else if (line == "async_record=0" || line == "async_record=1") {
                config.async_record = line.back() == '1';
                WriteDiag(std::string("boot.cfg: command list recording thread ") +
                          (config.async_record ? "on" : "off"));
            } else if (line == "pipeline_key_reuse=0" ||
                       line == "pipeline_key_reuse=1" ||
                       line == "pipeline_key_reuse=check") {
                using D3D12::PipelineKeyTracking::Mode;
                const std::string value = line.substr(19);
                config.pipeline_key_reuse = value == "check" ? Mode::Check
                                            : value == "1"   ? Mode::On
                                                             : Mode::Off;
                WriteDiag("boot.cfg: pipeline key reuse " +
                          std::string(value == "check" ? "checked"
                                      : value == "1"   ? "on"
                                                       : "off"));
            } else if (line == "timer_spin=auto") {
                config.timer_spin_us.reset();
                WriteDiag("boot.cfg: host timing follows the timer's lateness");
            } else if (line == "inline_exclusives=0" || line == "inline_exclusives=1") {
                config.inline_exclusives = line.back() == '1';
                WriteDiag(std::string("boot.cfg: exclusive accesses ") +
                          (config.inline_exclusives ? "inline" : "through the monitor"));
            } else if (line.starts_with("timer_spin=")) {
                const auto value = line.substr(11);
                u32 parsed{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed <= 100000) {
                    config.timer_spin_us = parsed;
                    WriteDiag("boot.cfg: host timing waits at most " + value +
                              " us on the CPU");
                } else {
                    WriteDiag("boot.cfg: invalid timer_spin, ignored");
                }
            } else if (line == "timer_sleep=0" || line == "timer_sleep=1") {
                config.timer_sleep = line.back() == '1';
                WriteDiag(std::string("boot.cfg: host timing sleeps on ") +
                          (config.timer_sleep ? "a high-resolution timer"
                                              : "timer ticks"));
            } else if (line == "force_swap_interval=1") {
                config.force_swap_interval_one = true;
                WriteDiag("boot.cfg: forcing guest swap intervals above one to one");
            } else if (line == "debug_layer=1" || line == "debug_layer=gbv") {
                config.debug_layer = true;
                config.gpu_validation = line == "debug_layer=gbv";
                WriteDiag(config.gpu_validation
                              ? "boot.cfg: D3D12 debug layer with GPU-based validation"
                              : "boot.cfg: D3D12 debug layer");
            } else if (line == "material_gpu_probe=1") {
                config.material_gpu_probe = true;
                WriteDiag("boot.cfg: isolated material GPU intermediate probe enabled");
            } else if (line == "hunt=1") {
                config.hunt_menu = true;
                WriteDiag("boot.cfg: controller bottleneck capture menu enabled");
            } else if (line == "material_capture_menu=1") {
                config.material_capture_menu = true;
                WriteDiag("boot.cfg: controller graphics capture menu enabled");
            } else if (line == "bug_tracker=0") {
                config.bug_tracker = false;
                WriteDiag("boot.cfg: bug tracker off");
            } else if (line.starts_with("log_filter=")) {
                config.log_filter = line.substr(11);
                WriteDiag("boot.cfg: log filter " + config.log_filter);
            } else if (line.starts_with("dump_shader=")) {
                const char* cursor = line.c_str() + 12;
                for (char* end{}; *cursor != '\0'; cursor = *end == ',' ? end + 1 : end) {
                    const u64 hash = std::strtoull(cursor, &end, 16);
                    if (end == cursor) {
                        break;
                    }
                    D3D12::SetDumpedShader(hash);
                }
                WriteDiag("boot.cfg: dumping shader " + line.substr(12));
            } else if (line.starts_with("cs_split=")) {
                const u32 factor = static_cast<u32>(
                    std::strtoul(line.c_str() + 9, nullptr, 10));
                // 16 by default (experimental); cs_split=1 turns the split off.
                if (factor >= 1 && factor <= 16 && std::has_single_bit(factor)) {
                    D3D12::SetComputeWorkgroupSplit(factor);
                    WriteDiag(factor == 1
                                  ? std::string("boot.cfg: compute dispatches not split")
                                  : "boot.cfg: small compute dispatches split by " +
                                        std::to_string(factor));
                } else {
                    WriteDiag("boot.cfg: cs_split wants 1, 2, 4, 8 or 16; ignored");
                }
            } else if (line.starts_with("skip_shader=")) {
                const u64 hash = std::strtoull(line.c_str() + 12, nullptr, 16);
                if (hash != 0) {
                    D3D12::SetSkippedShader(hash);
                    WriteDiag("boot.cfg: skipping draws with shader " + line.substr(12));
                }
            } else if (line.starts_with("drop_gs=")) {
                const u64 hash = std::strtoull(line.c_str() + 8, nullptr, 16);
                if (hash != 0) {
                    D3D12::SetGeometryDroppedShader(hash);
                    WriteDiag("boot.cfg: building VS " + line.substr(8) +
                              " without its geometry shader");
                }
            } else if (line.starts_with("material_capture_frame=")) {
                config.material_capture_frame = static_cast<u32>(
                    std::strtoul(line.c_str() + 23, nullptr, 10));
            } else if (line.starts_with("material_snapshot_vertices0=")) {
                config.material_snapshot_vertices[0] = static_cast<u32>(std::strtoul(line.c_str() + 28, nullptr, 10));
            } else if (line.starts_with("material_snapshot_vertices1=")) {
                config.material_snapshot_vertices[1] = static_cast<u32>(std::strtoul(line.c_str() + 28, nullptr, 10));
            } else if (line.starts_with("material_snapshot_ps0=")) {
                config.material_snapshot_ps[0] = std::strtoull(line.c_str() + 22, nullptr, 16);
            } else if (line.starts_with("material_snapshot_ps1=")) {
                config.material_snapshot_ps[1] = std::strtoull(line.c_str() + 22, nullptr, 16);
            } else if (line.starts_with("material_probe_x=")) {
                config.material_probe_x = static_cast<s32>(std::strtol(line.c_str() + 17, nullptr, 10));
            } else if (line.starts_with("material_probe_y=")) {
                config.material_probe_y = static_cast<s32>(std::strtol(line.c_str() + 17, nullptr, 10));
            } else if (line.starts_with("trace_frames=")) {
                config.traced_frames = std::max<u32>(
                    1, static_cast<u32>(std::strtoul(line.c_str() + 13, nullptr, 10)));
                WriteDiag("boot.cfg: trace spans " + std::to_string(config.traced_frames) +
                          " frames");
            } else if (line.starts_with("trace_frame=")) {
                config.traced_frame =
                    static_cast<u32>(std::strtoul(line.c_str() + 12, nullptr, 10));
                WriteDiag("boot.cfg: trace frame " + std::to_string(config.traced_frame));
            } else if (line == "bc_arrays=native") {
                config.bc_arrays_native = true;
                WriteDiag("boot.cfg: D3D12 block-compressed arrays stay compressed");
            } else if (line == "astc=gpu" || line == "astc=gpu-rgba" ||
                       line == "astc=cpu" || line == "astc=bc3") {
                using Astc = decltype(config.astc);
                config.astc = line == "astc=gpu"        ? Astc::Gpu
                              : line == "astc=gpu-rgba" ? Astc::GpuRgba
                              : line == "astc=cpu"      ? Astc::Cpu
                                                       : Astc::Bc3;
                WriteDiag("boot.cfg: ASTC " + line.substr(5));
            } else if (line == "astc_arrays=bc3" || line == "astc_arrays=rgba") {
                config.astc_arrays_rgba = line == "astc_arrays=rgba";
                WriteDiag("boot.cfg: ASTC arrays " + line.substr(12));
            } else if (line == "astc_opaque=bc1" || line == "astc_opaque=bc3") {
                config.astc_opaque_bc3 = line == "astc_opaque=bc3";
                WriteDiag("boot.cfg: opaque ASTC " + line.substr(12));
            } else if (line == "astc_mip0=drop" || line == "astc_mip0=keep") {
                config.astc_drop_mip0 = line == "astc_mip0=drop";
                WriteDiag("boot.cfg: ASTC top level " + line.substr(10));
            } else if (line == "astc_fresh=1") {
                config.astc_fresh = true;
                WriteDiag("boot.cfg: new ASTC scratch resources for every GPU upload");
            } else if (line == "astc_sync=1") {
                config.astc_sync = true;
                WriteDiag("boot.cfg: wait for the GPU after every GPU ASTC upload");
            } else if (line == "gpu_failure_probe=removed") {
                config.gpu_failure_probe = true;
                config.gpu_removal_probe = true;
            } else if (line == "gpu_failure_probe=1") {
                config.gpu_failure_probe = true;
            } else if (line == "rom_storage_checks=record") {
                config.rom_storage_checks = 1;
            } else if (line == "file_manager_gate=1") {
                config.file_manager_gate = true;
            } else if (line == "network_import_gate=1") {
                config.network_import_gate = true;
            } else if (line == "log_export_gate=1") {
                config.log_export_gate = true;
            } else if (line == "rom_storage_checks=restore") {
                config.rom_storage_checks = 2;
            } else if (line == "astc_verify=1") {
                config.astc_verify = true;
                WriteDiag("boot.cfg: verify first GPU BC3 upload against CPU");
            } else if (line == "dxil_sign=hash" || line == "dxil_sign=verify") {
                config.dxil_sign = line == "dxil_sign=hash" ? 1 : 2;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "dxil_cache=0" || line == "dxil_cache=1") {
                config.dxil_cache = line == "dxil_cache=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "stencil_blit_verify=1") {
                config.stencil_blit_verify = true;
                WriteDiag("boot.cfg: read back the first depth-stencil blits");
            } else if (line == "fastmem=0" || line == "fastmem=1" ||
                       line == "fastmem=hybrid" || line == "fastmem=full") {
                using Fastmem = decltype(config.fastmem);
                config.fastmem = line == "fastmem=0"      ? Fastmem::Off
                                 : line == "fastmem=full" ? Fastmem::Full
                                 : line == "fastmem=hybrid" ? Fastmem::Hybrid
                                                           : Fastmem::Auto;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "cpu_accuracy=accurate" || line == "cpu_accuracy=auto" ||
                       line == "cpu_accuracy=unsafe") {
                config.cpu_accuracy = line == "cpu_accuracy=accurate"
                    ? Settings::CpuAccuracy::Accurate
                    : line == "cpu_accuracy=unsafe" ? Settings::CpuAccuracy::Unsafe
                                                    : Settings::CpuAccuracy::Auto;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "gpu_accuracy=low" || line == "gpu_accuracy=high") {
                config.gpu_accuracy = line == "gpu_accuracy=low"
                    ? Settings::GpuAccuracy::Low : Settings::GpuAccuracy::High;
                WriteDiag("boot.cfg: " + line);
            } else if (line == "dma_accuracy=default" || line == "dma_accuracy=unsafe" ||
                       line == "dma_accuracy=safe") {
                config.dma_accuracy = line == "dma_accuracy=unsafe"
                    ? Settings::DmaAccuracy::Unsafe
                    : line == "dma_accuracy=safe" ? Settings::DmaAccuracy::Safe
                                                  : Settings::DmaAccuracy::Default;
                WriteDiag("boot.cfg: " + line);
            } else if (line.starts_with("fastmem_hot_mib=")) {
                const auto requested = static_cast<u32>(
                    std::strtoul(line.c_str() + 16, nullptr, 10));
                config.fastmem_hot_mib = std::clamp(requested, 128U, 448U);
                WriteDiag("boot.cfg: fastmem hot " +
                          std::to_string(config.fastmem_hot_mib) + " MiB");
            } else if (line == "async_shaders=0") {
                config.async_shaders = false;
                WriteDiag("boot.cfg: asynchronous shaders off");
            } else if (line.starts_with("alloc_watch=")) {
                // Byte sizes as the heap report prints them, separated by commas or
                // colons (a PowerShell -File argument splits at commas).
                std::vector<std::size_t> sizes;
                for (std::string_view rest = std::string_view{line}.substr(12);
                     !rest.empty();) {
                    const size_t comma = rest.find_first_of(",:");
                    const std::string_view item = rest.substr(0, comma);
                    std::size_t size{};
                    if (std::from_chars(item.data(), item.data() + item.size(), size)
                            .ec == std::errc{}) {
                        sizes.push_back(size);
                    }
                    rest = comma == std::string_view::npos ? std::string_view{}
                                                           : rest.substr(comma + 1);
                }
                AllocWatch::Watch(sizes);
                WriteDiag("boot.cfg: watching " + std::to_string(sizes.size()) +
                          " allocation sizes");
            } else if (line == "pso_probe=1") {
                D3D12::SetPsoCostProbe(true);
                WriteDiag("boot.cfg: measuring the driver's commit per graphics PSO");
            } else if (line == "host_trim=0") {
                config.host_trim = false;
                WriteDiag("boot.cfg: host_trim off");
            } else if (line == "pool_trim=0") {
                config.pool_trim = false;
                WriteDiag("boot.cfg: pool_trim off");
            } else if (line == "memory_audit=1" || line == "memory_audit=0") {
                config.memory_audit = line == "memory_audit=1";
                WriteDiag("boot.cfg: " + line);
            } else if (line == "pulse_diag=1") {
                Common::Pulse::SetDiagnostics(true);
                WriteDiag("boot.cfg: Pulse diagnostics on (GPU VA, shader code, page watch)");
            } else if (line == "play=1") {
                config.play = true;
                WriteDiag("boot.cfg: played by hand, no time limit or frame dumps");
            } else if (line == "renderer=null") {
                config.null_renderer = true;
                WriteDiag("boot.cfg: Null renderer");
            } else if (line.starts_with("input=")) {
                if (auto step = ParseInputStep(line.substr(6))) {
                    config.input_script.push_back(std::move(*step));
                    WriteDiag("boot.cfg: input " + line.substr(6));
                } else {
                    WriteDiag("boot.cfg: IGNORED bad input line " + line.substr(6));
                }
            } else if (line.starts_with("game=")) {
                config.game = line.substr(5);
                WriteDiag("boot.cfg: game " + config.game);
            }
        }
}

} // namespace Jadyx
