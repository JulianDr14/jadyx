// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

namespace Common {

/// How much of a committed range holds only zeros: the pages the guest reads as zero but that
/// still take commit, and the granules made of nothing else.
struct GuestZeroCensus {
    u64 committed{};     ///< Bytes committed, readable or not.
    u64 zero_bytes{};    ///< Readable committed pages that are all zero.
    u64 zero_granules{}; ///< Granules fully committed, readable and zero.
    u64 granules{};      ///< Granules scanned.
    u64 skipped{};       ///< Granules left out (on disk or on probation).
    u64 granule_size{};
};

inline std::string DescribeGuestZeroCensus(const GuestZeroCensus& census) {
    return std::to_string(census.committed >> 20) + " MiB committed, " +
           std::to_string(census.zero_bytes >> 20) + " MiB of it zero pages, " +
           std::to_string(census.zero_granules) + " of " + std::to_string(census.granules) +
           " granules all zero (" +
           std::to_string((census.zero_granules * census.granule_size) >> 20) + " MiB), " +
           std::to_string(census.skipped) + " skipped";
}

} // namespace Common
