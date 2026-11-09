// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"

/// Diagnostic ("glitch_hunt_menu=1"): follows one guest buffer of fixed-size records from the
/// first dispatch that binds it. A thread reads guest memory about once per millisecond and logs
/// every value change of the watched words, so the log holds what the buffer really held no
/// matter which path wrote it. The paths that can write it add their own lines around those.
/// The CPU line only appears for the first write after a page is protected again: later writes
/// to that page do not reach the emulator, but their values still show as changes.
/// Output: LogDir/glitch/watch_<date>_<time>.txt, read on the PC.
namespace VideoCommon::WriteWatch {

enum class Kind : u8 {
    Cpu,        ///< guest CPU store reached the rasterizer hook
    GpuWrite,   ///< a binding that writes the range was marked as written by the GPU
    Inline,     ///< inline upload through the buffer cache
    DmaCopy,    ///< DMA copy into the range
    DmaClear,   ///< DMA clear of the range
    Download,   ///< GPU bytes landed in guest memory
    BlockWrite, ///< memory manager write (engines, syncpoints, reports)
    Dispatch,   ///< the watched shader was dispatched
    Kept,       ///< a download left a CPU write in place
};

struct Event {
    u64 time_us;
    u64 address;
    u64 size;
    Kind kind;
};

inline std::atomic_bool enabled{};
inline std::atomic<u64> range_start{};
inline std::atomic<u64> range_end{};
inline std::mutex event_mutex;
inline std::vector<Event> events;

[[nodiscard]] inline u64 NowUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline void SetEnabled(bool value) {
    enabled.store(value, std::memory_order_relaxed);
}
[[nodiscard]] inline bool Enabled() {
    return enabled.load(std::memory_order_relaxed);
}

inline void Push(Kind kind, u64 address, u64 size) {
    std::scoped_lock lock{event_mutex};
    if (events.size() < (1U << 20)) {
        events.push_back({NowUs(), address, size, kind});
    }
}

/// Cheap when the watch is off or the write is elsewhere: range_end stays 0 until it starts.
inline void Note(Kind kind, u64 address, u64 size) {
    if (address >= range_end.load(std::memory_order_relaxed) ||
        address + size <= range_start.load(std::memory_order_relaxed)) {
        return;
    }
    Push(kind, address, size);
}

using Reader = std::function<void(u64 address, std::span<u8> out)>;

/// The watched shader is about to run with its buffer at [address, address + size). The first
/// call starts the thread; another range restarts the watch on it. GPU thread only.
void OnDispatch(const Reader& reader, u64 address, u64 size);

/// Ends the thread and closes the log. Before guest memory goes away.
void Stop();

} // namespace VideoCommon::WriteWatch
