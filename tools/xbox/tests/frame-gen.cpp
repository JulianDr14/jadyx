// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Frame generation gate.
//
// Pacer (CPU): games at 15, 20, 24 and 30 fps with arrival jitter, stalls, vsync gaps and mode
// changes. The content time shown on every output slot must advance evenly (no step back, no held
// frame once settled) and the output rate must follow the mode.
//
// GPU (WARP, debug layer): synthetic frames with a textured background and an object moving over
// it, at 720p (motion at 1/2) and 4K (motion at 1/4, the FSR output). The midpoint frame must put
// the object halfway (much closer than a crossfade), a still pair must come out unchanged, a
// scene cut must show the nearer frame, and a virtual-time run of the whole ingest -> Next ->
// output -> Unpin path must move the object by even steps. No debug layer error.
//
// "frame-gen.exe perf": the same passes on the desktop GPU without the debug layer, timed.
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen_pacer.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12sdklayers.h>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace D3D12;
using Kind = FrameGenPacer::Choice::Kind;

namespace {

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

/// The debug layer's queue, kept for the failure report after main's try block unwinds (the
/// queue is the device itself: it stays alive with it).
ComPtr<ID3D12InfoQueue> failure_messages;

bool DebugErrors(ID3D12InfoQueue* messages) {
    if (messages == nullptr) return false;
    bool found = false;
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<u8> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << message->pDescription << '\n';
            found = true;
        }
    }
    return found;
}

/// Deterministic jitter in [0, 1).
double Noise(u64 i) {
    u64 x = i * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 32;
    return static_cast<double>(x & 0xFFFFFF) / static_cast<double>(0x1000000);
}

// ---------------------------------------------------------------------------------------------
// Pacer simulation
// ---------------------------------------------------------------------------------------------

struct PacerRun {
    double fps{};             ///< game rate (vsync steps 60/fps, alternating for 24)
    double jitter{};          ///< arrival jitter, seconds
    FrameGenMode mode{FrameGenMode::Auto};
    double seconds{12.0};
    double stall_at{-1.0};    ///< host stall: arrivals from then on come stall_len later
    double stall_len{0.0};
    double gap_at{-1.0};      ///< guest gap: vsync jumps 30 (half a second of no frames)
    u32 spike_every{};        ///< one frame in this many arrives spike_len late (a host spike)
    double spike_len{};
    bool self_paced{};        ///< no vsync lock: frames at 1/fps, composed on the next vsync
    bool queue_times{};       ///< the pacer gets the times the game queued its frames
};

struct PacerResult {
    u32 target{};
    u32 outputs{};
    u32 backward{};
    u32 holds{};             ///< late: the newest frame shown again (counted after settling)
    u32 blends{};
    double max_step_error{}; ///< seconds, after settling
    double rms_step_error{};
    double delay{}; ///< at the end, seconds
    bool queue_clock{}; ///< at the end
};

