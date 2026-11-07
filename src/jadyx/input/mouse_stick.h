// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The mouse as the right stick. Pure logic, so the frontend tests run it on the PC.
//
// The model is Eden's mouse panning (input_common/drivers/mouse.cpp) with its default settings:
// each pixel of motion pushes the stick, a small push is lifted to a minimum so it clears the
// game's own deadzone, and the push decays so the stick re-centers when the mouse stops. Eden
// decays once every 5 ms; here the decay is scaled to the time that passed, so the feel does not
// depend on the poll rate.

#pragma once

#include <algorithm>
#include <cmath>

namespace Jadyx {

class MouseStick {
public:
    /// Eden's defaults: 50 of 100 sensitivity (x 0.001 per pixel), counterweight 20 (x 0.01),
    /// decay strength 18 and minimum decay 6, updated every 5 ms.
    static constexpr float PerPixel = 0.05f;
    static constexpr float Counterweight = 0.20f;
    static constexpr float MaximumRange = 1.5f;
    static constexpr float DecayStrength = 0.18f;
    static constexpr float MinimumDecay = 0.06f;
    static constexpr float StepMs = 5.0f;

    /// Raw relative motion in pixels: x to the right, y down (as the mouse reports it).
    /// `sensitivity_percent`: 100 is Eden's default.
    void Move(float dx, float dy, float sensitivity_percent) {
        const float scale = PerPixel * sensitivity_percent / 100.0f;
        x += dx * scale;
        y += dy * scale;
        const float length = std::hypot(x, y);
        if (length < Counterweight && length != 0.0f) {
            x *= Counterweight / length;
            y *= Counterweight / length;
        }
    }

    struct Position {
        float x{}, y{};
    };
    /// The stick for this poll (x right, y up, inside the unit circle), then the decay over
    /// `elapsed_ms`.
    Position Update(float elapsed_ms) {
        float length = std::hypot(x, y);
        if (length > MaximumRange) {
            x *= MaximumRange / length;
            y *= MaximumRange / length;
            length = MaximumRange;
        }
        Position out{};
        if (length > 0.0f) {
            const float unit = std::min(1.0f, 1.0f / length);
            out = {x * unit, -y * unit};
        }
        const float clamped = std::min(1.0f, length);
        const float decay = std::min(1.0f - MinimumDecay, 1.0f - clamped * clamped * DecayStrength);
        const float factor = std::pow(decay, std::max(0.0f, elapsed_ms) / StepMs);
        x *= factor;
        y *= factor;
        // Below this no game sees it: back to center rather than drifting forever.
        if (std::hypot(x, y) < 0.01f) x = y = 0.0f;
        return out;
    }

    void Reset() { x = y = 0.0f; }
    [[nodiscard]] bool Idle() const { return x == 0.0f && y == 0.0f; }

private:
    float x{}, y{};
};

} // namespace Jadyx
