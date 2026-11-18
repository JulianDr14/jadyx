// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Measures public rooms without joining them: an ENet handshake (the rooms' own UDP protocol)
// and an immediate disconnect. A room ignores a connection that never sends a join request, so
// nobody in it sees the probe.

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Network {

struct RoomProbeTarget {
    std::string host; ///< Dotted IPv4 address or host name.
    u16 port{};
};

/// One handshake with each target, all at once: the round trip in milliseconds, or nullopt when
/// a room did not answer within `timeout_ms`. Needs Network::Init(). Blocks for at most
/// about `timeout_ms` plus DNS.
std::vector<std::optional<int>> ProbeRooms(const std::vector<RoomProbeTarget>& targets, u32 timeout_ms);

} // namespace Network
