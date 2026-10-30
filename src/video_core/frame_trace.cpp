// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging.h"
#include "core/arm/jit_prewarm_stats.h"
#include "dynarmic/interface/jit_profile.h"
#include "video_core/frame_trace.h"

namespace VideoCore::FrameTrace {
namespace {

struct Entry {
    u64 ns;
    u32 thread;
    Event event;
    u64 a;
    u64 b;
};

// Keep sleep/lock SVCs as completed spans >=200us to bound eight-second diagnostics.
// Fixed storage (~16 MiB), with no allocations while recording.
constexpr size_t CAPACITY = 524288;

std::atomic_bool active{false};
std::atomic<CaptureState> capture_state{CaptureState::Idle};
std::atomic<u32> capture_id{0};
std::atomic<u32> vsyncs_left{0};
std::atomic<u32> next_entry{0};
std::atomic<u32> writers{0};
std::chrono::steady_clock::time_point start_time;
std::array<Entry, CAPACITY> entries;
Dynarmic::JitProfile::Snapshot jit_begin{};
bool jit_profiled{};
Core::JitPrewarm::MissSnapshot misses_begin{};

// Ring mode (ArmOnSwapIntervalRise): entries wrap until the game raises its swap interval.
bool ring{};
std::atomic_bool triggered{false};
u32 trigger_index{};
u32 ring_vsyncs_before{};
u32 ring_vsyncs_after{};
u32 ring_captures_left{};
u32 last_requested_interval{};

u32 CurrentThread() {
#ifdef _WIN32
    return static_cast<u32>(GetCurrentThreadId());
#else
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000000);
#endif
}

const char* Name(Event event) {
    switch (event) {
    case Event::Vsync:
        return "vsync";
    case Event::VsyncComposed:
        return "vsync-composed";
    case Event::ComposeWaitEnd:
        return "vsync-waited-gpu-thread";
    case Event::Composite:
        return "composite";
    case Event::QueueBuffer:
        return "game-queue-buffer";
    case Event::AcquireBuffer:
        return "display-acquire-buffer";
    case Event::ReleaseBuffer:
        return "display-release-buffer";
    case Event::DequeueWait:
        return "game-dequeue-wait";
    case Event::DequeueWaitEnd:
        return "game-dequeue-wait-end";
    case Event::GpuFenceWait:
        return "game-fence-wait";
    case Event::GpuFenceSignal:
        return "game-fence-signal";
    case Event::GpuSubmit:
        return "gpu-submit";
    case Event::GpuIdleEnd:
        return "gpu-thread-got-work";
    case Event::GuestSvcBegin:
        return "guest-svc-begin";
    case Event::GuestSvcEnd:
        return "guest-svc-end";
    case Event::GuestSvcLong:
        return "guest-svc-long";
    case Event::GuestThreadReady:
        return "guest-thread-ready";
    case Event::GuestDispatch:
        return "guest-dispatch";
    case Event::GuestRunCompile:
        return "guest-run-compile";
    case Event::GuestRunFlush:
        return "guest-run-flush";
    case Event::GuestRunLong:
        return "guest-run-long";
    case Event::GuestHostCpuWindow:
        return "guest-host-cpu-window";
    case Event::GuestRunPc:
        return "guest-run-pc";
    case Event::GuestRunStop:
        return "guest-run-stop";
    case Event::GuestRunClock:
        return "guest-run-clock";
    case Event::GuestRunIcache:
        return "guest-run-icache";
    case Event::GuestRunMemory:
        return "guest-run-memory";
    case Event::TextureGcBudget:
        return "texture-gc-budget";
    case Event::TextureGcPressure:
        return "texture-gc-pressure";
    case Event::TextureGcLong:
        return "texture-gc-long";
    case Event::TextureEvict:
        return "texture-evict";
    case Event::TextureCreate:
        return "texture-create";
    case Event::TextureHeapUsage:
        return "texture-heap-usage";
    case Event::TextureHeapFree:
        return "texture-heap-free";
    case Event::TextureHeapPending:
        return "texture-heap-pending";
    case Event::TextureGcReadback:
        return "texture-gc-readback";
    case Event::TextureGcPrepare: return "texture-gc-prepare";
    case Event::TextureGcStaging: return "texture-gc-staging";
    case Event::TextureGcCopy: return "texture-gc-copy";
    case Event::TextureGcWait: return "texture-gc-wait";
    case Event::TextureGcSwizzle: return "texture-gc-swizzle";
    case Event::TextureGcRelease: return "texture-gc-release";
    case Event::TextureGcCompact: return "texture-gc-compact";
    case Event::TextureGcFootprint: return "texture-gc-footprint";
    case Event::TextureGcSyncReason: return "texture-gc-sync-reason";
    case Event::TextureGcImageInfo: return "texture-gc-image-info";
    case Event::TextureGcImageFormat: return "texture-gc-image-format";
    case Event::TextureUploadLong:
        return "texture-upload-long";
    case Event::TextureUploadStaging:
        return "texture-upload-staging";
    case Event::TextureUploadRead:
        return "texture-upload-read";
    case Event::TextureUploadUnswizzle:
        return "texture-upload-unswizzle";
    case Event::TextureUploadConvert:
        return "texture-upload-convert";
    case Event::TextureUploadBackend:
        return "texture-upload-backend";
    case Event::TextureUploadRepack:
        return "texture-upload-repack";
    case Event::TextureUploadInfo:
        return "texture-upload-info";
    case Event::TextureUploadFormat:
        return "texture-upload-format";
    case Event::GuestIpcCommand:
        return "guest-ipc-command";
    case Event::GuestIpcName0:
        return "guest-ipc-name0";
    case Event::GuestIpcName1:
        return "guest-ipc-name1";
    case Event::GuestIpcName2:
        return "guest-ipc-name2";
    case Event::GuestIpcLock:
        return "guest-ipc-lock";
    case Event::GuestIpcHandler:
        return "guest-ipc-handler";
    case Event::VsyncTick:
        return "vsync-tick";
    case Event::FrameLeaseAcquire:
        return "frame-lease-acquire";
    case Event::FrameLeaseHold:
        return "frame-lease-hold";
    case Event::FrameLeaseRelease:
        return "frame-lease-release";
    case Event::DisplayComposeLock:
        return "display-compose-lock";
    case Event::DisplayComposeLong:
        return "display-compose-long";
    case Event::HostPresentLong:
        return "host-present-long";
    case Event::AddressWaitBegin:
        return "address-wait-begin";
    case Event::AddressWaitCondition:
        return "address-wait-condition";
    case Event::AddressWaitTimeout:
        return "address-wait-timeout";
    case Event::AddressWaitEnd:
        return "address-wait-end";
    case Event::AddressWake:
        return "address-wake";
    case Event::AddressCancel:
        return "address-cancel";
    case Event::GuestSvcPc:
        return "guest-svc-live-pc";
    case Event::GuestSvcLr:
        return "guest-svc-live-lr";
    case Event::AddressSignalRequest:
        return "address-signal-request";
    case Event::AddressSignalArgs:
        return "address-signal-args";
    case Event::AddressSignalEnd:
        return "address-signal-end";
    case Event::FenceQueued:
        return "fence-queued";
    case Event::FenceDequeued:
        return "fence-dequeued";
    case Event::FenceWaitLong:
        return "fence-wait-long";
    case Event::FenceFlushLong:
        return "fence-flush-long";
    case Event::FenceFlushLockLong:
        return "fence-flush-lock-long";
    case Event::FenceTextureFlushLong:
        return "fence-texture-flush-long";
    case Event::FenceBufferFlushLong:
        return "fence-buffer-flush-long";
    case Event::FenceQueryFlushLong:
        return "fence-query-flush-long";
    case Event::FenceCallbacksBegin:
        return "fence-callbacks-begin";
    case Event::FenceCallbacksLong:
        return "fence-callbacks-long";
    case Event::FenceDone:
        return "fence-done";
    case Event::FenceBackendTick:
        return "fence-backend-tick";
    case Event::FenceSubmitWaitLong:
        return "fence-submit-wait-long";
    case Event::FenceGpuWaitLong:
        return "fence-gpu-wait-long";
    case Event::SyncWaitBegin:
        return "sync-wait-begin";
    case Event::SyncWaitObject:
        return "sync-wait-object";
    case Event::SyncWakeObject:
        return "sync-wake-object";
    case Event::SyncWaitEnd:
        return "sync-wait-end";
    case Event::NvEventArmed:
        return "nv-event-armed";
    case Event::NvEventSignal:
        return "nv-event-signal";
    case Event::PipelineFrontendLong:
        return "pipeline-frontend-long";
    case Event::PipelineBuildRequested:
        return "pipeline-build-requested";
    case Event::PipelineWorkerBegin:
        return "pipeline-worker-begin";
    case Event::PipelineWorkerLong:
        return "pipeline-worker-long";
    case Event::PipelineDxilLong:
        return "pipeline-dxil-long";
    case Event::PipelineTranslateLong:
        return "pipeline-translate-long";
    case Event::PipelineValidatorLockLong:
        return "pipeline-validator-lock-long";
    case Event::PipelineSignLong:
        return "pipeline-sign-long";
    case Event::PipelinePsoLong:
        return "pipeline-pso-long";
    case Event::PipelineWaitLong:
        return "pipeline-wait-long";
    case Event::PipelineCacheResult:
        return "pipeline-cache-result";
    case Event::PipelineBuildDone:
        return "pipeline-build-done";
    case Event::NvEventSignalLong:
        return "nv-event-signal-long";
    case Event::VsyncSignal:
        return "guest-vsync-signal";
    }
    return "?";
}

/// Dumps `count` entries in recording order, starting at ring position `first`.
void Dump(u32 first, u32 count) {
    LOG_INFO(Render, "Frame trace: {} events (ms from the start, host thread, event, a, b)",
             count);
    for (u32 i = 0; i < count; ++i) {
        const Entry& entry = entries[(first + i) % CAPACITY];
        LOG_INFO(Render, "FT {:9.3f} {:>6} {} {} {}", static_cast<double>(entry.ns) / 1e6,
                 entry.thread, Name(entry.event), entry.a, entry.b);
    }
    LOG_INFO(Render, "Frame trace: end");
}

void BeginMeasuredWindow() {
    jit_profiled = Dynarmic::JitProfile::enabled.load(std::memory_order_relaxed);
    jit_begin = Dynarmic::JitProfile::Read();
    misses_begin = Core::JitPrewarm::ReadMisses();
    Core::JitPrewarm::capture_active.store(true, std::memory_order_relaxed);
}

void Arm() {
    next_entry.store(0);
    triggered.store(false);
    last_requested_interval = 0;
    vsyncs_left.store(ring_vsyncs_after);
    start_time = std::chrono::steady_clock::now();
    const u32 id = capture_id.fetch_add(1) + 1;
    LOG_INFO(Render, "Frame trace: armed (capture {}), records {} vsyncs before and {} after the "
                     "game raises its swap interval", id, ring_vsyncs_before, ring_vsyncs_after);
    capture_state.store(CaptureState::Armed);
    active.store(true);
}

/// Called on the queueing thread: starts the countdown once the requested interval rises.
void CheckSwapIntervalRise(u32 index, u64 intervals) {
    const u32 requested = static_cast<u32>(intervals);
    const u32 previous = std::exchange(last_requested_interval, requested);
    // From 2 up only: games often go from 1 to 2 when leaving their title screens.
    if (triggered.load() || previous < 2 || requested <= previous) {
        return;
    }
    trigger_index = index;
    triggered.store(true);
    BeginMeasuredWindow();
    LOG_INFO(Render, "Frame trace: game raised its swap interval {} -> {} (capture {}), recording "
                     "{} more vsyncs", previous, requested, capture_id.load(), ring_vsyncs_after);
    capture_state.store(CaptureState::Recording);
}

/// First ring position of the dump: `ring_vsyncs_before` vsyncs back from the trigger.
u32 RingDumpFirst(u32 total) {
    const u32 oldest = total > CAPACITY ? total - static_cast<u32>(CAPACITY) : 0;
    u32 first = trigger_index;
    u32 vsyncs = 0;
    while (first > oldest) {
        if (entries[(first - 1) % CAPACITY].event == Event::Vsync &&
            ++vsyncs > ring_vsyncs_before) {
            break;
        }
        --first;
    }
    return first;
}

} // Anonymous namespace

