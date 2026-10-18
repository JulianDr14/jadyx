// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Eased transitions for the frontend. A Tween moves from where it is toward a target over a fixed
// duration along a CSS cubic-bezier curve; retargeting mid-way starts from the current value, so
// quick presses never jump. Screens draw only on change: while a tween runs they ask for frames.

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Jadyx {

/// CSS cubic-bezier(x1, y1, x2, y2) between (0, 0) and (1, 1).
struct Ease {
    float x1, y1, x2, y2;

    float operator()(float t) const {
        if (t <= 0) return 0;
        if (t >= 1) return 1;
        // Solve x(s) = t for the curve parameter s (Newton, then bisection if it stalls).
        float s = t;
        for (int i = 0; i < 6; ++i) {
            const float error = Coordinate(s, x1, x2) - t;
            if (std::abs(error) < 1e-4f) return Coordinate(s, y1, y2);
            const float slope = Slope(s, x1, x2);
            if (std::abs(slope) < 1e-5f) break;
            s = std::clamp(s - error / slope, 0.0f, 1.0f);
        }
        float low = 0, high = 1;
        s = t;
        for (int i = 0; i < 20; ++i) {
            const float x = Coordinate(s, x1, x2);
            if (std::abs(x - t) < 1e-4f) break;
            (x < t ? low : high) = s;
            s = (low + high) / 2;
        }
        return Coordinate(s, y1, y2);
    }

private:
    static float Coordinate(float s, float p1, float p2) {
        // Bernstein form with p0 = 0, p3 = 1.
        const float u = 1 - s;
        return 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s;
    }
    static float Slope(float s, float p1, float p2) {
        const float u = 1 - s;
        return 3 * u * u * p1 + 6 * u * s * (p2 - p1) + 3 * s * s * (1 - p2);
    }
};

/// Focus, scroll and selection: quick out, long settle.
inline constexpr Ease EaseFocus{0.16f, 1.0f, 0.3f, 1.0f};
/// Fades of the hero and the backdrop.
inline constexpr Ease EaseHero{0.22f, 1.0f, 0.36f, 1.0f};

class Tween {
public:
    using Clock = std::chrono::steady_clock;

    Tween() = default;
    Tween(float value, std::chrono::milliseconds duration, Ease ease)
        : from{value}, to{value}, duration{duration}, ease{ease} {}

    /// Heads for `target` from the current value; a no-op when it already does.
    void Set(float target, Clock::time_point now) {
        if (target == to) return;
        from = Value(now);
        to = target;
        start = now;
    }
    /// Jumps to `target`, as on a screen's first frame.
    void Snap(float target) {
        from = to = target;
        start = {};
    }
    float Value(Clock::time_point now) const {
        if (from == to) return to;
        const float t = std::chrono::duration<float>(now - start) / duration;
        return t >= 1 ? to : from + (to - from) * ease(t);
    }
    bool Running(Clock::time_point now) const {
        return from != to && now - start < duration;
    }
    float Target() const {
        return to;
    }

private:
    float from{}, to{};
    Clock::time_point start{};
    std::chrono::duration<float> duration{std::chrono::milliseconds{220}};
    Ease ease{EaseFocus};
};

} // namespace Jadyx
