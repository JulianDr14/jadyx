// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include "jadyx/storage/network_portal.h"

namespace Jadyx {

/// One time the portal could not start, or stopped on its own.
struct PortalFault {
    std::string_view what; ///< "start", "stopped" or "address_changed".
    PortalError error{};
    PortalAddress address; ///< Empty when the console had none.
    unsigned short port{};
    unsigned spare_ports{};
    int os_error{};
};

/// Keeps every portal fault in <log>/jx_portal/portal_faults.log together with the adapters the
/// console saw at that moment, so the log archive a user exports from the portal carries what is
/// needed to debug it. Never writes the pairing code or the browser session. Library thread only.
class PortalFaultLog {
public:
    /// From this many failures in a row the user is told to look at the network itself.
    static constexpr unsigned RepeatedFailures = 3;

    explicit PortalFaultLog(std::filesystem::path log_directory);
    /// Returns the failures in a row, this one included.
    unsigned Record(const PortalFault& fault);
    /// A portal that started ends the run of failures.
    void Recovered(const PortalAddress& address, unsigned short port);
    unsigned Consecutive() const { return consecutive; }

private:
    void Append(const std::string& text);
    std::filesystem::path directory;
    unsigned consecutive{};
};

std::string_view PortalErrorName(PortalError error);

} // namespace Jadyx
