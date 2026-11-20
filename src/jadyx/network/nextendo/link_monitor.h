// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Measures the connection while a game plays on Nextendo: one ping a second to the game server
// (ICMP), or to the API's host (TCP connect time) when the server never answers ICMP. While it
// runs, the performance HUD shows the result the way it shows an Eden room's.

#include <optional>
#include <string>

#include "jadyx/network/nextendo/link_stats.h"

namespace Jadyx::Network::Nextendo {

/// Starts (or restarts) measuring towards `server_ip`. Empty: only the API's host is measured.
void StartLinkMonitor(std::string server_ip);
/// Stops measuring and takes the HUD lines away. Waits for the thread (at most one ping).
void StopLinkMonitor();
/// The window so far, nullopt while nothing runs.
std::optional<LinkSnapshot> LinkMonitorSnapshot();

} // namespace Jadyx::Network::Nextendo
