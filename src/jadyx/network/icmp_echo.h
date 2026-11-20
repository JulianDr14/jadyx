// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ICMP echo (ping) over the IP Helper API, shared by the Nextendo link monitor and the room list.

#include <cstdint>
#include <optional>
#include <string>

namespace Jadyx::Icmp {

/// An IPv4 address (network byte order) for a dotted address or a host name; nullopt when it
/// does not resolve to IPv4. A host name blocks on DNS.
std::optional<std::uint32_t> ResolveIpv4(const std::string& host);

/// One echo through `icmp` (an IcmpCreateFile handle) to `address` (network byte order): the
/// round trip in milliseconds, or nullopt when nothing answered within `timeout_ms`.
std::optional<int> Echo(void* icmp, std::uint32_t address, unsigned timeout_ms);

/// Echo with a handle of its own, for one-off measurements from any thread.
std::optional<int> Echo(std::uint32_t address, unsigned timeout_ms);

} // namespace Jadyx::Icmp
