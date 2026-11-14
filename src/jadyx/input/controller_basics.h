// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the game sees as the controller, how A/B/X/Y map and the stick dead zone. Settings >
// Controller holds the global ones; a game can keep its own from the in-game menu, as its right
// stick and rumble do (an unset field follows Settings).

#pragma once

#include <cstddef>
#include <optional>

#include "jadyx/input/controller_style.h"

namespace Jadyx {

/// Dead zone steps, as in the library's controller panel.
inline constexpr float DeadzoneSteps[] = {0.08f, 0.12f, 0.18f};

/// The next (or, with `back`, the previous) dead zone step; a value off the steps snaps first.
[[nodiscard]] inline float StepDeadzone(float deadzone, bool back) {
    std::size_t i = deadzone < 0.10f ? 0 : deadzone < 0.15f ? 1 : 2;
    i = back ? (i + 2) % 3 : (i + 1) % 3;
    return DeadzoneSteps[i];
}

struct ControllerBasics {
    ConsoleControllerStyle style{ConsoleControllerStyle::Auto};
    bool swap_face_buttons{};
    float deadzone{0.12f};

    bool operator==(const ControllerBasics&) const = default;
};

/// A game's own values; nullopt follows Settings > Controller.
struct GameControllerBasics {
    std::optional<ConsoleControllerStyle> style;
    std::optional<bool> swap_face_buttons;
    std::optional<float> deadzone;

    bool operator==(const GameControllerBasics&) const = default;
};

[[nodiscard]] inline ControllerBasics ResolveControllerBasics(const ControllerBasics& global,
                                                              const GameControllerBasics& game) {
    return {game.style.value_or(global.style), game.swap_face_buttons.value_or(global.swap_face_buttons),
            game.deadzone.value_or(global.deadzone)};
}

} // namespace Jadyx