PacerResult SimulatePacer(const PacerRun& run, double settle = 3.0,
                          double skip_from = -1.0, double skip_to = -1.0) {
    FrameGenPacer pacer;
    struct Arrival {
        double time;
        u64 vsync;
        double queue; ///< host time the game queued it
        double anim;  ///< the game's own time of the frame: what it animates by
    };
    std::vector<Arrival> arrivals;
    const double host_base = 100.0;
    u64 vsync = 1000;
    double stall_shift = 0.0;
    double self_time = 0.0;
    for (u64 i = 0;; ++i) {
        const double content = static_cast<double>(vsync) / 60.0;
        const double game_time = content - 1000.0 / 60.0;
        if (game_time > run.seconds) break;
        if (run.stall_at >= 0.0 && game_time >= run.stall_at) stall_shift = run.stall_len;
        // Render time varies; arrivals are only ever late.
        double arrival = host_base + content + 0.010 + run.jitter * Noise(i) + stall_shift;
        if (run.spike_every != 0 && i % run.spike_every == run.spike_every / 2) {
            arrival += run.spike_len;
        }
        // Frames come in order: a late one delays the ones behind it.
        if (!arrivals.empty()) arrival = std::max(arrival, arrivals.back().time);
        if (run.self_paced) {
            arrivals.push_back({arrival, vsync, host_base + self_time + 0.002, self_time});
            // Its next frame, 1 ms of spread, shown on the vsync after it is queued.
            self_time += 1.0 / run.fps + 0.001 * (Noise(i + 5000) - 0.5);
            vsync = 1000 + static_cast<u64>(std::ceil((self_time + 0.002) * 60.0));
            continue;
        }
        // Vsync-locked: queued some time before the vsync it is shown on.
        arrivals.push_back({arrival, vsync, host_base + game_time - 0.012 + 0.006 * Noise(i + 9000),
                            game_time});
        u64 step = 0;
        if (run.fps >= 23.5 && run.fps <= 24.5) {
            step = (i % 2 == 0) ? 2 : 3;
        } else {
            step = static_cast<u64>(std::lround(60.0 / run.fps));
        }
        vsync += step;
        if (run.gap_at >= 0.0 && game_time < run.gap_at &&
            static_cast<double>(vsync - 1000) / 60.0 >= run.gap_at) {
            vsync += 30;
        }
    }

    struct Held {
        double content;
        double anim;
        u64 serial;
    };
    std::vector<Held> history; // the last FrameGen::NUM_SLOTS frames
    u64 serial = 1;
    size_t next = 0;
    PacerResult result;
    std::optional<double> last_shown;
    double sum_sq = 0.0;
    u32 measured = 0;
    double slot = arrivals.front().time;
    u32 target = 0;
    const double end = arrivals.back().time;
    while (slot < end) {
        while (next < arrivals.size() && arrivals[next].time <= slot) {
            const Arrival& arrival = arrivals[next];
            target = pacer.OnFrame(run.mode, arrival.vsync, arrival.time,
                                   run.queue_times ? arrival.queue : 0.0);
            if (target != 0) {
                history.push_back({pacer.Content(), arrival.anim, serial++});
                if (history.size() > FrameGen::NUM_SLOTS) history.erase(history.begin());
            } else {
                // Passthrough: FrameGen::Break forgets the history.
                if (!history.empty()) pacer.Reset();
                history.clear();
                ++serial;
            }
            ++next;
        }
        if (target == 0 || history.empty()) {
            last_shown.reset();
            slot += 1.0 / 60.0;
            continue;
        }
        const double period = 1.0 / target;
        std::vector<FrameGenPacer::Frame> frames;
        for (u32 i = 0; i < history.size(); ++i) {
            frames.push_back({.content = history[i].content, .serial = history[i].serial, .slot = i});
        }
        const FrameGenPacer::Choice choice = pacer.Pick(slot, frames);
        // Measured in the game's own time: what the motion on screen follows.
        double shown = 0.0;
        bool hold = false;
        if (choice.kind == Kind::Blend) {
            shown = history[choice.a].anim +
                    choice.phase * (history[choice.b].anim - history[choice.a].anim);
        } else {
            const u32 index = choice.kind == Kind::Real ? choice.a : static_cast<u32>(frames.size() - 1);
            shown = history[index].anim;
            hold = index + 1 == frames.size() && last_shown && shown <= *last_shown + 1e-9;
        }
        const double game_time = slot - host_base - 1000.0 / 60.0;
        const bool settled = game_time > settle &&
                             !(skip_from >= 0.0 && game_time >= skip_from && game_time < skip_to);
        // Content time never goes back, not even through a stall.
        if (game_time > settle && last_shown && shown < *last_shown - 1e-9) ++result.backward;
        if (settled && last_shown) {
            ++result.outputs;
            if (hold) ++result.holds;
            if (choice.kind == Kind::Blend) ++result.blends;
            const double error = (shown - *last_shown) - period;
            result.max_step_error = std::max(result.max_step_error, std::abs(error));
            sum_sq += error * error;
            ++measured;
        }
        last_shown = shown;
        slot += period;
    }
    result.target = target;
    result.delay = pacer.Delay();
    result.queue_clock = pacer.QueueClock();
    result.rms_step_error = measured ? std::sqrt(sum_sq / measured) : 0.0;
    return result;
}

