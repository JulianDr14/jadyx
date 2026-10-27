// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>

#include "common/common_types.h"

/// Host work outside the renderer that can hold up the game, counted always so the renderer's
/// hitch report can name it. Only slow paths (a file read is a system call) are timed, so the two
/// clock reads and atomic adds per event cost nothing measurable. Totals only grow; the reader
/// diffs them per frame.
namespace Common::HostStalls {

enum class Counter : size_t {
    FileReads,     ///< reads of game files (local or external storage)
    FileReadBytes,
    FileReadUs,
    Count,
};

inline std::array<std::atomic<u64>, static_cast<size_t>(Counter::Count)> counters{};

inline void Add(Counter counter, u64 value) {
    counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

[[nodiscard]] inline u64 Read(Counter counter) {
    return counters[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
}

/// Times one file read; Done passes the bytes it returned.
class FileReadTimer {
public:
    void Done(size_t bytes) const {
        Add(Counter::FileReads, 1);
        Add(Counter::FileReadBytes, bytes);
        Add(Counter::FileReadUs,
            static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count()));
    }

private:
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

} // namespace Common::HostStalls
