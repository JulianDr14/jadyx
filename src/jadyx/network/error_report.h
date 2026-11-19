// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Error reports: when a game ends, the bugs the tracker counted during it (stubs, unimplemented
// functions, unsupported formats, failed calls...) go to the project's collector in the
// background, one line per distinct bug with its count. Nothing runs while the game plays: the
// tracker's counters are already there, and the report is read and sent after the game stopped.
// A report that cannot be sent waits in LocalState/jadyx/reports for the next game's.
//
// What is sent: the app version, the game's title id, name and version, and for each bug its
// category, source location, count and first message (IPv4 addresses removed). No account, no
// console id, no file paths of the player.

#include <cstdint>
#include <string>

namespace Jadyx::ErrorReport {

/// The game whose bugs the next report holds.
void GameStarted(std::uint64_t title_id, std::string title, std::string version);

/// Takes the ended game's bugs and posts them on a background thread. Returns at once.
void GameEnded();

} // namespace Jadyx::ErrorReport