void PacerGate() {
    const auto report = [](const char* name, const PacerResult& r) {
        std::printf("  pacer %-26s target %2u  outputs %4u  blends %4u  back %u  holds %u  "
                    "step error max %.2f ms rms %.2f ms  delay %.0f ms\n",
                    name, r.target, r.outputs, r.blends, r.backward, r.holds,
                    r.max_step_error * 1000.0, r.rms_step_error * 1000.0, r.delay * 1000.0);
    };
    struct Case {
        const char* name;
        PacerRun run;
        u32 target;
        double max_error; ///< ms
    };
    const Case cases[] = {
        {"15 fps, 4 ms jitter", {.fps = 15, .jitter = 0.004}, 30, 1.5},
        {"20 fps, 4 ms jitter", {.fps = 20, .jitter = 0.004}, 30, 1.5},
        {"20 fps, 12 ms jitter", {.fps = 20, .jitter = 0.012}, 30, 1.5},
        {"20 fps, 30 ms jitter", {.fps = 20, .jitter = 0.030}, 30, 1.5},
        // Uneven by nature: the guest shows it on 2 and 3 vsyncs in turn.
        {"24 fps (2:3 cadence)", {.fps = 24, .jitter = 0.004}, 30, 3.0},
        {"30 fps -> 60", {.fps = 30, .jitter = 0.004}, 60, 1.5},
        {"20 fps forced 60", {.fps = 20, .jitter = 0.004, .mode = FrameGenMode::Target60}, 60, 1.5},
    };
    for (const Case& c : cases) {
        const PacerResult r = SimulatePacer(c.run);
        report(c.name, r);
        Require(r.target == c.target, std::string{c.name} + ": wrong output rate");
        Require(r.outputs > 100, std::string{c.name} + ": no outputs");
        Require(r.backward == 0, std::string{c.name} + ": content time went back");
        Require(r.holds == 0, std::string{c.name} + ": late frames once settled");
        Require(r.max_step_error * 1000.0 <= c.max_error,
                std::string{c.name} + ": uneven content steps");
        Require(r.blends > r.outputs / 3, std::string{c.name} + ": too few generated frames");
        Require(r.rms_step_error * 1000.0 <= 0.5, std::string{c.name} + ": content steps jitter");
    }

    // A host stall (a shader compile): the offset resets and the output settles again.
    {
        PacerRun run{.fps = 20, .jitter = 0.004, .seconds = 14.0, .stall_at = 6.0, .stall_len = 0.4};
        const PacerResult r = SimulatePacer(run, 3.0, 5.5, 8.0);
        report("20 fps, 400 ms stall", r);
        Require(r.target == 30 && r.backward == 0 && r.holds == 0 && r.max_step_error < 0.0015,
                "20 fps stall: no recovery");
        // The stall does not stay as latency.
        Require(r.delay < 0.090, "20 fps stall: the delay stayed high");
    }
    // Rare host spikes (one frame 80 ms late every 4.5 s): held through, not kept as latency.
    {
        PacerRun run{.fps = 20, .jitter = 0.004, .seconds = 14.0, .spike_every = 90,
                     .spike_len = 0.080};
        const PacerResult r = SimulatePacer(run);
        report("20 fps, 80 ms spikes", r);
        Require(r.target == 30 && r.backward == 0, "20 fps spikes: no output");
        Require(r.holds <= 2 * 3, "20 fps spikes: held more than the spikes");
        Require(r.delay < 0.070, "20 fps spikes: a spike stayed in the delay");
    }
    // A game that paces itself (25 fps, no vsync lock): on the vsync clock its frames land 33 or
    // 50 ms apart and the motion judders; its queue times are even, and the content clock.
    {
        PacerRun run{.fps = 25, .jitter = 0.004, .seconds = 14.0, .self_paced = true};
        const PacerResult vsyncs = SimulatePacer(run);
        report("25 fps self-paced, vsyncs", vsyncs);
        run.queue_times = true;
        const PacerResult queued = SimulatePacer(run);
        report("25 fps self-paced, queue", queued);
        Require(queued.queue_clock, "25 fps self-paced: the queue clock was not taken");
        Require(queued.target == 30 && queued.backward == 0 && queued.holds == 0,
                "25 fps self-paced: queue clock output");
        Require(queued.rms_step_error < 0.001 && queued.rms_step_error * 3.0 < vsyncs.rms_step_error,
                "25 fps self-paced: the queue clock did not even out the motion");
    }
    // A game locked to the vsync keeps the vsync clock, however its queue times spread.
    {
        const PacerResult r = SimulatePacer({.fps = 20, .jitter = 0.004, .queue_times = true});
        report("20 fps locked, queue given", r);
        Require(!r.queue_clock && r.holds == 0 && r.rms_step_error * 1000.0 <= 0.5,
                "20 fps locked: the queue clock was taken");
    }
    // A guest gap (half a second without frames): no motion across it, then even again.
    {
        PacerRun run{.fps = 20, .jitter = 0.004, .seconds = 14.0, .gap_at = 6.0};
        const PacerResult r = SimulatePacer(run, 3.0, 5.5, 8.0);
        report("20 fps, 500 ms guest gap", r);
        Require(r.target == 30 && r.backward == 0 && r.holds == 0 && r.max_step_error < 0.0015,
                "20 fps gap: no recovery");
    }
    // The guest clock jumps ahead half a second (vsyncs caught up) while frames keep arriving at
    // the same pace: early by far, a resync, then even again.
    {
        PacerRun run{.fps = 20, .jitter = 0.004, .seconds = 14.0, .stall_at = 6.0,
                     .stall_len = -0.5, .gap_at = 6.0};
        const PacerResult r = SimulatePacer(run, 3.0, 5.5, 8.0);
        report("20 fps, clock 500 ms ahead", r);
        Require(r.target == 30 && r.backward == 0 && r.holds == 0 && r.max_step_error < 0.0015,
                "20 fps clock jump: no recovery");
    }
    // Rates: too slow, already fast, off, and switches.
    {
        const PacerResult slow = SimulatePacer({.fps = 8, .jitter = 0.004});
        Require(slow.target == 0, "8 fps should not be generated");
        const PacerResult fast = SimulatePacer({.fps = 60, .jitter = 0.002});
        Require(fast.target == 0, "60 fps should not be generated");
        const PacerResult off = SimulatePacer({.fps = 20, .jitter = 0.004, .mode = FrameGenMode::Off});
        Require(off.target == 0 && off.outputs == 0, "off generated frames");
        const PacerResult t30 = SimulatePacer({.fps = 40, .jitter = 0.004, .mode = FrameGenMode::Target30});
        Require(t30.target == 0, "forced 30 on a 40 fps game");
        std::printf("  pacer rates: 8 fps off, 60 fps off, mode off, forced 30 at 40 fps off\n");
    }
    {
        FrameGenPacer pacer;
        u64 vsync = 100;
        double time = 10.0;
        u32 target = 0;
        u32 frames_to_30 = 0;
        for (u32 i = 0; i < 60 && target != 30; ++i, vsync += 3, time += 0.05) {
            target = pacer.OnFrame(FrameGenMode::Auto, vsync, time);
            frames_to_30 = i + 1;
        }
        Require(target == 30, "auto never reached 30");
        u32 frames_to_60 = 0;
        for (u32 i = 0; i < 140 && target != 60; ++i, vsync += 2, time += 1.0 / 30.0) {
            target = pacer.OnFrame(FrameGenMode::Auto, vsync, time);
            frames_to_60 = i + 1;
        }
        Require(target == 60, "auto never switched 30 -> 60");
        // One odd frame must not flip the rate.
        for (u32 i = 0; i < 8; ++i, time += 1.0 / 30.0) {
            vsync += i == 3 ? 6 : 2;
            target = pacer.OnFrame(FrameGenMode::Auto, vsync, time);
            Require(target == 60, "a single slow frame flipped the rate");
        }
        vsync += 2;
        time += 1.0 / 30.0;
        target = pacer.OnFrame(FrameGenMode::Off, vsync, time);
        Require(target == 0, "off is not immediate");
        std::printf("  pacer switches: auto to 30 after %u frames, 30 -> 60 after %u, off at once\n",
                    frames_to_30, frames_to_60);
    }
    // A game between 20 and 30 (frames of 2 and 3 vsyncs at random, in runs): auto picks one rate
    // and keeps it, however the median moves.
    {
        FrameGenPacer pacer;
        u64 vsync = 100;
        double time = 10.0;
        u32 target = 0;
        u32 changes = 0;
        u32 step = 3;
        for (u32 i = 0; i < 1200; ++i) {
            if (Noise(i + 7000) < 0.15) {
                step = step == 3 ? 2 : 3;
            }
            vsync += step;
            time += step / 60.0;
            const u32 rate = pacer.OnFrame(FrameGenMode::Auto, vsync, time + 0.004 * Noise(i));
            if (target != 0 && rate != target) {
                ++changes;
            }
            target = rate;
        }
        std::printf("  pacer 20-30 fps mix: target %u, %u rate changes\n", target, changes);
        Require(target == 30 && changes == 0, "a 20-30 fps game flips between rates");
    }
    // The menu over the paused game: a redraw with vsync 0, frame generation off at once.
    {
        FrameGenPacer pacer;
        u64 vsync = 100;
        double time = 10.0;
        u32 target = 0;
        for (u32 i = 0; i < 60 && target == 0; ++i, vsync += 3, time += 0.05) {
            target = pacer.OnFrame(FrameGenMode::Auto, vsync, time);
        }
        Require(target == 30, "menu case: auto never reached 30");
        Require(pacer.OnFrame(FrameGenMode::Off, 0, time) == 0,
                "the menu frame went to frame generation");
        Require(pacer.OnFrame(FrameGenMode::Off, vsync - 3, time + 0.1) == 0,
                "a redraw under the menu went to frame generation");
        std::printf("  pacer menu: off at once on a redraw\n");
    }
}

