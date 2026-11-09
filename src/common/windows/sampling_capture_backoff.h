// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace Common::SamplingProfiler {
/// A slow Capture cannot be cancelled safely midway. Back off later attempts on that
/// thread only; ordinary and stalled sampling both call this policy after resuming it.
class CaptureBackoff {
public:
    static constexpr std::uint64_t SLOW_NS = 50'000'000;
    static constexpr std::uint64_t FIRST_REST_NS = 1'000'000'000;
    static constexpr std::uint64_t MAX_REST_NS = 8'000'000'000;
    [[nodiscard]] bool Ready(std::uint64_t now) const { return now >= retry_at; }
    void Observe(std::uint64_t now, std::uint64_t elapsed) {
        if (elapsed <= SLOW_NS) {
            rest = 0;
            retry_at = 0;
            return;
        }
        rest = rest == 0 ? FIRST_REST_NS : (std::min)(rest * 2, MAX_REST_NS);
        const auto headroom = (std::numeric_limits<std::uint64_t>::max)() - now;
        retry_at = now + (std::min)(rest, headroom);
    }
private:
    std::uint64_t retry_at{};
    std::uint64_t rest{};
};
} // namespace Common::SamplingProfiler
