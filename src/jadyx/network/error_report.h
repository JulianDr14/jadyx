// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Error reports: when a game ends, the bugs the tracker counted during it (stubs, unimplemented
// functions, unsupported formats, failed calls...) go to the project's collector in the
// background, one line per distinct bug with its count. Nothing runs while the game plays: the
// tracker's counters are already there, and the report is read and sent after the game stopped.
// A report that cannot be sent waits in LocalState/jadyx/reports for the next one sent.
//
// A session that never reaches GameEnded (the app closed by force, suspended and terminated, or
// crashed) still reports: every load sample also rewrites a checkpoint of the running game's
// report there, and the next launch sends it, marked "interrupted" instead of "closed". The
// game's offline mode does not stop reports; it keeps the game off the internet, not the app.
//
// What is sent: the app version, the game's title id, name and version, and for each bug its
// category, source location, count and first message (IPv4 addresses removed). No account, no
// console id, no file paths of the player. With them goes the session's load: memory used against
// the app's limit and the GPU budget, and the CPU and GPU time, as averages and 10 s peaks.

#include <cstdint>
#include <string>

#include "common/memory_ceiling.h"

namespace Jadyx::ErrorReport {

/// Sends, in the background, what earlier runs left: the checkpoint of a session that did not
/// end normally and the reports whose send failed. Once per process; call it at app start.
void SendLeftovers();

/// The game whose bugs the next report holds.
void GameStarted(std::uint64_t title_id, std::string title, std::string version);

/// Takes the ended game's bugs and posts them on a background thread. Returns at once.
void GameEnded();

/// Records the running game's load. Called every 10 s from the session's memory sampler with the
/// limits it already read; reads only the process CPU time and the GPU time counter on top. Also
/// queues the checkpoint of the report, written on the report thread.
void SampleLoad(const Common::MemoryCeilings& ceilings);

} // namespace Jadyx::ErrorReport