// ---------------------------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------------------------

/// Synchronous work on the emulation queue: uploads, outputs and readbacks.
class Runner {
public:
    explicit Runner(const Device& device_) : device{device_} {
        ID3D12Device* const dev = device.Get();
        ThrowIfFailed(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator)),
                      "allocator");
        ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&list)),
                      "list");
        ThrowIfFailed(list->Close(), "close");
        ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        Require(event != nullptr, "event");
    }
    ~Runner() {
        CloseHandle(event);
    }

    /// Records with record, submits, waits. Returns the fence value it signalled.
    u64 Run(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        ThrowIfFailed(allocator->Reset(), "allocator reset");
        ThrowIfFailed(list->Reset(allocator.Get(), nullptr), "list reset");
        record(list.Get());
        ThrowIfFailed(list->Close(), "close");
        const u64 value = ++fence_value;
        {
            std::scoped_lock lock{device.QueueMutex()};
            ID3D12CommandList* const lists[] = {list.Get()};
            device.Queue()->ExecuteCommandLists(1, lists);
            ThrowIfFailed(device.Queue()->Signal(fence.Get(), value), "signal");
        }
        ThrowIfFailed(fence->SetEventOnCompletion(value, event), "event");
        WaitForSingleObject(event, INFINITE);
        return value;
    }

    ID3D12Fence* Fence() const {
        return fence.Get();
    }

private:
    const Device& device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    u64 fence_value{};
};

struct Picture {
    u32 width{};
    u32 height{};
    std::vector<u8> rgba;
};

constexpr double BASE_WIDTH = 1280.0;
constexpr double OBJECT_Y = 280.0;
constexpr double OBJECT_SIZE = 120.0;

/// The background: smooth texture in 1280x720 units, scaled to the image, panned by pan units.
Picture Background(u32 width, u32 height, bool cut, double pan = 0.0) {
    Picture image{width, height, std::vector<u8>(static_cast<size_t>(width) * height * 4)};
    const double s = width / BASE_WIDTH;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const double u = x / s + pan;
            const double v = y / s;
            double value = cut ? 150.0 + 40.0 * std::cos(u * 0.031 + v * 0.05) *
                                             std::sin(v * 0.083 - u * 0.02)
                               : 70.0 + 35.0 * std::sin(u * 0.045) * std::cos(v * 0.06) +
                                     15.0 * std::sin((u + 2.0 * v) * 0.11);
            value = std::clamp(value, 0.0, 190.0);
            u8* p = &image.rgba[(static_cast<size_t>(y) * width + x) * 4];
            p[0] = static_cast<u8>(value);
            p[1] = static_cast<u8>(value * 0.9);
            p[2] = static_cast<u8>(value * 0.75);
            p[3] = 255;
        }
    }
    return image;
}

