// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How the connection to Nextendo is doing while a game plays on it, as the performance HUD shows
// it (the same lines an Eden room shows: ping, its variation, loss). Pure: the samples come from
// link_monitor (ICMP to the game server, or the API's TCP connect time), the gate feeds its own.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Jadyx::Network::Nextendo {

/// What answered the pings: the game server itself, or (when it ignores ICMP) the API's host.
enum class LinkVia { GameServer, Api };

struct LinkSnapshot {
    std::size_t samples = 0;   ///< Pings sent in the window.
    std::size_t answered = 0;  ///< Of those, the ones that came back.
    int round_trip_ms = 0;     ///< Mean of the answered ones.
    int variance_ms = 0;       ///< Mean distance from that mean (jitter).
    double loss_percent = 0;   ///< Unanswered, of the window.
    LinkVia via = LinkVia::GameServer;
};

/// The last `Window` pings.
class LinkStats {
public:
    static constexpr std::size_t Window = 20;

    /// One ping: its round trip, or nullopt when it got no answer in time.
    void Add(std::optional<int> round_trip_ms);
    void Clear();
    LinkSnapshot Snapshot(LinkVia via) const;

private:
    std::vector<std::optional<int>> samples_; // oldest first
};

struct LinkHudLine {
    std::string text;
    bool warning = false;
};

/// The HUD's lines. Its font has capitals, digits and a few signs only, and its lines are
/// Spanish, like the room's.
std::vector<LinkHudLine> LinkHudLines(const LinkSnapshot& snapshot);

/// The menu's status pill: "Nextendo · 34 ms", "Nextendo · API 34 ms", or "Nextendo" while
/// nothing answered yet. `slow` turns its dot amber.
struct LinkPill {
    std::wstring text;
    bool slow = false;
};
LinkPill LinkPillText(const LinkSnapshot& snapshot);

/// Past these, games that sync every frame start to stutter or drop players (as in rooms).
inline bool LinkIsSlow(const LinkSnapshot& snapshot) {
    return snapshot.answered == 0 || snapshot.round_trip_ms > 150 || snapshot.variance_ms > 50 ||
           snapshot.loss_percent >= 2.0;
}

} // namespace Jadyx::Network::Nextendo
