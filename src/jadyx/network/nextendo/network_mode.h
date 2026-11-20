// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Eden public rooms and Nextendo both take over the game's network: a joined room carries the
// game's sockets and local wireless, Nextendo redirects DNS and TLS to its servers. Only one may
// be active. Rooms keep working exactly as before while Nextendo is off, so Jadyx stays
// compatible with Eden's public rooms.
//
// The arbiter only decides; the caller performs the steps it returns (leave the room, stop
// Nextendo) and then reports the new mode with Commit.

#include <cstdint>

namespace Jadyx::Network::Nextendo {

enum class NetworkMode : std::uint8_t { Offline, Rooms, Nextendo };

struct ModeChange {
    bool allowed = false;
    bool leave_room = false;    // caller must leave the current room first
    bool stop_nextendo = false; // caller must disable Nextendo first
    NetworkMode next = NetworkMode::Offline;
};

class NetworkModeArbiter {
public:
    /// Plan for switching to wanted. While a game runs the mode is frozen, except going Offline:
    /// the services read the mode per request, so a switch mid-game would mix both networks.
    ModeChange Request(NetworkMode wanted, bool game_running) const {
        ModeChange change;
        change.next = wanted;
        if (wanted == mode_) {
            change.allowed = true;
            return change;
        }
        if (game_running && wanted != NetworkMode::Offline) {
            return change;
        }
        change.allowed = true;
        change.leave_room = mode_ == NetworkMode::Rooms;
        change.stop_nextendo = mode_ == NetworkMode::Nextendo;
        return change;
    }

    void Commit(NetworkMode mode) {
        mode_ = mode;
    }

    NetworkMode Current() const {
        return mode_;
    }

private:
    NetworkMode mode_ = NetworkMode::Offline;
};

} // namespace Jadyx::Network::Nextendo