/// The background with a white square (black border and stripes) at x (1280 units).
Picture Draw(const Picture& background, double x) {
    Picture image = background;
    const double s = image.width / BASE_WIDTH;
    const double x0 = x * s;
    const double y0 = OBJECT_Y * s;
    const double size = OBJECT_SIZE * s;
    for (u32 y = static_cast<u32>(y0); y < static_cast<u32>(y0 + size); ++y) {
        for (u32 px = static_cast<u32>(std::max(0.0, std::floor(x0)));
             px < std::min<u32>(image.width, static_cast<u32>(std::ceil(x0 + size))); ++px) {
            const double lx = (px + 0.5 - x0) / s;
            const double ly = (y + 0.5 - y0) / s;
            if (lx < 0.0 || lx >= OBJECT_SIZE) continue;
            const bool border = lx < 6.0 || lx >= OBJECT_SIZE - 6.0 || ly < 6.0 ||
                                ly >= OBJECT_SIZE - 6.0;
            const bool stripe = (lx >= 34.0 && lx < 42.0) || (lx >= 78.0 && lx < 86.0);
            const u8 value = border || stripe ? 10 : 245;
            u8* p = &image.rgba[(static_cast<size_t>(y) * image.width + px) * 4];
            p[0] = p[1] = p[2] = value;
        }
    }
    return image;
}

double Luma(const u8* p) {
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
}

/// Mean absolute luma difference over the band the object moves in.
double BandError(const Picture& a, const Picture& b, double from_x, double to_x) {
    const double s = a.width / BASE_WIDTH;
    const u32 x0 = static_cast<u32>(std::max(0.0, (from_x - 24.0) * s));
    const u32 x1 = static_cast<u32>(std::min<double>(a.width, (to_x + OBJECT_SIZE + 24.0) * s));
    const u32 y0 = static_cast<u32>((OBJECT_Y - 24.0) * s);
    const u32 y1 = static_cast<u32>((OBJECT_Y + OBJECT_SIZE + 24.0) * s);
    double sum = 0.0;
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.width + x) * 4;
            sum += std::abs(Luma(&a.rgba[i]) - Luma(&b.rgba[i]));
        }
    }
    return sum / (static_cast<double>(x1 - x0) * (y1 - y0));
}

/// Mean absolute luma difference over the whole image.
double ImageError(const Picture& a, const Picture& b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        sum += std::abs(Luma(&a.rgba[i]) - Luma(&b.rgba[i]));
    }
    return sum / (static_cast<double>(a.width) * a.height);
}

/// Object position (1280 units, left edge) from its bright pixels.
double ObjectX(const Picture& image) {
    const double s = image.width / BASE_WIDTH;
    double sum = 0.0;
    double count = 0.0;
    for (u32 y = static_cast<u32>((OBJECT_Y + 10.0) * s); y < static_cast<u32>((OBJECT_Y + 110.0) * s); ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            if (image.rgba[(static_cast<size_t>(y) * image.width + x) * 4] > 200) {
                sum += x + 0.5;
                ++count;
            }
        }
    }
    Require(count > 0, "object not found in an output");
    return sum / count / s - OBJECT_SIZE / 2.0;
}

/// Share of the pixels off by more than 40 luma: ghosts, tears, wrong blocks.
double BadPixels(const Picture& a, const Picture& b) {
    size_t bad = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        bad += std::abs(Luma(&a.rgba[i]) - Luma(&b.rgba[i])) > 40.0 ? 1 : 0;
    }
    return static_cast<double>(bad) / (static_cast<double>(a.width) * a.height);
}

Picture Crossfade(const Picture& a, const Picture& b, double t) {
    Picture image = a;
    for (size_t i = 0; i < a.rgba.size(); ++i) {
        image.rgba[i] = static_cast<u8>(std::lround(a.rgba[i] * (1.0 - t) + b.rgba[i] * t));
    }
    return image;
}

