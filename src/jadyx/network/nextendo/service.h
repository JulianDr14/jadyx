// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The app's Nextendo service: one worker thread drives the account Session (session.h), the
// refresh token lives in the PasswordVault, the signed catalog in LocalCache, the choices in
// LocalSettings. After every change the worker publishes the emulated services' config
// (core/online/nextendo/config.h), so a game sees Nextendo only while it is chosen, the server
// address is set and the account holds a game token.
//
// Everything here is safe to call from any thread and returns at once.
//
// Eden rooms and Nextendo exclude each other (network_mode.h): choosing Nextendo leaves a joined
// room, and RoomsAllowed() tells the Online tab to refuse joining or hosting while it is chosen.

#include <cstdint>
#include <string>
#include <string_view>

#include "jadyx/network/nextendo/preferences.h"
#include "jadyx/network/nextendo/session.h"

namespace Jadyx::Network::Nextendo {

/// Starts the worker: a stored sign-in resumes and the cached catalog loads. Once, at start.
void StartWorker();
/// Stops the worker and turns Nextendo off for the emulated services. The app is closing; the
/// server drops the presence by itself 90 s after the last one.
void StopWorker();

/// The session as last published (a copy).
SessionView ServiceView();
/// The verified catalog as last published (a copy).
Core::Online::Nextendo::GameCatalog ServiceCatalog();
/// Changes whenever ServiceView() or ServicePreferences() would: a cheap redraw check.
std::uint64_t ServiceVersion();

Preferences ServicePreferences();
/// Saves and applies the addresses and the TLS mode (use_nextendo is kept as it is).
void SetEndpoints(const std::string& server_ip, const std::string& nat_ip,
                  Core::Online::Nextendo::TlsVerifyMode tls_mode);

/// Chooses Nextendo (true) or Eden rooms (false). Choosing Nextendo leaves a joined room first.
/// Refused (false) while a game runs.
bool ChooseNextendo(bool on);
/// Eden rooms may be joined or hosted: Nextendo is not the chosen network.
bool RoomsAllowed();

/// The Nextendo view of the Online tab shows. Until the player opens it, chooses Nextendo or
/// signs in, the worker sends nothing to Nextendo (not even the public catalog download).
void ShowingNextendo(bool shown);

/// Starts a sign-in finished on another device. The system's sign-in window cannot run on the
/// console, so the player opens SignInUrl() on a phone or a PC through the Jadyx Portal, accepts,
/// and pastes back the address that device then fails to load (http://127.0.0.1/callback?...):
/// CompleteSignIn. The sign-in waits 15 minutes, then gives up. Ignored when already signed in or
/// signing in.
void SignIn();
/// The Nextendo sign-in page while a sign-in waits for its answer; "" otherwise.
std::string SignInUrl();
/// Hands over what the player pasted. Returns "" when the account goes on to sign in; "denied" or
/// "refused" when the answer ended the sign-in (the player said no, or the page sent another
/// error); otherwise why it was not taken, the sign-in still waiting: "none" (no sign-in waits),
/// "address" (no answer in it), "state" (an earlier attempt's answer).
std::string CompleteSignIn(std::string_view pasted);
/// The player stopped waiting on the console.
void CancelSignIn();
void SignOut();
/// Downloads the catalog again now.
void RefreshCatalog();

/// A game starts or ends: presence says what is played, the heartbeat runs, the choice freezes.
/// detail: what friends read (the game's name).
void GameStarted(std::uint64_t title_id, std::string detail);
void GameEnded();

/// The catalog's advice for a game about to start.
LaunchCheck CheckGame(std::uint64_t title_id, std::string_view installed_version);

} // namespace Jadyx::Network::Nextendo
