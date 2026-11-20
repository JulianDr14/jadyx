// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/icmp_echo.h"

#include <array>
#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format off
#include <iphlpapi.h>
#include <icmpapi.h>
// clang-format on

namespace Jadyx::Icmp {

std::optional<std::uint32_t> ResolveIpv4(const std::string& host) {
    if (host.empty()) return std::nullopt;
    IN_ADDR direct{};
    if (inet_pton(AF_INET, host.c_str(), &direct) == 1) return direct.S_un.S_addr;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return std::nullopt;
    std::optional<std::uint32_t> address;
    if (found->ai_family == AF_INET && found->ai_addrlen >= sizeof(sockaddr_in))
        address = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr.S_un.S_addr;
    freeaddrinfo(found);
    return address;
}

std::optional<int> Echo(void* icmp, std::uint32_t address, unsigned timeout_ms) {
    std::array<char, 32> payload{'j', 'a', 'd', 'y', 'x'};
    std::array<unsigned char, sizeof(ICMP_ECHO_REPLY) + 32 + 8 + 64> reply{};
    const DWORD count = IcmpSendEcho2(static_cast<HANDLE>(icmp), nullptr, nullptr, nullptr, address,
                                      payload.data(), static_cast<WORD>(payload.size()), nullptr, reply.data(),
                                      static_cast<DWORD>(reply.size()), timeout_ms);
    if (count == 0) return std::nullopt;
    ICMP_ECHO_REPLY echo{};
    std::memcpy(&echo, reply.data(), sizeof(echo));
    if (echo.Status != IP_SUCCESS) return std::nullopt;
    return static_cast<int>(echo.RoundTripTime);
}

std::optional<int> Echo(std::uint32_t address, unsigned timeout_ms) {
    const HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto result = Echo(icmp, address, timeout_ms);
    IcmpCloseHandle(icmp);
    return result;
}

} // namespace Jadyx::Icmp
