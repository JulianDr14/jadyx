// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/memory_ceiling.h"

namespace Common {

std::optional<MemoryCeiling> NearestCeiling(const MemoryCeilings& ceilings) noexcept {
    std::optional<MemoryCeiling> nearest;
    for (size_t i = 0; i < ceilings.limits.size(); ++i) {
        const MemoryLimit& limit = ceilings.limits[i];
        if (limit.Known() && (!nearest || limit.Free() < ceilings[*nearest].Free())) {
            nearest = static_cast<MemoryCeiling>(i);
        }
    }
    return nearest;
}

const char* CeilingName(MemoryCeiling ceiling) noexcept {
    switch (ceiling) {
    case MemoryCeiling::App:
        return "app limit";
    case MemoryCeiling::System:
        return "system commit";
    case MemoryCeiling::Gpu:
        return "GPU budget";
    default:
        return "unknown";
    }
}

void HeadroomTrend::Add(u64 time_ms, u64 free) noexcept {
    samples[next] = {time_ms, free};
    next = (next + 1) % SAMPLES;
    if (count < SAMPLES) {
        ++count;
    }
}

std::optional<s64> HeadroomTrend::BytesPerSecond() const noexcept {
    if (count < 2) {
        return std::nullopt;
    }
    const Sample& newest = samples[(next + SAMPLES - 1) % SAMPLES];
    const Sample& oldest = samples[(next + SAMPLES - count) % SAMPLES];
    if (newest.time_ms < oldest.time_ms + 1000) {
        return std::nullopt;
    }
    const s64 change = static_cast<s64>(newest.free) - static_cast<s64>(oldest.free);
    return change * 1000 / static_cast<s64>(newest.time_ms - oldest.time_ms);
}

std::optional<u64> HeadroomTrend::SecondsLeft(u64 free) const noexcept {
    const std::optional<s64> rate = BytesPerSecond();
    if (!rate || *rate >= 0) {
        return std::nullopt;
    }
    return free / static_cast<u64>(-*rate);
}

void MemoryCeilingTrends::Add(u64 time_ms, const MemoryCeilings& ceilings) noexcept {
    for (size_t i = 0; i < trends.size(); ++i) {
        if (ceilings.limits[i].Known()) {
            trends[i].Add(time_ms, ceilings.limits[i].Free());
        } else {
            trends[i].Reset();
        }
    }
}

std::string DescribeMemoryCeilings(const MemoryCeilings& ceilings,
                                   const MemoryCeilingTrends& trends) {
    const std::optional<MemoryCeiling> nearest = NearestCeiling(ceilings);
    if (!nearest) {
        return "nearest ceiling unknown";
    }
    const MemoryLimit& limit = ceilings[*nearest];
    const HeadroomTrend& trend = trends.trends[static_cast<size_t>(*nearest)];
    std::string out = std::string("nearest ceiling is the ") + CeilingName(*nearest) + ": " +
                      std::to_string(limit.Free() >> 20) + " MiB free";
    if (const std::optional<s64> rate = trend.BytesPerSecond()) {
        out += ", " + std::to_string(*rate / (1 << 20)) + " MiB/s";
        if (const std::optional<u64> left = trend.SecondsLeft(limit.Free())) {
            out += ", ~" + std::to_string(*left) + " s left";
        }
    }
    const char* separator = "; ";
    for (size_t i = 0; i < ceilings.limits.size(); ++i) {
        const MemoryLimit& each = ceilings.limits[i];
        if (!each.Known()) {
            continue;
        }
        out += separator;
        out += CeilingName(static_cast<MemoryCeiling>(i));
        out += " " + std::to_string(each.used >> 20) + " of " + std::to_string(each.limit >> 20) +
               " MiB";
        separator = ", ";
    }
    return out;
}

} // namespace Common
