// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>

#include "common/common_types.h"

namespace D3D12 {

/// Spaces background prewarm PSOs: after one is built, the next waits REST times as long as the
/// build took. In game a prewarm PSO costs one of the console's three cores for ~80-650 ms;
/// queued back to back the thread used ~35% of a core all through gameplay (0.3.9.112 profile),
/// next to a GPU thread that is the frame's bottleneck. Resting 3x the build keeps it near 8%.
/// On a loading screen (pso_loading) nothing is drawn that a build could slow: rest is 0.
struct PrewarmPacer {
    using Clock = std::chrono::steady_clock;
    static constexpr int REST = 3;

    void Queued(Clock::time_point now) noexcept {
        queued_at = now;
        observed_built = false;
    }

    /// GPU thread: observe completion even when memory policy or the prewarm switch prevents
    /// another queue. Otherwise time spent blocked is mistaken for time spent building.
    /// This remains a frame-boundary observation, not the worker's exact completion timestamp.
    void ObserveBuilt(Clock::time_point now) noexcept {
        if (!observed_built) {
            built_at = now;
            observed_built = true;
        }
    }

    /// Call only once the last queued PSO is built. ObserveBuilt may already have recorded it
    /// while queuing was blocked; repeated observations must not restart its rest period.
    [[nodiscard]] bool Rested(Clock::time_point now, int minimum_rest = 0) noexcept {
        ObserveBuilt(now);
        return now - built_at >= (built_at - queued_at) * (std::max)(rest, minimum_rest);
    }

    Clock::time_point queued_at{};
    Clock::time_point built_at{};
    bool observed_built{};
    int rest{REST};
};

/// Tells a loading screen from gameplay, once a composite, from the draws since the previous one:
/// fewer than FEW_DRAWS for FEW_DRAWS_FOR, or a composite after a gap of NO_FRAME_FOR that drew
/// little. A frame that draws a lot ends it at once, so a long gameplay stall followed by a full
/// frame is never taken for one.
struct LoadingDetector {
    using Clock = std::chrono::steady_clock;
    static constexpr u64 FEW_DRAWS = 200;
    static constexpr auto FEW_DRAWS_FOR = std::chrono::milliseconds{500};
    static constexpr auto NO_FRAME_FOR = std::chrono::milliseconds{300};

    /// Returns whether the game is on a loading screen.
    bool Tick(Clock::time_point now, u64 draws) noexcept {
        const bool gap = last_tick != Clock::time_point{} && now - last_tick >= NO_FRAME_FOR;
        last_tick = now;
        if (draws >= FEW_DRAWS) {
            few_since = {};
            loading = false;
            return loading;
        }
        if (few_since == Clock::time_point{}) {
            few_since = now;
        }
        loading = gap || loading || now - few_since >= FEW_DRAWS_FOR;
        return loading;
    }

    Clock::time_point last_tick{};
    Clock::time_point few_since{};
    bool loading{};
};

} // namespace D3D12
