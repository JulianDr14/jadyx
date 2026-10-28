// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;

/// Reuses the DEFAULT-heap buffers that copies go through (texture reinterpretations, plane
/// splits, row repacks; copies within one cached buffer) instead of creating a committed
/// resource for every copy and layer. One pool, owned by the renderer, serves both caches.
///
/// A released buffer is handed out again once the GPU has finished the submission that used it.
/// It is in COMMON then, like a new one: buffers decay to COMMON at the end of every
/// ExecuteCommandLists. Callers therefore keep the barriers they record for a fresh buffer.
class TransferBufferPool {
public:
    explicit TransferBufferPool(const Device& device, Scheduler& scheduler);
    ~TransferBufferPool();

    TransferBufferPool(const TransferBufferPool&) = delete;
    TransferBufferPool& operator=(const TransferBufferPool&) = delete;

    /// A buffer of at least size bytes in COMMON (with UAV access when unordered_access). user, a
    /// string literal, names the caller in the gpu_profile report.
    [[nodiscard]] ComPtr<ID3D12Resource> Acquire(u64 size, bool unordered_access = false,
                                                 const char* user = "other");

    /// gpu_profile only: acquisitions and misses per user since the last call, as one line.
    [[nodiscard]] std::string TakeUserReport();

    /// Returns a buffer once the commands recorded so far are done with it. Buffers past the pool's
    /// budget are released instead.
    void Release(ComPtr<ID3D12Resource>&& buffer);

    /// Frees idle buffers: all of them under memory pressure, otherwise the ones unused for a while.
    void Trim(bool under_pressure);

    /// Under memory pressure, once per frame: frees the buffers none of the last few frames used
    /// but keeps the ones copies cycle through every frame. In an emergency only the last frame's
    /// count, up to EMERGENCY_POOLED_BYTES.
    void TrimStale(bool emergency);

    [[nodiscard]] u64 PooledBytes() const;

    /// Bytes of buffers released past the pool's budget while recording the current submission:
    /// they stay allocated until it completes, so the recorder submits once they add up.
    [[nodiscard]] u64 PendingReleaseBytes() const;

    /// After the commit limit refused a resource of size bytes: frees what finished work no longer
    /// needs, so the caller can try once more. False (nothing freed) off the recording thread.
    bool ReclaimAfterOutOfMemory(u64 size, const char* what);

    /// What else to free when the commit limit refuses a buffer, after the pool's own buffers and
    /// the finished work's deferred releases (the texture cache's emptied heaps).
    void SetReclaimer(std::function<void()> reclaimer_) {
        reclaimer = std::move(reclaimer_);
    }

private:
    struct Entry {
        ComPtr<ID3D12Resource> buffer;
        u64 size;
        bool unordered_access;
        u64 tick; ///< scheduler tick of the last submission that used it
    };

    /// Idle buffers above this are released: the pool must not grow into the texture budget.
    static constexpr u64 MAX_POOLED_BYTES = 64ULL << 20;
    /// Submissions an idle buffer is kept for when there is no memory pressure.
    static constexpr u64 IDLE_TICKS = 2048;
    /// Frames an idle buffer is kept for under memory pressure. Emptying the pool every frame
    /// instead made a game recreate ~11 buffers per frame (~6 MiB, ~20 ms of the GPU thread).
    /// Counted in submissions (16) it was about one frame of a scene that submits a dozen times
    /// per frame, while the GPU still held the buffers of the frame before: ~7 buffers were
    /// created per frame again, 2.5% of the GPU thread plus their releases.
    static constexpr size_t STALE_FRAMES = 4;
    /// Idle bytes kept in an emergency: a frame of copies needs a few MiB, and emptying the pool
    /// recreated them every frame of a scene that sits near the commit limit.
    static constexpr u64 EMERGENCY_POOLED_BYTES = 16ULL << 20;

    struct UserStats {
        u64 acquires = 0;
        u64 misses = 0;
        u64 miss_bytes = 0;
    };

    const Device& device;
    Scheduler& scheduler;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::map<std::string_view, UserStats> user_stats;
    u64 pooled_bytes{};
    u64 pending_release_bytes{};
    u64 pending_release_tick{};
    std::function<void()> reclaimer;
    /// The tick at each of the last STALE_FRAMES TrimStale calls, oldest at stale_cursor.
    std::array<u64, STALE_FRAMES> stale_frame_ticks{};
    size_t stale_cursor{};

    void TrimOlderThan(u64 idle_ticks);
    void TrimUnusedSince(u64 tick, u64 budget);
};

} // namespace D3D12
