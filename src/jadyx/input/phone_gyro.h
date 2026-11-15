// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A phone as the emulated pad's gyro (MotionSource::Phone). A DSU ("Cemuhook") server app on the
// phone sends its motion over the local network; the console looks for it on its own network,
// connects to the one it finds (or to the address the player typed), remembers it, and hands
// each sample to the input thread and the on-screen controller. One worker thread, while wanted.
// The status struct is plain so the in-game menu's header (and its PC gate) can show it.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "jadyx/input/motion_emu.h"

namespace Jadyx {

enum class PhoneGyroState : std::uint8_t {
    Off,        ///< The phone is not the gyro's source.
    Searching,  ///< Looking for a phone on the console's network.
    NotFound,   ///< No phone answered yet; it keeps looking.
    Connecting, ///< Asking the phone at `address` for its motion.
    Connected,  ///< Its motion arrives.
    Lost,       ///< It stopped sending; it keeps asking.
};

struct PhoneGyroStatus {
    PhoneGyroState state{PhoneGyroState::Off};
    /// The phone's address ("192.168.1.23", ":port" if not the usual one); empty while none.
    std::wstring address;
    /// The player typed it: the console asks only it and never looks for another.
    bool typed{};
    /// Samples a second while connected.
    std::uint32_t rate_hz{};
    /// The console's own address on its network, for the player to compare with the phone's.
    std::wstring console_address;
};

namespace PhoneGyro {

/// Starts (true) or stops (false) the worker; cheap to call every poll.
void SetWanted(bool wanted);
/// Looks for a phone again now, forgetting a typed address.
void Search();
/// Uses the phone at `text` ("a.b.c.d" or "a.b.c.d:port") from now on; false if it is not one.
bool ConnectTo(std::wstring_view text);
[[nodiscard]] PhoneGyroStatus Status();
/// Changes whenever Status() does, to redraw a page only then.
[[nodiscard]] std::uint32_t StatusVersion();
/// The latest sample (pad frame, with its orientation) if it is newer than `max_age`.
[[nodiscard]] std::optional<MotionSample> Latest(std::chrono::milliseconds max_age);

} // namespace PhoneGyro
} // namespace Jadyx
