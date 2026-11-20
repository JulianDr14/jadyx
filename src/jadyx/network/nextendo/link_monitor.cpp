// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/network/nextendo/link_monitor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format off
#include <iphlpapi.h>
#include <icmpapi.h>
// clang-format on

#include "jadyx/app/boot_diag.h"
#include "jadyx/network/icmp_echo.h"
#include "video_core/performance_overlay.h"

namespace Jadyx::Network::Nextendo {
namespace {

constexpr auto Interval = std::chrono::seconds{1};
constexpr DWORD TimeoutMs = 1000;
constexpr const char* ApiHost = "nextendo.network";
constexpr const char* ApiPort = "443";
/// Unanswered ICMP pings in a row, from the first, before the API's host is measured instead.
constexpr int IcmpTries = 3;

struct Monitor {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    LinkStats stats;      // under mutex
    LinkVia via = LinkVia::GameServer;
    bool running = false; // under mutex
    std::thread thread;
    VideoCore::OverlayLinesSource previous_hud = nullptr;
};

Monitor& Shared() {
    static Monitor monitor;
    return monitor;
}

std::vector<VideoCore::OverlayLine> HudLines() {
    const auto snapshot = LinkMonitorSnapshot();
    if (!snapshot) return {};
    std::vector<VideoCore::OverlayLine> lines;
    for (auto& line : LinkHudLines(*snapshot)) lines.push_back({std::move(line.text), line.warning});
    return lines;
}

/// The time a TCP connect to the API's host takes: one round trip (SYN, SYN-ACK).
std::optional<int> PingTcp(const sockaddr_storage& address, int length) {
    const SOCKET sock = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return std::nullopt;
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    const auto start = std::chrono::steady_clock::now();
    std::optional<int> result;
    if (connect(sock, reinterpret_cast<const sockaddr*>(&address), length) == 0 ||
        WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(sock, &writable);
        FD_SET(sock, &failed);
        timeval timeout{static_cast<long>(TimeoutMs / 1000), static_cast<long>(TimeoutMs % 1000) * 1000};
        if (select(0, nullptr, &writable, &failed, &timeout) > 0 && FD_ISSET(sock, &writable)) {
            result = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
        }
    }
    closesocket(sock);
    return result;
}

std::optional<sockaddr_storage> ResolveApi(int& length) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(ApiHost, ApiPort, &hints, &found) != 0 || !found) return std::nullopt;
    sockaddr_storage address{};
    std::memcpy(&address, found->ai_addr, found->ai_addrlen);
    length = static_cast<int>(found->ai_addrlen);
    freeaddrinfo(found);
    return address;
}

void Run(Monitor& monitor, std::string server_ip) {
    WSADATA wsa{};
    const bool wsa_ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    IPAddr server{};
    HANDLE icmp = INVALID_HANDLE_VALUE;
    if (!server_ip.empty() && inet_pton(AF_INET, server_ip.c_str(), &server) == 1)
        icmp = IcmpCreateFile();
    bool use_icmp = icmp != INVALID_HANDLE_VALUE;
    if (!use_icmp)
        WriteDiag(server_ip.empty() ? "nextendo: link ping to the API (no server address)"
                                    : "nextendo: link ping to the API (ICMP unavailable, error " +
                                          std::to_string(GetLastError()) + ")");
    else
        WriteDiag("nextendo: link ping to the game server over ICMP");
    int icmp_misses = 0;
    bool icmp_answered = false;
    std::optional<sockaddr_storage> api;
    int api_length = 0;
    bool api_logged = false;

    std::unique_lock lock{monitor.mutex};
    while (!monitor.stop) {
        lock.unlock();
        const auto started = std::chrono::steady_clock::now();
        std::optional<int> sample;
        LinkVia via = LinkVia::GameServer;
        if (use_icmp) {
            sample = Icmp::Echo(icmp, server, TimeoutMs);
            if (sample) icmp_answered = true;
            // A server that never answers ICMP (a firewall) is measured through the API instead.
            if (!icmp_answered && !sample && ++icmp_misses >= IcmpTries) {
                use_icmp = false;
                WriteDiag("nextendo: the game server does not answer ICMP, link ping to the API");
            }
        } else {
            via = LinkVia::Api;
            if (!api) api = ResolveApi(api_length);
            if (api) {
                sample = PingTcp(*api, api_length);
            } else if (!api_logged) {
                api_logged = true;
                WriteDiag("nextendo: link ping could not resolve the API's host");
            }
        }
        lock.lock();
        if (monitor.stop) break;
        if (via != monitor.via) {
            monitor.stats.Clear();
            monitor.via = via;
        }
        // Misses before ICMP ever answered say nothing yet (it may be filtered): the HUD keeps
        // "measuring" until the server answers or the API takes over.
        if (via != LinkVia::GameServer || icmp_answered || sample) monitor.stats.Add(sample);
        monitor.wake.wait_until(lock, started + Interval, [&] { return monitor.stop; });
    }
    lock.unlock();
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
    if (wsa_ok) WSACleanup();
}

} // namespace

void StartLinkMonitor(std::string server_ip) {
    StopLinkMonitor();
    auto& monitor = Shared();
    {
        std::scoped_lock lock{monitor.mutex};
        monitor.stop = false;
        monitor.stats.Clear();
        monitor.via = LinkVia::GameServer;
        monitor.running = true;
    }
    // Nextendo and Eden rooms exclude each other: the room's lines come back when this stops.
    monitor.previous_hud = VideoCore::overlay_extra_lines.exchange(&HudLines, std::memory_order_relaxed);
    monitor.thread = std::thread([&monitor, ip = std::move(server_ip)]() mutable { Run(monitor, std::move(ip)); });
}

void StopLinkMonitor() {
    auto& monitor = Shared();
    {
        std::scoped_lock lock{monitor.mutex};
        if (!monitor.running) return;
        monitor.stop = true;
        monitor.running = false;
    }
    monitor.wake.notify_all();
    if (monitor.thread.joinable()) monitor.thread.join();
    VideoCore::OverlayLinesSource expected = &HudLines;
    VideoCore::overlay_extra_lines.compare_exchange_strong(expected, monitor.previous_hud,
                                                           std::memory_order_relaxed);
    monitor.previous_hud = nullptr;
}

std::optional<LinkSnapshot> LinkMonitorSnapshot() {
    auto& monitor = Shared();
    std::scoped_lock lock{monitor.mutex};
    if (!monitor.running) return std::nullopt;
    return monitor.stats.Snapshot(monitor.via);
}

} // namespace Jadyx::Network::Nextendo
