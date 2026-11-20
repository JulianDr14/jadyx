// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Glue between sfdnsres and the Nextendo name rules. Does nothing unless the app enabled Nextendo,
// so Eden rooms and offline play keep the original resolution path.

#include <optional>
#include <string>
#include <vector>

#include "common/socket_types.h"

namespace Core {
class System;
}

namespace Service::Sockets::NextendoDns {

struct Answer {
    std::string host;                     // name actually looked up (account hosts rewritten)
    std::vector<Network::AddrInfo> addrs; // empty -> caller continues with the real DNS for host
};

/// std::nullopt when Nextendo is off: the caller keeps its original behaviour untouched.
std::optional<Answer> Resolve(Core::System& system, const std::string& host,
                              const std::optional<std::string>& service);

/// Records every IPv4 answer as "last host for this IP" so TLS can send the right SNI.
void RememberAnswers(const std::string& host, const std::vector<Network::AddrInfo>& addrs);

} // namespace Service::Sockets::NextendoDns
