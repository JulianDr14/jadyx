// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>
#include <stdexcept>
#include <string>
#include "common/memory_ceiling.h"

using namespace Common;

namespace {

constexpr u64 MiB = 1ULL << 20;

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("FAIL " + what);
}

void Nearest() {
    MemoryCeilings ceilings;
    Check(!NearestCeiling(ceilings), "nothing known");
    ceilings[MemoryCeiling::System] = {7000 * MiB, 9000 * MiB};
    Check(NearestCeiling(ceilings) == MemoryCeiling::System, "only system known");
    ceilings[MemoryCeiling::App] = {4700 * MiB, 5120 * MiB};
    ceilings[MemoryCeiling::Gpu] = {3000 * MiB, 8000 * MiB};
    Check(NearestCeiling(ceilings) == MemoryCeiling::App, "app nearest");
    ceilings[MemoryCeiling::Gpu] = {7900 * MiB, 8000 * MiB};
    Check(NearestCeiling(ceilings) == MemoryCeiling::Gpu, "gpu nearest");
    ceilings[MemoryCeiling::Gpu] = {9000 * MiB, 8000 * MiB};
    Check(ceilings[MemoryCeiling::Gpu].Free() == 0, "over the budget is 0 free");
    std::cout << "PASS nearest ceiling: fewest free bytes among the known limits\n";
}

void Trend() {
    HeadroomTrend trend;
    Check(!trend.BytesPerSecond(), "no rate without samples");
    trend.Add(0, 1000 * MiB);
    Check(!trend.BytesPerSecond(), "no rate with one sample");
    trend.Add(500, 990 * MiB);
    Check(!trend.BytesPerSecond(), "no rate under a second");
    trend.Add(10'000, 650 * MiB);
    Check(trend.BytesPerSecond() == -35 * static_cast<s64>(MiB), "35 MiB/s shrinking");
    Check(trend.SecondsLeft(650 * MiB) == 18, "18 s left");
    for (u64 i = 1; i <= HeadroomTrend::SAMPLES; ++i) {
        trend.Add(10'000 + i * 10'000, 650 * MiB + i * 10 * MiB);
    }
    Check(trend.BytesPerSecond() == static_cast<s64>(MiB), "window drops old samples: +1 MiB/s");
    Check(!trend.SecondsLeft(720 * MiB), "growing headroom has no end");
    trend.Reset();
    Check(!trend.BytesPerSecond(), "reset");

    MemoryCeilings ceilings;
    ceilings[MemoryCeiling::App] = {4000 * MiB, 5120 * MiB};
    MemoryCeilingTrends trends;
    trends.Add(0, ceilings);
    ceilings[MemoryCeiling::App].used = 4708 * MiB;
    ceilings[MemoryCeiling::Gpu] = {3000 * MiB, 8000 * MiB};
    trends.Add(20'000, ceilings);
    const std::string line = DescribeMemoryCeilings(ceilings, trends);
    Check(line == "nearest ceiling is the app limit: 412 MiB free, -35 MiB/s, ~11 s left; "
                  "app limit 4708 of 5120 MiB, GPU budget 3000 of 8000 MiB",
          "description: " + line);
    Check(!trends.trends[static_cast<size_t>(MemoryCeiling::Gpu)].BytesPerSecond(),
          "a limit known for one sample has no rate");
    std::cout << "PASS headroom trend: MiB/s over the last samples, seconds left; " << line
              << '\n';
}

} // Anonymous namespace

int main() try {
    Nearest();
    Trend();
    std::cout << "PASS memory ceiling\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
