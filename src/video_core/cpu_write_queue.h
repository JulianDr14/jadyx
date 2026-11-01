// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "common/common_types.h"
#include "video_core/frame_pipeline.h"

/// "cpu_write_defer=0|1|check": guest CPU writes into rasterizer-cached memory no cache holds GPU
/// data for (VideoCommon::GpuWrittenPages clear) skip RasterizerInterface::OnCPUWrite and its two
/// cache locks. The write is queued here, one queue per emulated core, and the GPU thread applies
/// the queue (the same cache invalidation OnCPUWrite does) before it next uses a cache: a draw, a
/// dispatch, a copy, a DMA, a composite or a frame tick. The GPU thread holds those locks through
/// every draw, so a guest core no longer waits a draw out for each page it writes.
///
/// OnCPUWrite only defers (returns true) where the buffer cache holds GPU-modified data, and every
/// such range is marked in GpuWrittenPages first: a write the bitmap lets through never needed it.
/// A range that became GPU-modified while queued is invalidated at the next InvalidateGPUCache,
/// as OnCPUWrite's deferred writes are. A full queue falls back to OnCPUWrite.
///
/// Check: OnCPUWrite runs as always; writes that would be queued are counted, and those OnCPUWrite
/// then deferred are mismatches (VideoCore::Perf).
namespace VideoCommon::CpuWriteQueue {

using Mode = VideoCore::FramePipeline::Mode;

inline constexpr size_t QUEUES = 4; // Core::Hardware::NUM_CPU_CORES: the last one is guarded
inline constexpr u64 CAPACITY = 4096;
static_assert((CAPACITY & (CAPACITY - 1)) == 0);

struct Write {
    u64 address;
    u64 size;
};

/// One producer (an emulated core, or the system core under its guard), one consumer (the GPU
/// thread).
struct Queue {
    std::array<Write, CAPACITY> writes{};
    alignas(64) std::atomic<u64> tail{};
    alignas(64) std::atomic<u64> head{};
};

inline std::atomic<Mode> mode{Mode::Off};
inline std::array<Queue, QUEUES> queues{};

inline void SetMode(Mode value) noexcept {
    mode.store(value, std::memory_order_relaxed);
}

[[nodiscard]] inline Mode GetMode() noexcept {
    return mode.load(std::memory_order_relaxed);
}

/// Producer. False when the queue is full: the caller takes OnCPUWrite instead.
[[nodiscard]] inline bool Push(size_t queue, u64 address, u64 size) noexcept {
    Queue& q = queues[queue];
    const u64 tail = q.tail.load(std::memory_order_relaxed);
    const u64 head = q.head.load(std::memory_order_acquire);
    if (tail != head) {
        // Covered by the last write still queued (games write a page in many small stores).
        const Write& last = q.writes[(tail - 1) & (CAPACITY - 1)];
        if (address >= last.address && address + size <= last.address + last.size) {
            return true;
        }
    }
    if (tail - head >= CAPACITY) {
        return false;
    }
    q.writes[tail & (CAPACITY - 1)] = {address, size};
    q.tail.store(tail + 1, std::memory_order_release);
    return true;
}

/// Consumer: whether any write is queued.
[[nodiscard]] inline bool Empty() noexcept {
    for (const Queue& q : queues) {
        if (q.tail.load(std::memory_order_acquire) != q.head.load(std::memory_order_relaxed)) {
            return false;
        }
    }
    return true;
}

/// Consumer: calls func(address, size) for each queued write, oldest first per queue. Returns how
/// many there were.
template <typename Func>
inline u64 Drain(Func&& func) {
    u64 drained = 0;
    for (Queue& q : queues) {
        const u64 tail = q.tail.load(std::memory_order_acquire);
        u64 head = q.head.load(std::memory_order_relaxed);
        drained += tail - head;
        for (; head != tail; ++head) {
            const Write& write = q.writes[head & (CAPACITY - 1)];
            func(write.address, write.size);
        }
        q.head.store(head, std::memory_order_release);
    }
    return drained;
}

} // namespace VideoCommon::CpuWriteQueue