class Harness {
public:
    Harness(const Device& device_, Scheduler& scheduler_, Runner& runner_, u32 width_, u32 height_)
        : device{device_}, scheduler{scheduler_}, runner{runner_}, width{width_}, height{height_},
          pitch{width_ * 4} {
        Require(pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0, "width not pitch aligned");
        ID3D12Device* const dev = device.Get();
        const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = width, .Height = height,
            .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_NONE};
        for (auto& image : images) {
            ThrowIfFailed(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                       IID_PPV_ARGS(&image)),
                          "frame image");
        }
        ThrowIfFailed(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&back_buffer)),
                      "back buffer");
        const u64 bytes = static_cast<u64>(pitch) * height;
        upload = CreateCommittedBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                       D3D12_RESOURCE_FLAG_NONE, "upload");
        readback = CreateCommittedBuffer(dev, bytes, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                         "readback");
    }

    /// Uploads image into the next frame image (left in COPY_SOURCE) and returns it.
    ID3D12Resource* Upload(const Picture& image) {
        void* data{};
        ThrowIfFailed(upload->Map(0, nullptr, &data), "map upload");
        std::memcpy(data, image.rgba.data(), image.rgba.size());
        upload->Unmap(0, nullptr);
        ID3D12Resource* const dest = images[next_image].Get();
        next_image = (next_image + 1) % images.size();
        runner.Run([&](ID3D12GraphicsCommandList* list) {
            TransitionResource(list, dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
            const D3D12_TEXTURE_COPY_LOCATION to{.pResource = dest,
                                                 .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                 .SubresourceIndex = 0};
            const D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
                                                   .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                   .PlacedFootprint = Footprint()};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            TransitionResource(list, dest, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        });
        return dest;
    }

    /// Ingests a frame like PresentManager::Present: record, flush, commit. Returns the slot.
    u32 Ingest(FrameGen& fg, ID3D12Resource* image, u64 vsync) {
        const std::optional<u32> slot = fg.RecordIngest(image);
        Require(slot.has_value(), "ingest skipped");
        const u64 tick = scheduler.Flush();
        fg.Commit(*slot, tick, vsync);
        scheduler.Wait(tick);
        return *slot;
    }

    /// Records the output into the back buffer and reads it back. Returns the fence value.
    Picture Output(FrameGen& fg, const FrameGen::Output& output, u64* fence_value = nullptr) {
        const u64 value = runner.Run([&](ID3D12GraphicsCommandList* list) {
            fg.RecordOutput(list, output, back_buffer.Get());
            TransitionResource(list, back_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
                                                 .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                 .PlacedFootprint = Footprint()};
            const D3D12_TEXTURE_COPY_LOCATION from{.pResource = back_buffer.Get(),
                                                   .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                   .SubresourceIndex = 0};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            TransitionResource(list, back_buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        });
        if (fence_value) *fence_value = value;
        Picture image{width, height, std::vector<u8>(static_cast<size_t>(pitch) * height)};
        void* data{};
        const D3D12_RANGE range{0, image.rgba.size()};
        ThrowIfFailed(readback->Map(0, &range, &data), "map readback");
        std::memcpy(image.rgba.data(), data, image.rgba.size());
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        return image;
    }

    /// Records only the output (timing).
    void RecordOutput(FrameGen& fg, ID3D12GraphicsCommandList* list, const FrameGen::Output& out) {
        fg.RecordOutput(list, out, back_buffer.Get());
    }

private:
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint() const {
        return {.Offset = 0,
                .Footprint = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM, .Width = width,
                              .Height = height, .Depth = 1, .RowPitch = pitch}};
    }

    const Device& device;
    Scheduler& scheduler;
    Runner& runner;
    u32 width;
    u32 height;
    u32 pitch;
    std::array<ComPtr<ID3D12Resource>, 2> images;
    size_t next_image{};
    ComPtr<ID3D12Resource> back_buffer;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
};

/// Brings fg to output rate 30 the way a 20 fps game would.
void Activate(FrameGen& fg, u64& vsync, double& time) {
    u32 rate = 0;
    for (u32 i = 0; i < 80 && rate == 0; ++i) {
        rate = fg.OnFrame(vsync, true, time);
        vsync += 3;
        time += 0.05;
    }
    Require(rate == 30, "frame generation did not start");
}

FrameGen::Output Blend(u32 a, u32 b, float phase) {
    FrameGen::Output out;
    out.kind = Kind::Blend;
    out.a = a;
    out.b = b;
    out.phase = phase;
    return out;
}

