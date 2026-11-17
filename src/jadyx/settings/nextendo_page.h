// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Online tab's Nextendo view (LT/RT, or the switch on its first card, swaps it with Eden
// rooms). Two columns like the rooms view: at the left the account (sign-in, or the profile with
// its state) over the card that sets up play (use Nextendo, the server and NAT addresses, the
// TLS check); at the right the games Nextendo serves, crossed with this library. The library
// copies the service's state while the view shows (library_session_pages.cpp) and the canvas
// draws from this copy; nothing here holds a token.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "jadyx/i18n/localization.h"
#include "jadyx/network/nextendo/catalog_rows.h"
#include "jadyx/network/nextendo/preferences.h"
#include "jadyx/network/nextendo/session.h"

namespace Jadyx {

struct NextendoPage {
    /// The left column's rows: the account's button, then the play card's rows.
    enum Row : std::size_t { Account, UseNextendo, ServerIp, NatIp, TlsMode, Rows };

    std::size_t selected{Account};
    bool in_games{};      ///< The focus is in the games list (the right column).
    std::size_t game{};   ///< The focused game, in `games`.
    std::size_t first{};  ///< The first game drawn (the list scrolls).

    std::uint64_t version = ~std::uint64_t{0}; ///< The service's, for the copy below (none yet).
    std::size_t library_titles{};     ///< Library games with a known title id, when `games` was made.
    Network::Nextendo::Preferences preferences;
    Network::Nextendo::SessionView view;
    std::vector<Network::Nextendo::CatalogRow> games;
};

/// The account is in a usable state: signed in, the tokens asked for or in hand.
inline bool NextendoSignedIn(const NextendoPage& page) {
    using Network::Nextendo::Phase;
    return page.view.phase == Phase::Connecting || page.view.phase == Phase::Ready;
}

/// The next TLS check, as A cycles it: off, log only, enforce.
inline Core::Online::Nextendo::TlsVerifyMode NextTlsMode(Core::Online::Nextendo::TlsVerifyMode mode) {
    using Core::Online::Nextendo::TlsVerifyMode;
    switch (mode) {
    case TlsVerifyMode::Off: return TlsVerifyMode::LogOnly;
    case TlsVerifyMode::LogOnly: return TlsVerifyMode::Enforce;
    case TlsVerifyMode::Enforce: break;
    }
    return TlsVerifyMode::Off;
}

inline I18n::Text TlsModeText(Core::Online::Nextendo::TlsVerifyMode mode) {
    using Core::Online::Nextendo::TlsVerifyMode;
    switch (mode) {
    case TlsVerifyMode::Off: return I18n::Text::NxTlsOff;
    case TlsVerifyMode::LogOnly: return I18n::Text::NxTlsLog;
    case TlsVerifyMode::Enforce: break;
    }
    return I18n::Text::NxTlsEnforce;
}

/// Why the last call failed, for the account card; Count when there is nothing to say.
inline I18n::Text NextendoProblemText(Network::Nextendo::Problem problem) {
    using I18n::Text;
    using Network::Nextendo::Problem;
    switch (problem) {
    case Problem::None: break;
    case Problem::Offline: return Text::NxProblemOffline;
    case Problem::RateLimited: return Text::NxProblemRateLimited;
    case Problem::Unavailable: return Text::NxProblemUnavailable;
    case Problem::SignInCancelled: return Text::NxProblemCancelled;
    case Problem::SignInRejected: return Text::NxProblemRejected;
    case Problem::SessionExpired: return Text::NxProblemExpired;
    case Problem::MissingScope: return Text::NxProblemScope;
    case Problem::Revoked: return Text::NxProblemRevoked;
    case Problem::StorageFailed: return Text::NxProblemStorage;
    }
    return Text::Count;
}

/// Why the account may not play online now.
inline I18n::Text NextendoBlockText(Network::Nextendo::OnlineBlock block) {
    using I18n::Text;
    using Network::Nextendo::OnlineBlock;
    switch (block) {
    case OnlineBlock::Disabled: return Text::NxBlockDisabled;
    case OnlineBlock::Suspended: return Text::NxBlockSuspended;
    case OnlineBlock::Unverified: return Text::NxBlockUnverified;
    case OnlineBlock::DiscordUnlinked: return Text::NxBlockDiscord;
    case OnlineBlock::Elsewhere: return Text::NxBlockElsewhere;
    case OnlineBlock::None:
    case OnlineBlock::Unknown: break;
    }
    return Text::NxBlockUnknown;
}

inline I18n::Text NextendoVerdictText(Network::Nextendo::RowVerdict verdict) {
    using I18n::Text;
    using Network::Nextendo::RowVerdict;
    switch (verdict) {
    case RowVerdict::Ready: return Text::NxVerdictReady;
    case RowVerdict::WrongVersion: return Text::NxVerdictWrongVersion;
    case RowVerdict::NeedsUpdate: return Text::NxVerdictNeedsUpdate;
    case RowVerdict::Disabled: return Text::NxVerdictDisabled;
    case RowVerdict::NotInstalled: break;
    }
    return Text::NxVerdictNotInstalled;
}

/// What to tell the player as a game starts, or Count when nothing (Nextendo off, or all good).
inline I18n::Text LaunchAdviceText(Network::Nextendo::LaunchAdvice advice) {
    using I18n::Text;
    using Network::Nextendo::LaunchAdvice;
    switch (advice) {
    case LaunchAdvice::NotUsingNextendo:
    case LaunchAdvice::Supported: break;
    case LaunchAdvice::NotSignedIn: return Text::NxLaunchSignedOut;
    case LaunchAdvice::NoServer: return Text::NxLaunchNoServer;
    case LaunchAdvice::Blocked: return Text::NxLaunchBlocked;
    case LaunchAdvice::WrongVersion: return Text::NxLaunchWrongVersion;
    case LaunchAdvice::Disabled: return Text::NxLaunchDisabled;
    case LaunchAdvice::UnsupportedClient: return Text::NxLaunchNeedsUpdate;
    case LaunchAdvice::NotListed: return Text::NxLaunchNotListed;
    }
    return Text::Count;
}

} // namespace Jadyx
