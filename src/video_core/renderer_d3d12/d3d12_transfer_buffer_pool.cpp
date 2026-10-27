// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"

namespace D3D12 {

namespace {

/// Buffers are placed at 64 KiB anyway: sizing them so lets similar copies share one.
constexpr u64 SIZE_GRANULARITY = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

} // Anonymous namespace

TransferBufferPool::TransferBufferPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

TransferBufferPool::~TransferBufferPool() = default;

ComPtr<ID3D12Resource> TransferBufferPool::Acquire(u64 size, bool unordered_access,
                                                   const char* user) {
    size = Common::AlignUp(std::max<u64>(size, 1), SIZE_GRANULARITY);
    {
        std::scoped_lock lock{mutex};
        UserStats* const stats =
            VideoCore::Perf::DetailedGpuProfileEnabled() ? &user_stats[user] : nullptr;
        if (stats) {
            ++stats->acquires;
        }
        // Best fit among the idle buffers, and at most twice the size so a large buffer is not
        // tied up by small copies.
        auto best = entries.end();
        bool busy_fit = false;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->size < size || it->size > size * 2 ||
                (unordered_access && !it->unordered_access)) {
                continue;
            }
            if (!scheduler.IsFree(it->tick)) {
                busy_fit = true;
                continue;
            }
            if (best == entries.end() || it->size < best->size) {
                best = it;
            }
        }
        if (best != entries.end()) {
            ComPtr<ID3D12Resource> buffer = std::move(best->buffer);
            pooled_bytes -= best->size;
            *best = std::move(entries.back());
            entries.pop_back();
            VideoCore::Perf::Add(VideoCore::Perf::Counter::TransferPoolHits, 1);
            return buffer;
        }
        VideoCore::Perf::Add(busy_fit ? VideoCore::Perf::Counter::TransferPoolMissBusy
                                      : VideoCore::Perf::Counter::TransferPoolMissNoFit,
                             1);
        VideoCore::Perf::Add(VideoCore::Perf::Counter::TransferPoolMissBytes, size);
        if (stats) {
            ++stats->misses;
            stats->miss_bytes += size;
        }
    }
    HRESULT hr = S_OK;
    const auto create = [&] {
        return CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATE_COMMON,
                                     unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                      : D3D12_RESOURCE_FLAG_NONE,
                                     "Create transfer buffer", &hr);
    };
    ComPtr<ID3D12Resource> buffer = create();
    if (!buffer && hr == E_OUTOFMEMORY && ReclaimAfterOutOfMemory(size, "transfer buffer")) {
        buffer = create();
        if (buffer) {
            LOG_INFO(Render, "D3D12: transfer buffer created after reclaiming");
        }
    }
    if (!buffer) {
        ThrowIfFailed(hr, "Create transfer buffer");
    }
    return buffer;
}

bool TransferBufferPool::ReclaimAfterOutOfMemory(u64 size, const char* what) {
    if (!scheduler.IsRecordingThread()) {
        return false;
    }
    // The commit limit refused it, typically while a loading screen fills the caches. Closing
    // the game for it lost the session; give back what finished work no longer needs (idle
    // pooled buffers, deferred releases, then the heaps they emptied). Waiting only on
    // submitted ticks never splits the list being recorded.
    LOG_WARNING(Render, "D3D12: out of memory creating a {} byte {}; reclaiming and retrying",
                size, what);
    Trim(true);
    const u64 current = scheduler.CurrentTick();
    if (current > 1) {
        scheduler.Wait(current - 1);
    }
    scheduler.CollectGarbage();
    if (reclaimer) {
        reclaimer();
    }
    return true;
}

void TransferBufferPool::Release(ComPtr<ID3D12Resource>&& buffer) {
    if (!buffer) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    {
        std::scoped_lock lock{mutex};
        if (pooled_bytes + desc.Width <= MAX_POOLED_BYTES) {
            pooled_bytes += desc.Width;
            entries.push_back({
                .buffer = std::move(buffer),
                .size = desc.Width,
                .unordered_access = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
                .tick = scheduler.CurrentTick(),
            });
            return;
        }
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TransferPoolOverflows, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TransferPoolOverflowBytes, desc.Width);
    scheduler.DeferRelease(std::move(buffer));
}

void TransferBufferPool::Trim(bool under_pressure) {
    TrimOlderThan(under_pressure ? 0 : IDLE_TICKS);
}

void TransferBufferPool::TrimStale() {
    TrimOlderThan(STALE_TICKS);
}

void TransferBufferPool::TrimOlderThan(u64 idle_ticks) {
    const u64 current = scheduler.CurrentTick();
    std::scoped_lock lock{mutex};
    std::erase_if(entries, [&](Entry& entry) {
        if (idle_ticks != 0 && current <= entry.tick + idle_ticks) {
            return false;
        }
        pooled_bytes -= entry.size;
        // Not necessarily done on the GPU yet: release it after the work recorded so far.
        scheduler.DeferRelease(std::move(entry.buffer));
        return true;
    });
}

std::string TransferBufferPool::TakeUserReport() {
    std::scoped_lock lock{mutex};
    std::string report;
    for (const auto& [user, stats] : user_stats) {
        report += fmt::format("{}{} {} ({} created, {:.1f} MiB)", report.empty() ? "" : "; ",
                              user, stats.acquires, stats.misses,
                              static_cast<double>(stats.miss_bytes) / (1 << 20));
    }
    user_stats.clear();
    return report;
}

u64 TransferBufferPool::PooledBytes() const {
    std::scoped_lock lock{mutex};
    return pooled_bytes;
}

} // namespace D3D12