void InterpolationGate(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                       DescriptorRing& ring, SamplerHeap& samplers, Runner& runner,
                       ID3D12InfoQueue* messages, u32 width, u32 height) {
    const auto start = std::chrono::steady_clock::now();
    FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), width, height};
    Harness harness{device, scheduler, runner, width, height};
    u64 vsync = 600;
    double time = 50.0;
    Activate(fg, vsync, time);

    const Picture background = Background(width, height, false);
    const double x_a = 300.0;
    const double dx = 40.0;
    const Picture a = Draw(background, x_a);
    const Picture b = Draw(background, x_a + dx);

    // The midpoint and the thirds (20 -> 30).
    const u32 slot_a = harness.Ingest(fg, harness.Upload(a), vsync);
    const u32 slot_b = harness.Ingest(fg, harness.Upload(b), vsync + 3);
    vsync += 6;
    for (const float phase : {0.5f, 1.0f / 3.0f, 2.0f / 3.0f}) {
        const Picture out = harness.Output(fg, Blend(slot_a, slot_b, phase));
        const Picture truth = Draw(background, x_a + dx * phase);
        const Picture fade = Crossfade(a, b, phase);
        const double error = BandError(out, truth, x_a, x_a + dx);
        const double fade_error = BandError(fade, truth, x_a, x_a + dx);
        const double position = ObjectX(out);
        std::printf("  %ux%u phase %.2f: object at %.1f (want %.1f), band error %.2f "
                    "(crossfade %.2f), image error %.2f\n",
                    width, height, phase, position, x_a + dx * phase, error, fade_error,
                    ImageError(out, truth));
        Require(std::abs(position - (x_a + dx * phase)) < 2.5,
                "interpolated object out of place");
        Require(error < fade_error * 0.35, "interpolation no better than a crossfade");
        Require(ImageError(out, truth) < 1.5, "interpolated image differs");
    }
    // A camera pan under an object moving the other way: the object covers background on one
    // side and reveals it on the other, where no vector matches both frames.
    {
        const double pan = 18.0;
        const Picture pa = Draw(Background(width, height, false, 0.0), x_a);
        const Picture pb = Draw(Background(width, height, false, pan), x_a + dx);
        const u32 s0 = harness.Ingest(fg, harness.Upload(pa), vsync);
        const u32 s1 = harness.Ingest(fg, harness.Upload(pb), vsync + 3);
        vsync += 6;
        for (const float phase : {1.0f / 3.0f, 0.5f, 2.0f / 3.0f}) {
            const Picture out = harness.Output(fg, Blend(s0, s1, phase));
            const Picture truth =
                Draw(Background(width, height, false, pan * phase), x_a + dx * phase);
            const Picture fade = Crossfade(pa, pb, phase);
            const double error = ImageError(out, truth);
            const double bad = BadPixels(out, truth);
            std::printf("  %ux%u pan phase %.2f: image error %.2f (crossfade %.2f), bad pixels "
                        "%.3f%% (crossfade %.3f%%)\n",
                        width, height, phase, error, ImageError(fade, truth), 100.0 * bad,
                        100.0 * BadPixels(fade, truth));
            Require(error < ImageError(fade, truth) * 0.5, "pan: no better than a crossfade");
            Require(bad < 0.004, "pan: ghosts or tears");
        }
    }
    // Phase 0 and 1 of a blend are the real frames.
    {
        const Picture out = harness.Output(fg, Blend(slot_a, slot_b, 0.0f));
        Require(ImageError(out, a) < 0.5, "phase 0 is not frame A");
    }
    // A real output is an exact copy.
    {
        FrameGen::Output real;
        real.kind = Kind::Real;
        real.a = slot_b;
        const Picture out = harness.Output(fg, real);
        Require(out.rgba == b.rgba, "real output is not an exact copy");
    }
    // A still pair stays still.
    {
        const u32 s0 = harness.Ingest(fg, harness.Upload(b), vsync);
        const u32 s1 = harness.Ingest(fg, harness.Upload(b), vsync + 3);
        vsync += 6;
        const Picture out = harness.Output(fg, Blend(s0, s1, 0.5f));
        const double error = ImageError(out, b);
        std::printf("  %ux%u still pair: image error %.3f\n", width, height, error);
        Require(error < 0.25, "a still pair changed");
    }
    // A scene cut shows the nearer frame.
    {
        const Picture cut = Draw(Background(width, height, true), 900.0);
        const u32 s0 = harness.Ingest(fg, harness.Upload(b), vsync);
        const u32 s1 = harness.Ingest(fg, harness.Upload(cut), vsync + 3);
        vsync += 6;
        const Picture near_a = harness.Output(fg, Blend(s0, s1, 0.35f));
        const Picture near_b = harness.Output(fg, Blend(s0, s1, 0.65f));
        const double error_a = ImageError(near_a, b);
        const double error_b = ImageError(near_b, cut);
        std::printf("  %ux%u scene cut: error to the nearer frame %.3f / %.3f\n", width, height,
                    error_a, error_b);
        Require(error_a < 0.25 && error_b < 0.25, "a scene cut was blended");
    }
    // After a Break the history is gone: no frames until the next ingest.
    fg.Break();
    Require(!fg.HasFrames(), "frames survived a break");

    // The whole path on a virtual clock: a 20 fps game, 4 ms jitter, outputs at 30.
    {
        double next_slot = -1.0;
        u32 frame = 0;
        constexpr u32 FRAMES = 36;
        constexpr double SPEED = 480.0; // 1280 units per second: 24 per game frame
        const double x_start = 100.0;
        std::vector<double> positions;
        std::vector<Kind> kinds;
        while (frame < FRAMES) {
            const double arrival = time + 0.008 + 0.004 * Noise(frame);
            if (frame < FRAMES && (next_slot < 0.0 || arrival <= next_slot)) {
                const u32 rate = fg.OnFrame(vsync, true, arrival);
                Require(rate == 30, "rate changed in a steady run");
                const double x = x_start + 24.0 * frame;
                harness.Ingest(fg, harness.Upload(Draw(background, x)), vsync);
                if (next_slot < 0.0) next_slot = arrival;
                vsync += 3;
                time += 0.05;
                ++frame;
                continue;
            }
            if (fg.HasFrames()) {
                const FrameGen::Output out = fg.Next(next_slot);
                Require(out.kind != Kind::None, "nothing to show with frames");
                Require(out.sync_interval == 2, "30 fps output not on every other vsync");
                u64 fence_value{};
                const Picture image = harness.Output(fg, out, &fence_value);
                fg.Unpin(out, fence_value);
                positions.push_back(ObjectX(image));
                kinds.push_back(out.kind);
            }
            next_slot += 1.0 / 30.0;
        }
        // Settled: from the eighth output on, every step is 16 (480 / 30).
        u32 blends = 0;
        double max_error = 0.0;
        for (size_t i = 8; i < positions.size(); ++i) {
            const double step = positions[i] - positions[i - 1];
            max_error = std::max(max_error, std::abs(step - SPEED / 30.0));
            blends += kinds[i] == Kind::Blend ? 1 : 0;
        }
        std::printf("  %ux%u virtual 20 -> 30 run: %zu outputs, %u generated, step error max "
                    "%.2f (of 16)\n",
                    width, height, positions.size(), blends, max_error);
        Require(positions.size() > 30, "too few outputs");
        Require(max_error < 3.0, "uneven motion in the paced run");
        Require(blends * 2 > positions.size() - 8, "too few generated frames in the paced run");
    }
    scheduler.Finish();
    Require(!DebugErrors(messages), "debug layer errors");
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %ux%u done in %.1f s\n", width, height, seconds);
}

