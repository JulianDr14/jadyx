// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <unordered_map>

#include "common/common_types.h"

/// Frame-level performance counters, to tell what a slow frame spent its time on (the D3D12
/// renderer logs them per hitch and per pacing window). Any thread adds; the presenter reads the
/// running totals and diffs them per frame, so nothing is ever reset.
namespace VideoCore::Perf {

enum class Counter : size_t {
    GpuThreadIdleUs,    ///< GPU thread waiting for guest command lists (the guest CPU is behind)
    GpuThreadFlushes,   ///< FlushRegion requests (the guest reading back GPU memory)
    GpuThreadFlushUs,
    Submits,            ///< command lists submitted to the queue
    FenceWaits,         ///< CPU waits on the GPU fence that actually blocked
    FenceWaitUs,
    GpuBusyUs,          ///< GPU time of the submitted command lists (timestamps)
    PipelineStalls,     ///< draws or dispatches that waited for a pipeline to be built
    PipelineStallUs,
    PipelineWaitUs,     ///< Dependent draws/dispatches only; separate from frontend translation.
    PipelineFrontendUs,
    PsoRequests,       ///< Host PSO lookups, not per-draw guest pipeline hits.
    PsoHits,
    PsoShared,         ///< Concurrent equivalent creation avoided.
    PsoBypasses,       ///< Host index capacity reached; correctness preserved.
    PsoCreates,
    PsoCreateUs,       ///< Accumulated worker wall time; overlaps between workers.
    Draws,
    Dispatches,
    TextureUploads,
    TextureUploadBytes,
    TextureDownloads,
    TextureDownloadBytes,
    ResourcesCreated,   ///< committed resources created (images, staging, transfer buffers)
    ResourceCreateUs,
    StagingDedicated,      ///< staging buffers created outside the stream ring
    StagingDedicatedBytes,
    StagingStreamWaits,    ///< large uploads that waited for the GPU to free the stream ring
    TransferPoolHits,      ///< transfer buffers handed out again from the pool
    TransferPoolMissBusy,  ///< misses with a fitting pooled buffer the GPU still used
    TransferPoolMissNoFit, ///< misses with no pooled buffer of a fitting size
    TransferPoolMissBytes, ///< bytes of the buffers created on those misses
    TransferPoolOverflows, ///< returned buffers released because the pool was at its budget
    TransferPoolOverflowBytes,
    TextureDecodeUs,       ///< CPU decoding/re-encoding of converted textures (ASTC, BCn arrays)
    TextureGpuDecodes,     ///< textures decoded by a compute shader (ASTC)
    TextureUploadMapNs,    ///< querying a CPU pointer for non-staging upload sources
    TextureUploadRepackNs, ///< CPU row repacking into a D3D12-aligned footprint
    TextureUploadRecordNs, ///< CopyTextureRegion recording
    TextureUploadMaps,
    TextureUploadRepacks,
    TextureUploadCopies,
    TextureCacheFindNs,
    TextureCacheFinds,
    TextureCacheInsertNs,
    TextureCacheInserts,
    TextureCacheOverlapNs,
    TextureCacheImageCreateNs,
    TextureCacheRefreshNs,
    TextureCacheStagingNs,
    TextureCacheUnswizzleNs,
    TextureCacheBackendUploadNs,
    TextureCacheRegisterNs,
    TextureCacheViewCreateNs,
    TextureCacheViewsCreated,
    TextureCacheViewSetupNs,       ///< format, swizzle, range and dimension derivation
    TextureCacheViewReinterpretNs, ///< typeless-family validation/reinterpreted image lookup
    TextureCacheViewSrvNs,         ///< SRV parameters and eager natural SRV creation
    TextureCacheViewAttachmentNs,  ///< RTV/DSV/UAV capability checks and descriptor creation
    TextureCacheViewSlotInsertNs,  ///< SlotVector insertion when it already has a free slot
    TextureCacheViewSlotGrowNs,    ///< SlotVector allocation and relocation plus inserted view
    TextureCacheViewSlotGrows,
    TextureCacheViewSlotClockNs,  ///< back-to-back steady_clock calls in profiled insertion
    TextureCacheViewSlotFreeNs,   ///< selecting/growing the backing slot
    TextureCacheViewSlotConstructNs, ///< placement-new, including member initializers
    TextureCacheViewSlotBitNs,    ///< marking the selected slot occupied
    TextureCacheViewIndexNs,       ///< linking the new view into its owning Image
    TextureCacheViewBaseNs,        ///< ImageViewBase constructor body
    TextureCacheViewCompatibilityNs, ///< IsViewCompatible inside ImageViewBase
    DrawNs,                ///< whole draws (direct and indirect), nested counters included
    DrawTexturesNs,        ///< of which: reading texture handles and finding the image views
    DrawBuffersNs,         ///< uniform, storage, texel, vertex and index buffers
    DrawDescriptorsNs,     ///< writing the descriptor table (and each stage's host buffers)
    DrawTargetsNs,         ///< render targets, feedback loops and image transitions
    DrawUpdateTargetsNs,
    DrawFeedbackNs,
    DrawFramebufferNs,
    DrawImageTransitionsNs,
    DrawSamplersNs,        ///< the sampler table
    DrawRecordNs,          ///< preparing attachments and recording state and the draw
    PipelineFastHits,      ///< transition-cache hits in CurrentGraphicsPipeline
    PipelineFastMisses,
    PipelineKeyReuses,     ///< draws that kept the previous pipeline without refreshing the key
    PipelineKeyMismatches, ///< check mode: reuses that would have drawn with another pipeline
    PipelineKeyChecks,     ///< check mode: draws that could have reused, verified by a lookup
    CbvCreateNs,
    CbvCreates,
    CbvStreamed,
    CbvReused,      ///< streamed uniforms rebound from the submission's earlier copy
    CbvPersistent,
    CbvNull,
    CbvRoot,        ///< bound as root CBVs instead of table descriptors
    CbvRootClamped, ///< root CBVs bound to zeros: the shader could read past the buffer
    ViewCopyNs,
    ViewCopies,
    DescriptorTables,       ///< resource tables a draw or dispatch asked for
    DescriptorTableReuses,  ///< of them, served by an identical table of the same list
    RenderTargetEarlyOuts,  ///< draws that kept the attachments prepared by an earlier one
    StageViewReuses,        ///< shader stages that kept the views and samplers of the last draw
    StageViewMisses,        ///< cacheable stages that resolved them again
    TextureReplays,         ///< draws that bound the textures of the pipeline's last draw
    TextureReplayMismatches, ///< texture_replay=check: replays that would have bound otherwise
    SubmitListNs,
    SubmitLists,
    GpuTickNs,
    GpuTicks,
    CacheInvalidationNs,
    CacheInvalidations,
    DmaPullerNs,
    DmaPullerCalls,
    DmaMacroNs,
    DmaMacroCalls,
    DmaMaxwellNs,
    DmaMaxwellCalls,
    DmaComputeNs,
    DmaComputeCalls,
    DmaCopyNs,
    DmaCopyCalls,
    DmaOtherNs,
    DmaOtherCalls,
    DmaHeaderFlushes,        ///< pushbuffer reads that flushed GPU-written memory first
    DmaHeaderFlushesSkipped, ///< safe pushbuffer reads with no GPU-written granule (no flush)
    MaxwellDirtyChanged,
    MaxwellDirtyUnchanged,
    ClearNs,
    DispatchNs,
    IndexRewrites,         ///< index lists converted (quads, fans, loops, u8) and cached
    IndexRewriteReuses,    ///< draws that bound a cached conversion without reading the guest
    IndexRewriteRechecks,  ///< cached conversions confirmed by comparing the guest bytes
    IndexRewriteNs,        ///< time spent finding, checking and converting index lists
    ConstBufferRuns,       ///< CB_DATA packets stored as one block instead of word by word
    ConstBufferRunWords,   ///< words those packets carried
    BlitUploadsSkipped,    ///< CPU-modified blit destinations not uploaded: the blit rewrote them
    BlitUploadBytesSkipped,
    BlitDiscardsRestored,  ///< skipped uploads put back because the backend drew nothing
    GuestGpuWaits,         ///< guest fence waits (nvhost_ctrl events) that the GPU signalled later
    GuestGpuWaitUs,        ///< time from each of those waits to its signal; waits can overlap
    SyncpointsEarly,       ///< syncpoints signalled when the GPU thread reached them
    // Syncpoints signalled once the host GPU ran the work before them, by the first reason found:
    SyncpointsAfterBufferDownloads,  ///< GPU-written buffers to download with this fence
    SyncpointsAfterTextureDownloads, ///< GPU-written images to download with this fence
    SyncpointsAfterQueryFlushes,     ///< query results to flush with this fence
    SyncpointsAfterOperations,       ///< operations queued with this fence (query reports)
    SyncpointsAfterQueuedFences,     ///< an earlier fence still has downloads or operations
    // The frame chain: game -> GPU thread -> composite -> vsync.
    Vsyncs,                ///< vsyncs processed by the VSyncThread (60 per second when on time)
    VsyncsLost,            ///< vsyncs skipped because the previous one ran late
    VsyncComposeWaitUs,    ///< the VSyncThread waiting for the GPU thread's previous composite
    VsyncFrames,           ///< vsyncs that composed a new game frame
    Composites,            ///< composites the renderer ran
    CompositeLatencyUs,    ///< from each composite request to the renderer running it
    CompositesLate,        ///< composites that ran more than a frame after the request
    GuestFramesQueued,     ///< frames the game queued (QueueBuffer)
    GuestSwapIntervalOverrides, ///< diagnostic requests above one forced back to one
    GuestDequeueWaits,     ///< times the game waited for a free framebuffer (DequeueBuffer)
    GuestDequeueWaitUs,
    GuestCoreIdleUs,       ///< emulated CPU cores with no guest thread to run, summed over cores
    GuestCoreWakesSpun,    ///< idle cores interrupted while spinning (no OS wake)
    GuestCoreWakesBlocked, ///< idle cores interrupted after blocking in the OS
    // The GPU thread taking a present frame to composite into (PresentManager::AcquireFrame).
    PresentFreeWaitUs,     ///< waiting for a free frame: every one queued or being copied
    PresentRenderWaitUs,   ///< waiting for the GPU to finish the frame's previous rendering
    PresentCopyWaitUs,     ///< waiting on the CPU for the frame's previous copy to the display
    PresentWaitsLong,      ///< frames taken after waiting over 1 ms in all
    PresentsReplaced,      ///< present_mailbox: queued frames reused before being shown
    PresentsWouldReplace,  ///< present_mailbox=check: frames the mailbox would have reused
    VsyncComposeWaitsCut,  ///< vsync_nowait: composite waits cut at 2 ms (check: would cut)
    VsyncComposeWaitsFull, ///< vsync_nowait=1: full waits, a cut composite still in flight
    GuestSwapIntervalChanges, ///< the game changed its swap interval (logged with the cause)
    GuestSwapIntervalsHeld, ///< swap_interval_hold: frames of interval 3 paced at 2 (check: would)
    // Guest CPU writes into rasterizer-cached memory (VideoCommon::CpuWriteQueue).
    CpuWritesDeferred,        ///< cpu_write_defer=1: queued instead of taking the cache locks
    CpuWritesWouldDefer,      ///< cpu_write_defer=check: writes that would have been queued
    CpuWriteDeferMismatches,  ///< check: of those, OnCPUWrite found GPU data (must stay 0)
    CpuWriteQueueFull,        ///< cpu_write_defer=1: queue full, OnCPUWrite instead
    CpuWritesDrained,         ///< queued writes the GPU thread applied
    CpuWritesLateGpu,         ///< of those, ranges that became GPU-modified while queued
    CpuWriteDrains,           ///< cpu_write_test: drains that found writes queued
    CpuWriteDrainNs,          ///< cpu_write_test: GPU thread time applying them
    CpuWriteRanges,           ///< cpu_write_test: runs of queued writes that touch or overlap
    CpuWritePageRuns,         ///< cpu_write_test: runs of queued writes inside one page
    // Guest loads of rasterizer-cached memory (jit_read_table, Common::PageTable).
    JitReadPagesCleared,      ///< load entries cleared when the GPU first wrote their memory
    JitReadsWouldInline,      ///< check: slow loads the load table would have done inline
    JitReadMismatches,        ///< check: of those, memory the GPU may have written (must stay 0)
    GpuFlushUnmarked,         ///< a CPU read flushed GPU data GpuWrittenPages missed (must stay 0)
    // Why emulated cores left the JIT (Kernel::PhysicalCore::RunThread). One exit can count twice.
    GuestRunExits,            ///< JIT runs that returned
    GuestExitsSvc,            ///< of those, supervisor calls (per number in guest_svc_exits)
    GuestExitsInterrupt,      ///< interrupts (BreakLoop: scheduling, timers, cross-core requests)
    GuestExitsCacheInvalidation, ///< code cache invalidations
    GuestExitsDataAbort,      ///< data aborts
    // Indirect draw macros (DrawArraysIndirect / DrawIndexedIndirect).
    IndirectMacroGpu,         ///< drawn from the GPU-side arguments (ExecuteIndirect)
    IndirectMacroCpu,         ///< drawn directly from arguments read on the CPU
    IndirectMacroCpuWritten,  ///< of those, arguments the GPU had written (read back with a flush)
    IndirectMacroCpuZero,     ///< of those, draws with no instances
    // Sample counts (occlusion queries) written back to guest memory.
    OcclusionResults,         ///< results written
    OcclusionResultsZero,     ///< of those, zero samples passed
    OcclusionResultLatencyUs, ///< summed time from the guest's report to the write
    // Cache garbage collection under memory pressure (always counted, once per frame at most).
    CacheGcRuns,              ///< texture and buffer cache collections
    CacheGcUs,                ///< time they took, including buffer downloads before deletion
    // Emulated tessellation, GPU time per step of the sampled draws (gpu_profile=1, timestamps).
    TessDrawsSampled,
    TessCaptureGpuNs,
    TessControlGpuNs,
    TessBarriersGpuNs,  ///< transitions of the tessellator's buffers back to unordered access
    TessTessellateGpuNs,
    TessReadbackGpuNs,  ///< barriers after the tessellator and the copy of the record's needs
    TessDrawGpuNs,
    // Draws D3D12 needs extra work for (always counted).
    TessDraws,                ///< draws through the emulated tessellation (compute passes)
    GeneratedGeometryDraws,   ///< draws through a generated geometry shader (lines, points,
                              ///< fill rectangle, polygon points, last provoking vertex)
    StencilFaceSplits,        ///< draws split per face for different stencil references
    SnapshotLevelCopies,      ///< logic op snapshots that copied only stale levels
    SnapshotReuses,           ///< logic op snapshots still current: no copy
    LogicOpCopies,            ///< emulated logic op: destination copied before the draw
    LogicOpCopyBytes,
    DepthFeedbackCopies,      ///< depth images copied so a draw can sample its own target
    QueryBlockingWaits,       ///< guest query reads that waited on the GPU fence
    QueryBlockingWaitUs,
    Count,
};

constexpr size_t NUM_COUNTERS = static_cast<size_t>(Counter::Count);
using Snapshot = std::array<u64, NUM_COUNTERS>;

inline std::array<std::atomic<u64>, NUM_COUNTERS> counters{};
inline std::atomic<bool> detailed_gpu_profile{};
inline std::atomic<bool> force_swap_interval_one{};
/// The swap interval the game last asked for (HardwareComposer), 0 before its first frame.
inline std::atomic<s32> guest_swap_interval{};
/// Guest macro method running on this thread (the GPU thread), 0 outside macros. The
/// bottleneck hunt tags each draw with the macro that issued it.
inline thread_local u32 current_macro{};

constexpr size_t NUM_GUEST_SYNCPOINTS = 256;
struct GuestWaitSiteCounters {
    std::atomic<u64> count{};
    std::atomic<u64> total_us{};
    std::atomic<u64> max_us{};
};
struct GuestWaitSiteSnapshot {
    u64 count{};
    u64 total_us{};
    u64 max_us{};
};
inline std::array<GuestWaitSiteCounters, NUM_GUEST_SYNCPOINTS> guest_wait_sites{};

/// Supervisor calls that ended a JIT run, by SVC number.
constexpr size_t NUM_GUEST_SVCS = 0x100;
inline std::array<std::atomic<u64>, NUM_GUEST_SVCS> guest_svc_exits{};

struct MacroProfileSnapshot {
    u32 method{};
    u64 hash{};
    u64 count{};
    u64 total_ns{};
    u64 nested_ns{};
    u64 max_ns{};
};
inline std::mutex macro_profiles_mutex;
inline std::unordered_map<u32, MacroProfileSnapshot> macro_profiles;

inline void SetDetailedGpuProfile(bool enabled) {
    detailed_gpu_profile.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool DetailedGpuProfileEnabled() {
    return detailed_gpu_profile.load(std::memory_order_relaxed);
}

inline void SetForceSwapIntervalOne(bool enabled) {
    force_swap_interval_one.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool ForceSwapIntervalOne() {
    return force_swap_interval_one.load(std::memory_order_relaxed);
}

inline void Add(Counter counter, u64 value) {
    counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

inline void AddDetailed(Counter counter, u64 value) {
    if (DetailedGpuProfileEnabled()) {
        Add(counter, value);
    }
}

[[nodiscard]] inline u64 ReadCounter(Counter counter) {
    return counters[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
}

inline void RecordMacroProfile(u32 method, u64 hash, u64 total_ns, u64 nested_ns) {
    if (!DetailedGpuProfileEnabled()) {
        return;
    }
    Add(Counter::DmaMacroCalls, 1);
    Add(Counter::DmaMacroNs, total_ns);
    std::scoped_lock lock{macro_profiles_mutex};
    auto& profile = macro_profiles[method];
    profile.method = method;
    profile.hash = hash;
    ++profile.count;
    profile.total_ns += total_ns;
    profile.nested_ns += nested_ns;
    profile.max_ns = std::max(profile.max_ns, total_ns);
}

inline std::vector<MacroProfileSnapshot> TakeMacroProfiles() {
    std::scoped_lock lock{macro_profiles_mutex};
    std::vector<MacroProfileSnapshot> result;
    result.reserve(macro_profiles.size());
    for (const auto& [method, profile] : macro_profiles) {
        result.push_back(profile);
    }
    macro_profiles.clear();
    return result;
}

inline void RecordGuestGpuWait(u32 syncpoint_id, u64 waited_us) {
    Add(Counter::GuestGpuWaits, 1);
    Add(Counter::GuestGpuWaitUs, waited_us);
    if (syncpoint_id >= guest_wait_sites.size()) {
        return;
    }
    auto& site = guest_wait_sites[syncpoint_id];
    site.count.fetch_add(1, std::memory_order_relaxed);
    site.total_us.fetch_add(waited_us, std::memory_order_relaxed);
    u64 previous = site.max_us.load(std::memory_order_relaxed);
    while (previous < waited_us &&
           !site.max_us.compare_exchange_weak(previous, waited_us, std::memory_order_relaxed)) {
    }
}

inline void RecordGuestSvcExit(u32 svc) {
    Add(Counter::GuestExitsSvc, 1);
    if (svc < guest_svc_exits.size()) {
        guest_svc_exits[svc].fetch_add(1, std::memory_order_relaxed);
    }
}

inline std::array<u64, NUM_GUEST_SVCS> TakeGuestSvcExits() {
    std::array<u64, NUM_GUEST_SVCS> result{};
    for (size_t svc = 0; svc < result.size(); ++svc) {
        result[svc] = guest_svc_exits[svc].exchange(0, std::memory_order_relaxed);
    }
    return result;
}

inline std::array<GuestWaitSiteSnapshot, NUM_GUEST_SYNCPOINTS> TakeGuestWaitSites() {
    std::array<GuestWaitSiteSnapshot, NUM_GUEST_SYNCPOINTS> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        auto& site = guest_wait_sites[index];
        result[index] = {.count = site.count.exchange(0, std::memory_order_relaxed),
                         .total_us = site.total_us.exchange(0, std::memory_order_relaxed),
                         .max_us = site.max_us.exchange(0, std::memory_order_relaxed)};
    }
    return result;
}

inline Snapshot Read() {
    Snapshot snapshot{};
    for (size_t i = 0; i < NUM_COUNTERS; ++i) {
        snapshot[i] = counters[i].load(std::memory_order_relaxed);
    }
    return snapshot;
}

inline u64 Get(const Snapshot& snapshot, Counter counter) {
    return snapshot[static_cast<size_t>(counter)];
}

inline u64 ElapsedUs(std::chrono::steady_clock::time_point start) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

/// Adds the scope's duration to a microsecond counter (and 1 to count_counter, if given).
class ScopedTimer {
public:
    explicit ScopedTimer(Counter us_counter_, Counter count_counter_ = Counter::Count)
        : us_counter{us_counter_}, count_counter{count_counter_} {}
    ~ScopedTimer() {
        Add(us_counter, ElapsedUs(start));
        if (count_counter != Counter::Count) {
            Add(count_counter, 1);
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Counter us_counter;
    Counter count_counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

/// Splits a function into consecutive phases: each Lap adds the nanoseconds since the previous one
/// (the phases of a draw last a few microseconds, too short to truncate to microseconds).
class LapTimer {
public:
    LapTimer() : enabled{DetailedGpuProfileEnabled()} {
        if (enabled) {
            last = std::chrono::steady_clock::now();
        }
    }

    void Lap(Counter ns_counter) {
        if (!enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        Add(ns_counter, static_cast<u64>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last)
                                .count()));
        last = now;
    }

private:
    bool enabled{};
    std::chrono::steady_clock::time_point last{};
};

/// Adds the scope's duration to a nanosecond counter.
class ScopedNsTimer {
public:
    explicit ScopedNsTimer(Counter ns_counter_)
        : ns_counter{ns_counter_}, enabled{DetailedGpuProfileEnabled()} {
        if (enabled) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~ScopedNsTimer() {
        if (enabled) {
            Add(ns_counter,
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count()));
        }
    }

    ScopedNsTimer(const ScopedNsTimer&) = delete;
    ScopedNsTimer& operator=(const ScopedNsTimer&) = delete;

private:
    Counter ns_counter;
    bool enabled{};
    std::chrono::steady_clock::time_point start{};
};

} // namespace VideoCore::Perf
