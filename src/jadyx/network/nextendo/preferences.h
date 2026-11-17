// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the player chose for Nextendo, and the pure rules around it: which addresses the fields
// accept, when the emulated services may be pointed at Nextendo, and what the catalog says about
// a game before it starts. No platform calls, so the desktop gate covers it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/online/nextendo/catalog.h"
#include "core/online/nextendo/config.h"
#include "jadyx/network/nextendo/session.h"

namespace Jadyx::Network::Nextendo {

struct Preferences {
    /// Online goes through Nextendo instead of Eden rooms. Off by default: rooms work as before.
    bool use_nextendo = false;
    std::string server_ip; // the game servers; empty until the project publishes it
    std::string nat_ip;    // the NAT test servers; empty falls back to server_ip
    Core::Online::Nextendo::TlsVerifyMode tls_mode = Core::Online::Nextendo::TlsVerifyMode::LogOnly;

    bool operator==(const Preferences&) const = default;
};

/// An address typed by the player: spaces trimmed, then empty or a dotted IPv4 literal.
/// nullopt when it is neither (a host name is refused: nothing must depend on the real DNS).
std::optional<std::string> CleanAddress(std::string_view typed);

/// The server address is known: without it Nextendo cannot be used for play.
inline bool EndpointsReady(const Preferences& preferences) {
    return !preferences.server_ip.empty();
}

/// What the emulated services get. Enabled only when the player chose Nextendo, the addresses
/// are set and the session holds a game token; otherwise everything stays as in plain Eden.
Core::Online::Nextendo::Config BuildCoreConfig(const Preferences& preferences,
                                               const std::optional<GameIdentity>& identity);

/// Same rules as the published config, for comparing without the token values.
bool SameCoreConfig(const Core::Online::Nextendo::Config& a, const Core::Online::Nextendo::Config& b);

enum class LaunchAdvice : std::uint8_t {
    NotUsingNextendo, // Nextendo off: nothing to say
    NotSignedIn,      // chosen but no game token (signed out, or still connecting)
    NoServer,         // chosen, but the server address is still empty
    Blocked,          // the account may not play online now (SessionView::block says why)
    Supported,        // listed, enabled and this version is accepted
    WrongVersion,     // listed, but another version is needed (recommended in the verdict)
    Disabled,         // listed but switched off by the project
    UnsupportedClient,// needs a capability this build lacks
    NotListed,        // not in the catalog (or no catalog yet)
};

struct LaunchCheck {
    LaunchAdvice advice = LaunchAdvice::NotUsingNextendo;
    std::string recommended_version;
};

/// What to tell the player when a game starts. installed_version: the game's display version
/// with its update ("1.2.0"), as the catalog lists it.
LaunchCheck CheckLaunch(const Preferences& preferences, const SessionView& view,
                        const Core::Online::Nextendo::GameCatalog& catalog, std::uint64_t title_id,
                        std::string_view installed_version);

} // namespace Jadyx::Network::Nextendo