void Start(u32 vsyncs) {
    if (active.load() || capture_state.load() == CaptureState::Saving || vsyncs == 0) {
        return;
    }
    ring = false;
    next_entry.store(0);
    vsyncs_left.store(vsyncs);
    BeginMeasuredWindow();
    start_time = std::chrono::steady_clock::now();
    const u32 id = capture_id.fetch_add(1) + 1;
    LOG_INFO(Render, "Frame trace: recording {} vsyncs (capture {}), focused address chain guests79/83", vsyncs, id);
    capture_state.store(CaptureState::Recording);
    active.store(true);
}

void ArmOnSwapIntervalRise(u32 vsyncs_before, u32 vsyncs_after, u32 captures) {
    if (active.load() || capture_state.load() == CaptureState::Saving || vsyncs_after == 0 ||
        captures == 0) {
        return;
    }
    ring = true;
    ring_vsyncs_before = vsyncs_before;
    ring_vsyncs_after = vsyncs_after;
    ring_captures_left = captures;
    Arm();
}

bool Active() {
    return active.load(std::memory_order_relaxed);
}

CaptureStatus GetCaptureStatus() {
    return {capture_state.load(std::memory_order_relaxed),
            capture_id.load(std::memory_order_relaxed),
            vsyncs_left.load(std::memory_order_relaxed)};
}

