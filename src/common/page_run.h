// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace Common {

/// True when guest pages [first_page, last_page] can be read through one host pointer.
///
/// The page table stores, per page, the host pointer minus the guest address. Pages of one host
/// contiguous run therefore store the same value, whichever mapping call wrote them, so comparing
/// that value page by page proves contiguity without a mapping id. Each page must also be one the
/// caller can address directly (is_direct), which rules out unmapped and debug pages.
///
/// Entries needs operator[] returning something with Raw(), whose result has a page field.
template <typename Entries, typename IsDirect>
[[nodiscard]] constexpr bool IsHostContiguousRun(const Entries& entries, std::uint64_t first_page,
                                                 std::uint64_t last_page, IsDirect&& is_direct) {
    const auto first = entries[first_page].Raw();
    if (!is_direct(first)) {
        return false;
    }
    for (std::uint64_t page = first_page + 1; page <= last_page; ++page) {
        const auto raw = entries[page].Raw();
        if (raw.page != first.page || !is_direct(raw)) {
            return false;
        }
    }
    return true;
}

} // namespace Common
