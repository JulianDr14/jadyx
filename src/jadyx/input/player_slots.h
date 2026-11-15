// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace Jadyx {
/// Players a Switch connects at once (Player1..Player8); the Series pairs up to eight pads too.
inline constexpr std::size_t MaxPlayers = 8;

// Which pad plays each of players 2-8. Player 1 stays the selected pad (uwp_input.h); every other
// pad takes the lowest free player and keeps it while connected, so a pad dropping out never
// renumbers the others mid-game. Pure policy, testable on the host.
class PlayerSlots {
public:
    /// `ids`: the connected pads other than player 1's, in discovery order. Returns the players
    /// (indices 1..7) whose pad changed: freed, taken, or both.
    std::vector<std::size_t> Assign(const std::vector<std::wstring>& ids) { return Assign(ids, {}); }

    /// The same, with players chosen by hand: `pinned[player]` names the pad that must play it
    /// (a DSU pad set to "Player 3"), if it is among `ids`; it takes that player from whoever had
    /// it, who moves to the lowest free one. Index 0 is ignored.
    std::vector<std::size_t> Assign(const std::vector<std::wstring>& ids,
                                    const std::array<std::wstring, MaxPlayers>& pinned) {
        const auto present = [&](const std::wstring& id) {
            return !id.empty() && std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        const std::array<std::wstring, MaxPlayers> before = slots;
        std::array<std::wstring, MaxPlayers> next{};
        for (std::size_t player = 1; player < MaxPlayers; ++player)
            if (present(pinned[player])) next[player] = pinned[player];
        const auto placed = [&](const std::wstring& id) { return std::find(next.begin(), next.end(), id) != next.end(); };
        // Everyone else keeps the player they had, if it is still free.
        for (std::size_t player = 1; player < MaxPlayers; ++player) {
            const auto& id = before[player];
            if (next[player].empty() && present(id) && !placed(id)) next[player] = id;
        }
        for (const auto& id : ids) {
            if (id.empty() || placed(id)) continue;
            const auto free = std::find_if(next.begin() + 1, next.end(),
                                           [](const std::wstring& slot) { return slot.empty(); });
            if (free == next.end()) break; // More pads than players: the extra ones wait.
            *free = id;
        }
        slots = next;
        std::vector<std::size_t> changed;
        for (std::size_t player = 1; player < MaxPlayers; ++player)
            if (slots[player] != before[player]) changed.push_back(player);
        return changed;
    }
    /// The pad id playing `player` (0-based), empty when nobody does. Index 0 is never assigned.
    const std::wstring& Id(std::size_t player) const { return slots[player]; }

private:
    std::array<std::wstring, MaxPlayers> slots;
};

/// The pad automatic selection gives player 1: the one it has while that can still play, so a
/// reordered pad list never swaps players mid-game; else the lowest other player's, so the rest
/// keep their numbers; else empty, any pad. `playable(id)` says whether that pad can play now.
template <typename Playable>
std::wstring PreferredPlayerOne(const std::wstring& current, const PlayerSlots& slots,
                                Playable playable) {
    if (!current.empty() && playable(current)) return current;
    for (std::size_t player = 1; player < MaxPlayers; ++player) {
        if (!slots.Id(player).empty() && playable(slots.Id(player))) return slots.Id(player);
    }
    return {};
}
} // namespace Jadyx
