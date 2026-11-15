// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pads over the network from DSU ("Cemuhook") servers: an app on a phone, or a PC program that
// shares a DualSense, Joy-Cons or any pad it reads. The console keeps a list of servers (found on
// its network or typed), asks each for its pads, and hands every pad's buttons, sticks and motion
// to the input thread, which makes them players (uwp_input.cpp) or the gyro's source. Several
// servers at once, four pads each. One worker thread while anyone wants it; the list, each pad's
// player and the gyro's pad are kept in the app's settings.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "jadyx/input/dsu_protocol.h"
#include "jadyx/input/dsu_status.h"
#include "jadyx/input/motion_emu.h"

namespace Jadyx::DsuClient {

/// Who keeps the worker running: it runs while any of them does.
enum class Wanter : std::uint8_t { Input = 1, Menu = 2, Library = 4 };
/// Starts or stops wanting the worker; cheap to call every poll.
void SetWanted(Wanter who, bool wanted);
/// Servers are saved: a game's input keeps the worker running so their pads play.
[[nodiscard]] bool HasServers();

/// Looks on the console's network now and adds every server that answers.
void Search();
/// Adds the server at `text` ("a.b.c.d" or "a.b.c.d:port"); false if it is not an address.
bool Add(std::wstring_view text);
/// Forgets the `index`th server of Status().
void Remove(std::size_t index);
/// Who the pad `key` plays, kept for when it comes back.
void SetAssign(std::uint64_t key, DsuAssign assign);
/// The pad whose motion the gyro follows; 0 is automatic.
void SetGyroPad(std::uint64_t key);

[[nodiscard]] DsuStatus Status();
/// Changes whenever Status() does, to redraw a page only then.
[[nodiscard]] std::uint32_t StatusVersion();

/// A connected pad as the input thread reads it.
struct PadFrame {
    std::uint64_t key{};
    DsuAssign assign{};
    Dsu::PadInput input;
    /// Its latest motion (pad frame), if it sends one.
    std::optional<MotionSample> motion;
    /// Samples since the last Pads(): the motion is new.
    bool fresh{};
};
/// The pads that sent something within `max_age`, in the servers' order.
[[nodiscard]] std::vector<PadFrame> Pads(std::chrono::milliseconds max_age);
/// The input thread says which player the pad plays (0 none), for the menus.
void SetResolved(std::uint64_t key, std::uint8_t player);
/// The gyro's sample: the chosen pad's; else the first "gyro only" pad's, the one playing
/// player 1, or any with motion. None while no pad sends it within `max_age`.
[[nodiscard]] std::optional<MotionSample> GyroSample(std::chrono::milliseconds max_age);

} // namespace Jadyx::DsuClient
