// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>

#include "common/common_types.h"

namespace Common {

/// Bytes in use under one limit. A limit of 0 is unknown.
struct MemoryLimit {
    u64 used{};
    u64 limit{};

    [[nodiscard]] bool Known() const noexcept {
        return limit != 0;
    }
    [[nodiscard]] u64 Free() const noexcept {
        return limit > used ? limit - used : 0;
    }
};

/// The limits the process can hit first: the app's memory limit (AppMemoryUsageLimit), the
/// system's commit limit and the GPU's local budget (DXGI).
enum class MemoryCeiling : u8 { App, System, Gpu, Count };

struct MemoryCeilings {
    std::array<MemoryLimit, static_cast<size_t>(MemoryCeiling::Count)> limits{};

    [[nodiscard]] MemoryLimit& operator[](MemoryCeiling ceiling) noexcept {
        return limits[static_cast<size_t>(ceiling)];
    }
    [[nodiscard]] const MemoryLimit& operator[](MemoryCeiling ceiling) const noexcept {
        return limits[static_cast<size_t>(ceiling)];
    }
};

/// The known limit with the fewest free bytes; nullopt when none is known.
[[nodiscard]] std::optional<MemoryCeiling> NearestCeiling(const MemoryCeilings& ceilings) noexcept;

[[nodiscard]] const char* CeilingName(MemoryCeiling ceiling) noexcept;

/// How fast the free bytes under a limit change, from its last samples (one a report).
class HeadroomTrend {
public:
    static constexpr size_t SAMPLES = 7;

    void Add(u64 time_ms, u64 free) noexcept;
    void Reset() noexcept {
        count = 0;
    }

    /// Change of the free bytes per second over the samples kept: negative while they shrink.
    /// nullopt with fewer than two samples a second or more apart.
    [[nodiscard]] std::optional<s64> BytesPerSecond() const noexcept;

    /// Seconds until free reaches 0 at the current rate; nullopt unless it shrinks.
    [[nodiscard]] std::optional<u64> SecondsLeft(u64 free) const noexcept;

private:
    struct Sample {
        u64 time_ms;
        u64 free;
    };
    std::array<Sample, SAMPLES> samples{};
    size_t count{};
    size_t next{};
};

/// Trends of every limit, fed with the same samples.
struct MemoryCeilingTrends {
    std::array<HeadroomTrend, static_cast<size_t>(MemoryCeiling::Count)> trends{};

    void Add(u64 time_ms, const MemoryCeilings& ceilings) noexcept;
};

/// "nearest ceiling is the app limit: 412 MiB free, -35 MiB/s, ~11 s left; app 4708 of 5120 MiB,
/// system ..., GPU ..." (the limits that are known).
[[nodiscard]] std::string DescribeMemoryCeilings(const MemoryCeilings& ceilings,
                                                 const MemoryCeilingTrends& trends);

} // namespace Common
