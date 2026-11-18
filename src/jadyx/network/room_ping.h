// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ping the room list shows next to each public room, apart from the network so a desktop
// gate can check it. It is the round trip of an ENet handshake with the room's own port (the
// console blocks ICMP for apps); a room that is off or does not answer shows a dash.

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "jadyx/network/online_rooms.h"

namespace Jadyx::Online {

/// Up to this round trip a room plays well; up to PingFairMs it plays with some delay.
inline constexpr int PingGoodMs = 80;
inline constexpr int PingFairMs = 150;

enum class PingTone : std::uint8_t { Good, Fair, Poor };

inline PingTone PingToneFor(int ms) {
    return ms <= PingGoodMs ? PingTone::Good : ms <= PingFairMs ? PingTone::Fair : PingTone::Poor;
}

/// What one measurement is filed under: a host may list several rooms on different ports.
inline std::string PingKey(const std::string& address, std::uint16_t port) {
    return address + ":" + std::to_string(port);
}

struct PingTarget {
    std::string address;
    std::uint16_t port{};
};

/// The rooms to measure: each address and port once, in list order.
inline std::vector<PingTarget> PingTargets(const std::vector<LobbyRoom>& rooms) {
    std::vector<PingTarget> targets;
    for (const auto& room : rooms) {
        if (room.address.empty() || room.port == 0) continue;
        bool seen = false;
        for (const auto& target : targets) seen = seen || (target.address == room.address && target.port == room.port);
        if (!seen) targets.push_back({room.address, room.port});
    }
    return targets;
}

/// Hands each room the result filed under its PingKey; rooms not measured stay as they were.
inline void ApplyPings(std::vector<LobbyRoom>& rooms, const std::map<std::string, std::optional<int>>& results) {
    for (auto& room : rooms) {
        const auto found = results.find(PingKey(room.address, room.port));
        if (found == results.end()) continue;
        room.pinged = true;
        room.ping_ms = found->second;
    }
}

/// Once measured, the closest rooms go first: lowest ping up, then rooms that did not answer,
/// then rooms not measured. Ties keep the lobby's order (rooms with people first).
inline void SortByPing(std::vector<LobbyRoom>& rooms) {
    const auto rank = [](const LobbyRoom& room) {
        return !room.pinged ? 2 : room.ping_ms ? 0 : 1;
    };
    std::ranges::stable_sort(rooms, [&](const LobbyRoom& a, const LobbyRoom& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return rank(a) == 0 && *a.ping_ms < *b.ping_ms;
    });
}

/// The same room in a list that changed (refreshed or sorted): matched by address, port and name.
inline std::optional<std::size_t> FindRoom(const std::vector<LobbyRoom>& rooms, const LobbyRoom& room) {
    for (std::size_t i = 0; i < rooms.size(); ++i)
        if (rooms[i].address == room.address && rooms[i].port == room.port && rooms[i].name == room.name) return i;
    return std::nullopt;
}

/// What the row shows: "45 ms", a dash when the room did not answer, nothing while measuring.
inline std::wstring PingLabel(const LobbyRoom& room) {
    if (!room.pinged) return {};
    if (!room.ping_ms) return L"\u2014 ms";
    return std::to_wstring(*room.ping_ms) + L" ms";
}

} // namespace Jadyx::Online