void Mark(Event event, u64 a, u64 b) {
    if (!Enabled(event, a, b) || !active.load(std::memory_order_relaxed)) {
        return;
    }
    writers.fetch_add(1);
    const u32 index = next_entry.fetch_add(1);
    if (index < CAPACITY || ring) {
        entries[index % CAPACITY] = {
            .ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start_time)
                                       .count()),
            .thread = CurrentThread(),
            .event = event,
            .a = a,
            .b = b,
        };
    }
    writers.fetch_sub(1);
    if (ring && event == Event::QueueBuffer) {
        CheckSwapIntervalRise(index, b);
    }
    const bool counting = !ring || triggered.load(std::memory_order_relaxed);
    const bool last_vsync =
        counting && event == Event::Vsync && vsyncs_left.fetch_sub(1) == 1;
    // Guest state events can be recorded with the kernel scheduler lock held. Defer the
    // dump to VSync even when full: formatting thousands of lines under that lock would
    // stall the threads whose wakeup latency we are measuring. Entries stay bounded.
    const bool full_at_vsync = !ring && event == Event::Vsync && index + 1 >= CAPACITY;
    if (!full_at_vsync && !last_vsync) {
        return;
    }
    auto expected = CaptureState::Recording;
    if (!capture_state.compare_exchange_strong(expected, CaptureState::Saving)) {
        return;
    }
    // Publish Saving before disabling marks: T must not reset entries while Dump reads them.
    active.store(false);
    Core::JitPrewarm::capture_active.store(false, std::memory_order_relaxed);
    // Let marks already past the active check finish writing.
    while (writers.load() != 0) {
        std::this_thread::yield();
    }
    // Sample before Dump: writing thousands of trace lines is outside the measured capture.
    const auto misses_end = Core::JitPrewarm::ReadMisses();
    const auto jit_end = Dynarmic::JitProfile::Read();
    for (size_t core = 0; core < misses_end.size(); ++core) {
        for (size_t reason = 0; reason < Core::JitPrewarm::MissNames.size(); ++reason) {
            const auto count = misses_end[core][reason] - misses_begin[core][reason];
            if (count != 0) {
                LOG_INFO(Render, "Frame trace JIT misses capture {} core {} {}: {} blocks",
                         capture_id.load(), core, Core::JitPrewarm::MissNames[reason], count);
            }
        }
    }
    if (jit_profiled) {
        const u32 id = capture_id.load();
        LOG_INFO(Render, "Frame trace JIT capture {}: elapsed sums across cores; phases overlap; "
                         "calls crossing borders are included at completion", id);
        for (size_t i = 0; i < jit_end.size(); ++i) {
            LOG_INFO(Render, "Frame trace JIT capture {} {}: {} calls, {:.3f} ms", id,
                     Dynarmic::JitProfile::names[i], jit_end[i].calls - jit_begin[i].calls,
                     (jit_end[i].ns - jit_begin[i].ns) / 1.0e6);
        }
    }
    if (ring) {
        const u32 total = next_entry.load();
        const u32 first = RingDumpFirst(total);
        Dump(first, total - first);
        capture_state.store(CaptureState::Saved);
        if (--ring_captures_left != 0) {
            Arm();
        }
        return;
    }
    Dump(0, std::min<u32>(next_entry.load(), static_cast<u32>(CAPACITY)));
    capture_state.store(full_at_vsync && !last_vsync ? CaptureState::Truncated : CaptureState::Saved);
}

} // namespace VideoCore::FrameTrace
