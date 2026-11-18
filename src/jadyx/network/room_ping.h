// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ping the room list shows next to each public room, apart from the network so a desktop
// gate can check it. Rooms speak UDP (ENet), so only an ICMP echo can measure them without
// joining; a host whose router drops ICMP shows a dash instead of a number.

#include <cstddef>
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

/// The addresses to measure: each one once (a host may list several rooms), in list order.
inline std::vector<std::string> PingTargets(const std::vector<LobbyRoom>& rooms) {
    std::vector<std::string> targets;
    for (const auto& room : rooms) {
        bool seen = false;
        for (const auto& target : targets) seen = seen || target == room.address;
        if (!seen && !room.address.empty()) targets.push_back(room.address);
    }
    return targets;
}

/// Hands each room the result for its address; rooms whose address was not measured stay as
/// they were.
inline void ApplyPings(std::vector<LobbyRoom>& rooms, const std::map<std::string, std::optional<int>>& results) {
    for (auto& room : rooms) {
        const auto found = results.find(room.address);
        if (found == results.end()) continue;
        room.pinged = true;
        room.ping_ms = found->second;
    }
}

/// What the row shows: "45 ms", a dash when the host did not answer, nothing while measuring.
inline std::wstring PingLabel(const LobbyRoom& room) {
    if (!room.pinged) return {};
    if (!room.ping_ms) return L"\u2014 ms";
    return std::to_wstring(*room.ping_ms) + L" ms";
}

} // namespace Jadyx::Online
