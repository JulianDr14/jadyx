// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Kernel {

template <typename Clear>
std::size_t InitializeUnoptimizedPageRuns(std::span<const std::uint64_t> bitmap,
                                         std::size_t offset, std::size_t count, Clear&& clear) {
    std::size_t initialized = 0;
    for (std::size_t i = 0; i < count;) {
        const auto needs_clear = [&](std::size_t page) {
            return (bitmap[page / 64] & (std::uint64_t{1} << (page % 64))) == 0;
        };
        if (!needs_clear(offset + i)) {
            ++i;
            continue;
        }
        const auto start = i++;
        while (i < count && needs_clear(offset + i)) {
            ++i;
        }
        clear(offset + start, i - start);
        initialized += i - start;
    }
    return initialized;
}

inline void CoalescePageRuns(std::vector<std::pair<std::size_t, std::size_t>>& runs) {
    std::sort(runs.begin(), runs.end());
    std::size_t merged = 0;
    for (const auto& run : runs) {
        if (merged != 0 && runs[merged - 1].first + runs[merged - 1].second == run.first) {
            runs[merged - 1].second += run.second;
        } else {
            runs[merged++] = run;
        }
    }
    runs.resize(merged);
}

template <typename Release>
std::size_t ReleasePageRun(std::size_t offset, std::size_t count, std::size_t slice_pages,
                           std::size_t& budget, Release&& release) {
    std::size_t total = 0;
    for (std::size_t done = 0; budget != 0 && done < count;) {
        const std::size_t pages = (std::min)(count - done, slice_pages);
        const std::size_t released = release(offset + done, pages);
        total += released;
        budget -= (std::min)(budget, released);
        done += pages;
    }
    return total;
}

} // namespace Kernel
