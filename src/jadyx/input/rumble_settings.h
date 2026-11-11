// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's rumble on the real pad: on or off, and how strong. Settings > Controller holds the
// global ones; a game can keep its own from the in-game menu (an unset field follows Settings).

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Jadyx {

/// Strength steps, in percent of what the game asks. Over 100 lifts a weak rumble; the pad's
/// motors still top out at full.
inline constexpr std::uint8_t RumbleStrengthSteps[] = {25, 50, 75, 100, 125, 150, 200};

struct RumbleSettings {
    bool on{true};
    std::uint8_t strength{100};

    bool operator==(const RumbleSettings&) const = default;
};

/// A game's own values; nullopt follows Settings > Controller.
struct GameRumble {
    std::optional<bool> on;
    std::optional<std::uint8_t> strength;

    bool operator==(const GameRumble&) const = default;
};

[[nodiscard]] inline RumbleSettings ResolveRumble(const RumbleSettings& global, const GameRumble& game) {
    return {game.on.value_or(global.on), game.strength.value_or(global.strength)};
}

/// One motor's speed for the pad, 0 to 1, from the game's amplitude.
[[nodiscard]] inline float RumbleMotor(const RumbleSettings& rumble, float amplitude) {
    if (!rumble.on || !(amplitude > 0.0f)) return 0.0f;
    return std::clamp(amplitude * rumble.strength / 100.0f, 0.0f, 1.0f);
}

} // namespace Jadyx
