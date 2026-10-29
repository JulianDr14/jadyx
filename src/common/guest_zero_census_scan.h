// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include <windows.h>

#include "common/guest_zero_census.h"

namespace Common {

/// Counts the zero pages of [begin, begin + size), one granule at a time. `scan(granule, f)` runs
/// f with the granule held still (no decommit, no protection change) or returns false to skip it.
/// Only committed read-write pages are read.
template <typename Scan>
GuestZeroCensus CensusZeroPages(const u8* begin, size_t size, size_t granule_size,
                                Scan&& scan) {
    constexpr size_t PAGE = 4096;
    const auto is_zero = [](const u8* page) {
        const auto* const words = reinterpret_cast<const volatile u64*>(page);
        for (size_t i = 0; i < PAGE / sizeof(u64); i += 4) {
            if ((words[i] | words[i + 1] | words[i + 2] | words[i + 3]) != 0) {
                return false;
            }
        }
        return true;
    };
    GuestZeroCensus census{.granule_size = granule_size};
    for (size_t offset = 0; offset < size; offset += granule_size) {
        const u8* const granule = begin + offset;
        const size_t length = (std::min)(granule_size, size - offset);
        ++census.granules;
        u64 committed = 0;
        u64 zero = 0;
        if (!scan(granule, [&] {
                for (const u8* address = granule; address < granule + length;) {
                    MEMORY_BASIC_INFORMATION info{};
                    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
                        break;
                    }
                    const u8* const region_end = (std::min)(
                        static_cast<const u8*>(info.BaseAddress) + info.RegionSize,
                        granule + length);
                    if (info.State == MEM_COMMIT) {
                        committed += static_cast<u64>(region_end - address);
                        if (info.Protect == PAGE_READWRITE) {
                            for (const u8* page = address; page < region_end; page += PAGE) {
                                zero += is_zero(page) ? PAGE : 0;
                            }
                        }
                    }
                    address = region_end;
                }
            })) {
            ++census.skipped;
            continue;
        }
        census.committed += committed;
        census.zero_bytes += zero;
        census.zero_granules += length == granule_size && zero == granule_size;
    }
    return census;
}

} // namespace Common
