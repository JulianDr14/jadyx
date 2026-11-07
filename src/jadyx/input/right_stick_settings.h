// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The right stick's settings: axis inversion, sensitivity for a pad (and the keyboard's stick
// keys), and the mouse as the right stick with its own sensitivity. Settings > Controller holds
// the global ones; a game can keep its own over any of them from the in-game menu, as its
// graphics do (an unset field follows Settings).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <span>

namespace Jadyx {

/// Sensitivity steps, in percent. 100 is the stick as it is (and, for the mouse, Eden's default).
inline constexpr std::uint8_t StickSensitivitySteps[] = {50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200};
inline constexpr std::uint8_t MouseSensitivitySteps[] = {25, 50, 75, 100, 125, 150, 175, 200, 250};

struct RightStickSettings {
    bool invert_x{};
    bool invert_y{};
    std::uint8_t stick_sensitivity{100};
    bool mouse{};
    std::uint8_t mouse_sensitivity{100};

    bool operator==(const RightStickSettings&) const = default;
};

/// A game's own values; nullopt follows Settings > Controller.
struct GameRightStick {
    std::optional<bool> invert_x;
    std::optional<bool> invert_y;
    std::optional<std::uint8_t> stick_sensitivity;
    std::optional<bool> mouse;
    std::optional<std::uint8_t> mouse_sensitivity;

    bool operator==(const GameRightStick&) const = default;
};

[[nodiscard]] inline RightStickSettings ResolveRightStick(const RightStickSettings& global, const GameRightStick& game) {
    return {game.invert_x.value_or(global.invert_x), game.invert_y.value_or(global.invert_y),
            game.stick_sensitivity.value_or(global.stick_sensitivity), game.mouse.value_or(global.mouse),
            game.mouse_sensitivity.value_or(global.mouse_sensitivity)};
}

[[nodiscard]] inline bool IsSensitivityStep(std::span<const std::uint8_t> steps, int value) {
    return std::find(steps.begin(), steps.end(), value) != steps.end();
}

/// The next (or, with `back`, the previous) step, wrapping; a value off the steps snaps to the
/// nearest one first.
[[nodiscard]] inline std::uint8_t StepSensitivity(std::span<const std::uint8_t> steps, std::uint8_t value, bool back) {
    std::size_t at = 0;
    for (std::size_t i = 1; i < steps.size(); ++i)
        if (std::abs(int{steps[i]} - value) < std::abs(int{steps[at]} - value)) at = i;
    return steps[back ? (at + steps.size() - 1) % steps.size() : (at + 1) % steps.size()];
}

/// A game's value after the in-game menu stepped what it resolves to: landing on Settings' own
/// value follows Settings again, any other is the game's.
template <typename T>
[[nodiscard]] std::optional<T> GameOverride(T stepped, T global) {
    return stepped == global ? std::nullopt : std::optional<T>{stepped};
}

/// Sensitivity on a stick position: scales its length, capped at the edge.
inline void ScaleStick(float& x, float& y, std::uint8_t percent) {
    if (percent == 100) return;
    const float scale = percent / 100.0f;
    x *= scale;
    y *= scale;
    const float length = std::hypot(x, y);
    if (length > 1.0f) {
        x /= length;
        y /= length;
    }
}

/// Flips the right stick the game sees; applied after every source (pad, keyboard, mouse).
inline void InvertStick(float& x, float& y, bool invert_x, bool invert_y) {
    if (invert_x) x = -x;
    if (invert_y) y = -y;
}

} // namespace Jadyx
