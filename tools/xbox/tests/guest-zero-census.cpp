// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "common/guest_zero_census_scan.h"

using namespace Common;

namespace {

constexpr size_t HOST_PAGE = 4096;
constexpr size_t GRANULE = 2 * 1024 * 1024;

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("FAIL " + what);
}

u8* Commit(u8* begin, size_t size) {
    Check(VirtualAlloc(begin, size, MEM_COMMIT, PAGE_READWRITE) == begin, "commit");
    return begin;
}

void Census() {
    // Five granules: zero, zero but one written byte, half committed, one no-access page, held.
    constexpr size_t SIZE = 5 * GRANULE;
    auto* const base = static_cast<u8*>(VirtualAlloc(nullptr, SIZE, MEM_RESERVE, PAGE_NOACCESS));
    Check(base != nullptr, "reserve");
    Commit(base, GRANULE);
    Commit(base + GRANULE, GRANULE)[GRANULE / 2 + 17] = 1;
    Commit(base + 2 * GRANULE, GRANULE / 2);
    Commit(base + 3 * GRANULE, GRANULE);
    DWORD old{};
    Check(VirtualProtect(base + 3 * GRANULE + 5 * HOST_PAGE, HOST_PAGE, PAGE_NOACCESS, &old) != 0,
          "protect");
    Commit(base + 4 * GRANULE, GRANULE);

    unsigned held = 0;
    const GuestZeroCensus census =
        CensusZeroPages(base, SIZE, GRANULE, [&](const u8* granule, auto&& count) {
            ++held;
            if (granule == base + 4 * GRANULE) {
                return false;
            }
            count();
            return true;
        });
    Check(held == 5, "every granule is offered");
    Check(census.granules == 5 && census.skipped == 1, "one granule skipped");
    Check(census.committed == 3 * GRANULE + GRANULE / 2, "committed bytes");
    Check(census.zero_bytes == 3 * GRANULE + GRANULE / 2 - 2 * HOST_PAGE,
          "zero bytes: the written and the no-access pages are out");
    Check(census.zero_granules == 1, "only the clean granule is all zero");
    const std::string line = DescribeGuestZeroCensus(census);
    Check(line == "7 MiB committed, 6 MiB of it zero pages, 1 of 5 granules all zero (2 MiB), "
                  "1 skipped",
          "description: " + line);

    // A tail shorter than a granule is scanned but never counts as a zero granule.
    const GuestZeroCensus tail =
        CensusZeroPages(base, GRANULE + GRANULE / 2, GRANULE, [](const u8*, auto&& count) {
            count();
            return true;
        });
    Check(tail.granules == 2 && tail.zero_granules == 1 &&
              tail.zero_bytes == GRANULE + GRANULE / 2,
          "partial tail granule");
    Check(VirtualFree(base, 0, MEM_RELEASE) != 0, "release");
    std::cout << "PASS guest zero census: " << line << '\n';
}

} // Anonymous namespace

int main() try {
    Census();
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
