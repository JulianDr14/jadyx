// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/portal_fault_log.h"

#include <fstream>
#include <system_error>
#include <windows.h>
#include <fmt/format.h>
#include <winrt/Windows.ApplicationModel.h>
#include "common/logging.h"
#include "common/scm_rev.h"

namespace Jadyx {
namespace {
constexpr std::uintmax_t MaxLogBytes = 512 * 1024;

std::string Now() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}Z", time.wYear, time.wMonth, time.wDay,
                       time.wHour, time.wMinute, time.wSecond);
}

std::string AppVersion() {
    std::string package = "?";
    try {
        const auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        package = fmt::format("{}.{}.{}.{}", v.Major, v.Minor, v.Build, v.Revision);
    } catch (...) {} // The native harness has no package identity.
    return fmt::format("{} ({})", package, Common::g_scm_desc);
}

std::string Describe(const PortalAddress& address) {
    if (address.ipv4.empty()) return "none";
    return fmt::format("{} {} gateway={} link_local={}", address.name, address.ipv4, address.gateway,
                       address.link_local);
}
} // namespace

std::string_view PortalErrorName(PortalError error) {
    switch (error) {
    case PortalError::None: return "none";
    case PortalError::NoAddress: return "no_address";
    case PortalError::AddressLost: return "address_lost";
    case PortalError::PortInUse: return "port_in_use";
    case PortalError::PortBlocked: return "port_blocked";
    case PortalError::Bind: return "bind";
    case PortalError::Random: return "random";
    case PortalError::Listen: return "listen";
    case PortalError::Storage: return "storage";
    }
    return "unknown";
}

PortalFaultLog::PortalFaultLog(std::filesystem::path log_directory)
    : directory{std::move(log_directory) / "jx_portal"} {}

unsigned PortalFaultLog::Record(const PortalFault& fault) {
    ++consecutive;
    LOG_ERROR(Frontend, "Network portal fault: {} {} on {} port {} (WSA {}), {} in a row", fault.what,
              PortalErrorName(fault.error), Describe(fault.address), fault.port, fault.os_error, consecutive);
    Append(fmt::format("=== {} fault {} in a row ===\n"
                       "what: {}\nerror: {} (WSA {})\naddress: {}\nport: {} (+{} spare)\napp: {}\n{}\n",
                       Now(), consecutive, fault.what, PortalErrorName(fault.error), fault.os_error,
                       Describe(fault.address), fault.port, fault.spare_ports, AppVersion(),
                       PortalAdapterReport()));
    return consecutive;
}

void PortalFaultLog::Recovered(const PortalAddress& address, unsigned short port) {
    if (!consecutive) return;
    LOG_INFO(Frontend, "Network portal: listening on port {} after {} faults", port, consecutive);
    Append(fmt::format("=== {} recovered after {} faults: {} port {} ===\n\n", Now(), consecutive,
                       Describe(address), port));
    consecutive = 0;
}

void PortalFaultLog::Append(const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto path = directory / "portal_faults.log";
    // One previous file is kept, so a long run of faults never grows without bound.
    if (std::filesystem::file_size(path, ec) > MaxLogBytes && !ec)
        std::filesystem::rename(path, directory / "portal_faults.1.log", ec);
    std::ofstream file{path, std::ios::binary | std::ios::app};
    file << text;
    if (!file) LOG_WARNING(Frontend, "Network portal: the fault report could not be written");
}

} // namespace Jadyx