void PerfGate(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
              DescriptorRing& ring, SamplerHeap& samplers, Runner& runner, u32 width, u32 height) {
    FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), width, height};
    Harness harness{device, scheduler, runner, width, height};
    u64 vsync = 600;
    double time = 50.0;
    Activate(fg, vsync, time);
    const Picture background = Background(width, height, false);
    std::array<ID3D12Resource*, 2> images{harness.Upload(Draw(background, 300.0)),
                                          harness.Upload(Draw(background, 340.0))};
    constexpr u32 COUNT = 60;
    // Warm up, then time ingests submitted back to back (one list each, like the game's frames).
    u32 slot_a = harness.Ingest(fg, images[0], vsync);
    u32 slot_b = harness.Ingest(fg, images[1], vsync + 3);
    vsync += 6;
    scheduler.Finish();
    auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < COUNT; ++i) {
        const std::optional<u32> slot = fg.RecordIngest(images[i % 2]);
        Require(slot.has_value(), "ingest skipped");
        const u64 tick = scheduler.Flush();
        fg.Commit(*slot, tick, vsync);
        vsync += 3;
        slot_a = slot_b;
        slot_b = *slot;
    }
    scheduler.Finish();
    const double ingest_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
        COUNT;
    start = std::chrono::steady_clock::now();
    runner.Run([&](ID3D12GraphicsCommandList* list) {
        for (u32 i = 0; i < COUNT; ++i) {
            harness.RecordOutput(fg, list, Blend(slot_a, slot_b, 0.5f));
        }
    });
    const double compose_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
        COUNT;
    std::printf("  perf %ux%u: ingest %.3f ms per game frame, compose %.3f ms per generated "
                "frame (desktop GPU, wall clock)\n",
                width, height, ingest_ms, compose_ms);
}

} // namespace

int main(int argc, char** argv) try {
    const bool perf = argc > 1 && std::string{argv[1]} == "perf";
    Common::Log::Initialize();
    // Frame generation reports why it could not start only in the log.
    Common::Log::SetColorConsoleBackendEnabled(true);
    if (!perf) {
        PacerGate();
        // The hardware GPU with the debug layer: the translated shaders are SM 6.4, and the WARP
        // that ships with Windows 10 stops at 6.2.
        Settings::values.renderer_debug = true;
    }
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    if (!perf) {
        ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
        messages->ClearStoredMessages();
        failure_messages = messages;
    }
    Scheduler scheduler{device};
    ShaderCompiler compiler;
    DescriptorRing ring{device.Get(), scheduler, 64 * 1024};
    SamplerHeap samplers{device.Get(), scheduler};
    Runner runner{device};
    SetFrameGenMode(FrameGenMode::Target30);
    if (perf) {
        PerfGate(device, scheduler, compiler, ring, samplers, runner, 1920, 1080);
        PerfGate(device, scheduler, compiler, ring, samplers, runner, 3840, 2160);
        scheduler.Finish();
        std::cout << "PASS frame generation perf\n";
        return 0;
    }
    InterpolationGate(device, scheduler, compiler, ring, samplers, runner, messages.Get(), 1280, 720);
    InterpolationGate(device, scheduler, compiler, ring, samplers, runner, messages.Get(), 3840, 2160);
    // Off: OnFrame reports no rate and nothing is built.
    {
        SetFrameGenMode(FrameGenMode::Off);
        FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), 1280, 720};
        u64 vsync = 100;
        for (u32 i = 0; i < 60; ++i, vsync += 3) {
            Require(fg.OnFrame(vsync, true, 10.0 + i * 0.05) == 0, "off generated a rate");
        }
        Require(!fg.HasFrames() && fg.Target() == 0, "off has frames");
        Require(!fg.RecordIngest(nullptr).has_value(), "off ingested");
        // Menus (allowed = false) never generate.
        SetFrameGenMode(FrameGenMode::Auto);
        for (u32 i = 0; i < 60; ++i, vsync += 3) {
            Require(fg.OnFrame(vsync, false, 20.0 + i * 0.05) == 0, "a menu frame generated");
        }
        std::cout << "  off and menus: no rate, nothing built\n";
    }
    scheduler.Finish();
    Require(!DebugErrors(messages.Get()), "debug layer errors");
    std::cout << "PASS frame generation: pacer cadence, interpolation at 720p and 4K, still "
                 "pairs, scene cuts, paced run, off; no debug layer errors\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL frame generation: " << e.what() << '\n';
    // The debug layer says why a pipeline or a resource was refused.
    DebugErrors(failure_messages.Get());
    return 1;
}
