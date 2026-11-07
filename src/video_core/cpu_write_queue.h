// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
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

/// "cpu_write_test=1": one session compares three ways of handling these writes, one phase per
/// pacing window, and logs what each costs. Off, nothing below changes the drain.
enum class TestPhase : u8 { Current, Merged, Off, Count };

inline std::atomic<bool> test_enabled{false};
inline std::atomic<TestPhase> test_phase{TestPhase::Current};

inline void EnableTest(bool enabled) noexcept {
    test_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        SetMode(Mode::On); // The first phase is Current, whatever the settings say.
    }
}

[[nodiscard]] inline bool Testing() noexcept {
    return test_enabled.load(std::memory_order_relaxed);
}

[[nodiscard]] inline TestPhase CurrentTestPhase() noexcept {
    return test_phase.load(std::memory_order_relaxed);
}

/// The next phase. Only the mode changes here: writes already queued drain as usual.
inline void NextTestPhase() noexcept {
    const auto next = static_cast<TestPhase>((static_cast<u8>(CurrentTestPhase()) + 1) %
                                             static_cast<u8>(TestPhase::Count));
    test_phase.store(next, std::memory_order_relaxed);
    SetMode(next == TestPhase::Off ? Mode::Off : Mode::On);
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

struct DrainResult {
    u64 writes{};
    u64 ranges{}; ///< cpu_write_test: runs of writes that touch or overlap
    u64 pages{};  ///< cpu_write_test: runs of writes inside one 4 KiB page, gaps included
};

/// Consumer: calls func(address, size) for each queued write, oldest first per queue.
template <typename Func>
inline DrainResult Drain(Func&& func) {
    DrainResult result{};
    const bool testing = Testing();
    const bool merging = testing && CurrentTestPhase() == TestPhase::Merged;
    for (Queue& q : queues) {
        const u64 tail = q.tail.load(std::memory_order_acquire);
        u64 head = q.head.load(std::memory_order_relaxed);
        result.writes += tail - head;
        u64 run_start = 0;
        u64 run_end = 0;
        u64 page = ~u64{0};
        for (; head != tail; ++head) {
            const Write write = q.writes[head & (CAPACITY - 1)];
            if (!testing) {
                func(write.address, write.size);
                continue;
            }
            const u64 write_end = write.address + write.size;
            if ((write.address >> 12) != page || ((write_end - 1) >> 12) != page) {
                ++result.pages;
                page = write.address >> 12;
            }
            // Only ranges that touch: a gap could hold GPU data this write never reached.
            if (run_start != run_end && write.address <= run_end && write_end >= run_start) {
                run_start = (std::min)(run_start, write.address);
                run_end = (std::max)(run_end, write_end);
            } else {
                ++result.ranges;
                if (merging && run_start != run_end) {
                    func(run_start, run_end - run_start);
                }
                run_start = write.address;
                run_end = write_end;
            }
            if (!merging) {
                func(write.address, write.size);
            }
        }
        if (merging && run_start != run_end) {
            func(run_start, run_end - run_start);
        }
        q.head.store(head, std::memory_order_release);
    }
    return result;
}

} // namespace VideoCommon::CpuWriteQueue
