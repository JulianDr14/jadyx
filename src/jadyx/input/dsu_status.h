// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the DSU client (dsu_client.h) knows, as plain values: its servers, each one's pads and the
// player each pad plays. The in-game menu, the library and their PC gates read it without any of
// the client's sockets or threads.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Jadyx {

/// Who a DSU pad plays. Auto takes the next free player once it presses a button; a player
/// number pins it there; GyroOnly sends only its motion (the gyro's source); Off ignores it.
enum class DsuAssign : std::uint8_t {
    Auto = 0,
    Player1, Player2, Player3, Player4, Player5, Player6, Player7, Player8,
    GyroOnly,
    Off,
};
inline constexpr std::size_t DsuAssignCount = 11;

/// The player (1..8) a pinned assignment names, 0 for the others.
[[nodiscard]] constexpr std::uint8_t DsuPinnedPlayer(DsuAssign assign) {
    const auto value = static_cast<std::uint8_t>(assign);
    return value >= 1 && value <= 8 ? value : 0;
}
/// The next assignment, wrapping, for a menu's left and right.
[[nodiscard]] constexpr DsuAssign StepDsuAssign(DsuAssign assign, bool back) {
    const auto value = static_cast<std::size_t>(assign);
    return static_cast<DsuAssign>((value + (back ? DsuAssignCount - 1 : 1)) % DsuAssignCount);
}

/// A pad of a server: ip, port and slot in one number, the same for every component.
[[nodiscard]] constexpr std::uint64_t DsuPadKey(std::uint32_t ipv4, std::uint16_t port, std::uint8_t slot) {
    return (static_cast<std::uint64_t>(ipv4) << 24) | (static_cast<std::uint64_t>(port) << 8) | slot;
}
[[nodiscard]] constexpr std::uint8_t DsuPadSlot(std::uint64_t key) { return static_cast<std::uint8_t>(key & 0xFF); }

enum class DsuServerState : std::uint8_t {
    Connecting, ///< Asked, no pad data yet.
    Connected,  ///< Pad data arrives.
    Lost,       ///< It stopped sending; it keeps being asked.
};

struct DsuPadStatus {
    std::uint64_t key{};
    std::uint8_t slot{};      ///< 0..3, as the server numbers them.
    DsuAssign assign{};
    std::uint8_t player{};    ///< The player it plays now (1..8), 0 for none yet.
    bool motion{};            ///< It sends a gyro.
    bool operator==(const DsuPadStatus&) const = default;
};

struct DsuServerStatus {
    std::wstring address;     ///< "a.b.c.d", ":port" unless the usual one.
    DsuServerState state{DsuServerState::Connecting};
    std::uint32_t rate_hz{};  ///< Samples a second, every pad together.
    std::vector<DsuPadStatus> pads;
    bool operator==(const DsuServerStatus&) const = default;
};

struct DsuStatus {
    std::vector<DsuServerStatus> servers;
    /// The console's address on its network, to compare with the app's.
    std::wstring console_address;
    /// A search is listening for answers now.
    bool searching{};
    /// How many new servers the last search added; none: nothing answered.
    std::optional<std::size_t> last_found;
    /// The pad the gyro follows; 0 is automatic (dsu_client.h's GyroSample).
    std::uint64_t gyro_pad{};
    bool operator==(const DsuStatus&) const = default;

    [[nodiscard]] std::size_t PadCount() const {
        std::size_t count = 0;
        for (const auto& server : servers) count += server.pads.size();
        return count;
    }
    /// The pad and its server for `key`, if it is connected.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> Find(std::uint64_t key) const {
        for (std::size_t s = 0; s < servers.size(); ++s)
            for (std::size_t p = 0; p < servers[s].pads.size(); ++p)
                if (servers[s].pads[p].key == key) return std::pair{s, p};
        return std::nullopt;
    }
    /// The pads that send a gyro, in the servers' order: the gyro page's choices after Auto.
    [[nodiscard]] std::vector<std::uint64_t> MotionPads() const {
        std::vector<std::uint64_t> keys;
        for (const auto& server : servers)
            for (const auto& pad : server.pads)
                if (pad.motion) keys.push_back(pad.key);
        return keys;
    }
};

/// One line of a DSU page under its Search and Add rows: a server, or one of its pads.
struct DsuRowRef {
    std::size_t server{};
    std::optional<std::size_t> pad;
};
[[nodiscard]] inline std::vector<DsuRowRef> DsuRows(const DsuStatus& status) {
    std::vector<DsuRowRef> rows;
    for (std::size_t s = 0; s < status.servers.size(); ++s) {
        rows.push_back({s, std::nullopt});
        for (std::size_t p = 0; p < status.servers[s].pads.size(); ++p) rows.push_back({s, p});
    }
    return rows;
}

} // namespace Jadyx
