// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/input/dsu_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.h>

#include "common/logging.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace Jadyx::DsuClient {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto ServersKey = L"dsu_servers";
constexpr auto PadsKey = L"dsu_pads";
constexpr auto GyroPadKey = L"dsu_gyro_pad";
/// Where the phone-only client kept its one address: brought into the list once.
constexpr auto OldPhoneKey = L"controller_phone_address";

constexpr std::size_t MaxServers = 8;
constexpr std::size_t PadsPerServer = 4;

/// How often each server is asked again (they stop after ~5 s without a request), when a pad or a
/// server that stopped sending counts as gone, how long a search listens and how often the console
/// looks on its own while it has no server.
constexpr auto RequestEvery = 1s;
constexpr auto LostAfter = 2500ms;
constexpr auto SearchListen = 1500ms;
constexpr auto SearchEvery = 4s;

[[nodiscard]] std::uint64_t AddressId(Dsu::Address address) {
    return (static_cast<std::uint64_t>(address.ipv4) << 16) | address.port;
}

struct PadSlot {
    bool present{};
    bool has_motion{};
    Dsu::PadInput input;
    std::optional<MotionSample> motion;
    Clock::time_point at{};
    std::uint32_t samples{}; ///< Since the input thread's last Pads().
};

struct Server {
    Dsu::Address address;
    DsuServerState state{DsuServerState::Connecting};
    std::uint32_t rate_hz{};
    std::array<PadSlot, PadsPerServer> pads{};
};

struct Shared {
    std::mutex mutex;
    bool loaded{};
    std::vector<Server> servers;
    std::map<std::uint64_t, DsuAssign> assign;
    std::map<std::uint64_t, std::uint8_t> resolved;
    std::uint64_t gyro_pad{};
    std::wstring console_address;
    /// An explicit search runs; its result, once over.
    bool searching{};
    std::optional<std::size_t> last_found;
    bool search_requested{};
    /// Removed servers a search does not add back, until the player asks for a search.
    std::set<std::uint64_t> ignored;
};
Shared g;
std::atomic<std::uint32_t> g_version{1};
std::atomic<bool> g_has_servers{};
std::mutex g_thread_mutex;
std::jthread g_thread;
std::atomic<std::uint8_t> g_wanted{};

void Bump() { g_version.fetch_add(1, std::memory_order_relaxed); }

winrt::Windows::Foundation::Collections::IPropertySet Values() {
    return winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
}

[[nodiscard]] std::wstring ReadString(const wchar_t* key) {
    try {
        const auto values = Values();
        if (values.HasKey(key)) return std::wstring{winrt::unbox_value<winrt::hstring>(values.Lookup(key))};
    } catch (...) {}
    return {};
}

/// Splits "a;b;c", skipping empty parts.
[[nodiscard]] std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        if (end > start) parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

/// Under g.mutex.
void SaveLocked() {
    std::wstring servers;
    for (const Server& server : g.servers) {
        if (!servers.empty()) servers += L';';
        servers += Dsu::FormatAddress(server.address); // ":port" unless the usual one.
    }
    std::wstring pads;
    for (const auto& [key, assign] : g.assign) {
        if (assign == DsuAssign::Auto) continue;
        if (!pads.empty()) pads += L';';
        pads += std::to_wstring(key) + L'=' + std::to_wstring(static_cast<unsigned>(assign));
    }
    try {
        const auto values = Values();
        values.Insert(ServersKey, winrt::box_value(winrt::hstring{servers}));
        values.Insert(PadsKey, winrt::box_value(winrt::hstring{pads}));
        values.Insert(GyroPadKey, winrt::box_value(winrt::hstring{std::to_wstring(g.gyro_pad)}));
    } catch (...) {}
}

/// Under g.mutex: the saved list, once.
void EnsureLoadedLocked() {
    if (g.loaded) return;
    g.loaded = true;
    bool had_list = false;
    try { had_list = Values().HasKey(ServersKey); } catch (...) {}
    std::vector<std::wstring> addresses = Split(ReadString(ServersKey), L';');
    if (!had_list) {
        const std::wstring old = ReadString(OldPhoneKey);
        if (!old.empty()) addresses.push_back(old);
    }
    for (const auto& text : addresses) {
        const auto address = Dsu::ParseAddress(text);
        if (!address || g.servers.size() >= MaxServers) continue;
        const bool known = std::any_of(g.servers.begin(), g.servers.end(),
                                       [&](const Server& s) { return AddressId(s.address) == AddressId(*address); });
        if (!known) g.servers.push_back({*address});
    }
    for (const auto& entry : Split(ReadString(PadsKey), L';')) {
        const std::size_t equals = entry.find(L'=');
        if (equals == std::wstring::npos) continue;
        try {
            const std::uint64_t key = std::stoull(entry.substr(0, equals));
            const unsigned value = static_cast<unsigned>(std::stoul(entry.substr(equals + 1)));
            if (value < DsuAssignCount) g.assign[key] = static_cast<DsuAssign>(value);
        } catch (...) {}
    }
    try { g.gyro_pad = std::stoull(ReadString(GyroPadKey)); } catch (...) { g.gyro_pad = 0; }
    if (!had_list && !g.servers.empty()) SaveLocked();
    g_has_servers.store(!g.servers.empty(), std::memory_order_relaxed);
}

/// Under g.mutex.
[[nodiscard]] DsuStatus BuildStatusLocked() {
    DsuStatus status;
    status.console_address = g.console_address;
    status.searching = g.searching || (g.servers.empty() && g_wanted.load(std::memory_order_relaxed) != 0);
    status.last_found = g.last_found;
    status.gyro_pad = g.gyro_pad;
    for (const Server& server : g.servers) {
        DsuServerStatus entry;
        entry.address = Dsu::FormatAddress(server.address);
        entry.state = server.state;
        entry.rate_hz = server.rate_hz;
        for (std::size_t slot = 0; slot < PadsPerServer; ++slot) {
            const PadSlot& pad = server.pads[slot];
            if (!pad.present) continue;
            const std::uint64_t key = DsuPadKey(server.address.ipv4, server.address.port, static_cast<std::uint8_t>(slot));
            const auto assign = g.assign.find(key);
            const auto resolved = g.resolved.find(key);
            entry.pads.push_back({key, static_cast<std::uint8_t>(slot),
                                  assign != g.assign.end() ? assign->second : DsuAssign::Auto,
                                  resolved != g.resolved.end() ? resolved->second : std::uint8_t{0}, pad.has_motion});
        }
        status.servers.push_back(std::move(entry));
    }
    return status;
}

/// The console's IPv4 networks: its address and prefix on each adapter.
struct LocalNetwork {
    std::uint32_t ipv4{};
    std::uint8_t prefix{};
};
std::vector<LocalNetwork> LocalNetworks() {
    std::vector<LocalNetwork> networks;
    using namespace winrt::Windows::Networking;
    try {
        for (const auto& host : Connectivity::NetworkInformation::GetHostNames()) {
            if (host.Type() != HostNameType::Ipv4) continue;
            const auto info = host.IPInformation();
            if (!info) continue;
            const auto parsed = Dsu::ParseAddress(host.CanonicalName().c_str());
            if (!parsed) continue;
            const std::uint32_t ip = parsed->ipv4;
            if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue; // Loopback, link-local.
            std::uint8_t prefix = 24;
            if (const auto length = info.PrefixLength()) prefix = length.Value();
            networks.push_back({ip, prefix});
        }
    } catch (...) {}
    return networks;
}

sockaddr_in ToSockaddr(Dsu::Address address) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(address.port);
    to.sin_addr.s_addr = htonl(address.ipv4);
    return to;
}

class Worker {
public:
    explicit Worker(std::stop_token stop_) : stop(std::move(stop_)) {
        std::random_device random;
        client_id = random();
    }

    void Run() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            LOG_ERROR(Input, "DSU: no sockets");
            return;
        }
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            LOG_ERROR(Input, "DSU: no UDP socket ({})", WSAGetLastError());
            WSACleanup();
            return;
        }
        sockaddr_in any{};
        any.sin_family = AF_INET;
        ::bind(socket_, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
        // A sweep's "port unreachable" would otherwise fail the next receive.
        BOOL report = FALSE;
        DWORD returned = 0;
        WSAIoctl(socket_, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);

        RefreshConsoleAddress();
        {
            std::scoped_lock lock{g.mutex};
            EnsureLoadedLocked();
        }
        next_auto_search = Clock::now();

        while (!stop.stop_requested()) {
            Tick();
            Receive(20ms);
            Publish();
        }
        closesocket(socket_);
        WSACleanup();
        std::scoped_lock lock{g.mutex};
        for (Server& server : g.servers) {
            server.state = DsuServerState::Connecting;
            server.rate_hz = 0;
            server.pads = {};
        }
        g.searching = false;
        g.resolved.clear();
        Bump();
    }

private:
    struct Link {
        Clock::time_point added{}, last_request{}, last_data{}, rate_since{};
        std::uint32_t rate_count{};
        bool ever_connected{};
        std::array<std::optional<std::uint32_t>, PadsPerServer> last_counter{};
        std::array<std::uint64_t, PadsPerServer> last_timestamp{};
        std::array<Clock::time_point, PadsPerServer> last_pad{};
        std::array<Dsu::PhoneMotionFilter, PadsPerServer> filter{};
    };

    void RefreshConsoleAddress() {
        networks = LocalNetworks();
        const std::wstring address = networks.empty() ? std::wstring{} : Dsu::FormatAddress({networks.front().ipv4});
        std::scoped_lock lock{g.mutex};
        g.console_address = address;
    }

    void SendTo(Dsu::Address address, const Dsu::Packet& packet) {
        const sockaddr_in to = ToSockaddr(address);
        ::sendto(socket_, reinterpret_cast<const char*>(packet.bytes.data()), static_cast<int>(packet.size), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void Tick() {
        const auto now = Clock::now();
        std::vector<Dsu::Address> addresses;
        bool start_search = false;
        bool list_empty = false;
        {
            std::scoped_lock lock{g.mutex};
            for (const Server& server : g.servers) addresses.push_back(server.address);
            list_empty = g.servers.empty();
            if (std::exchange(g.search_requested, false)) {
                start_search = true;
                explicit_search = true;
            }
        }
        // Links follow the list: new servers get one, removed ones lose theirs.
        std::map<std::uint64_t, Link> next;
        for (const auto& address : addresses) {
            const std::uint64_t id = AddressId(address);
            auto it = links.find(id);
            if (it != links.end()) next.emplace(id, std::move(it->second));
            else next.emplace(id, Link{.added = now, .rate_since = now});
        }
        links = std::move(next);

        // Searching: when asked, and on its own every few seconds while there is no server.
        if (!search_listening && (start_search || (list_empty && now >= next_auto_search))) {
            RefreshConsoleAddress();
            found.clear();
            SweepNetworks();
            search_listening = true;
            search_started = now;
        }
        if (search_listening && now - search_started >= SearchListen) FinishSearch(now);

        for (const auto& address : addresses) {
            Link& link = links.at(AddressId(address));
            if (now - link.last_request >= RequestEvery) {
                SendTo(address, Dsu::MakePortInfoRequest(client_id));
                SendTo(address, Dsu::MakePadDataRequest(client_id));
                link.last_request = now;
            }
        }

        std::scoped_lock lock{g.mutex};
        for (Server& server : g.servers) {
            const auto it = links.find(AddressId(server.address));
            if (it == links.end()) continue;
            Link& link = it->second;
            if (now - link.rate_since >= 1s) {
                server.rate_hz = link.rate_count;
                link.rate_count = 0;
                link.rate_since = now;
            }
            for (std::size_t slot = 0; slot < PadsPerServer; ++slot) {
                if (server.pads[slot].present && now - link.last_pad[slot] >= LostAfter) {
                    server.pads[slot] = {};
                    link.last_counter[slot].reset();
                    link.filter[slot].Reset();
                }
            }
            const auto since = now - (link.ever_connected ? link.last_data : link.added);
            DsuServerState state = server.state;
            if (link.ever_connected && since < LostAfter) state = DsuServerState::Connected;
            else if (since >= LostAfter) state = DsuServerState::Lost;
            else state = DsuServerState::Connecting;
            if (state != server.state) {
                if (state == DsuServerState::Lost && server.state == DsuServerState::Connected)
                    LOG_INFO(Input, "DSU: {} stopped sending", winrt::to_string(Dsu::FormatAddress(server.address)));
                server.state = state;
                if (state == DsuServerState::Lost) server.rate_hz = 0;
            }
        }
    }

    void FinishSearch(Clock::time_point now) {
        search_listening = false;
        next_auto_search = now + SearchEvery - SearchListen;
        std::size_t added = 0;
        {
            std::scoped_lock lock{g.mutex};
            for (const auto& address : found) {
                if (g.servers.size() >= MaxServers) break;
                if (g.ignored.contains(AddressId(address))) continue;
                const bool known = std::any_of(g.servers.begin(), g.servers.end(), [&](const Server& s) {
                    return AddressId(s.address) == AddressId(address);
                });
                if (known) continue;
                g.servers.push_back({address});
                ++added;
                LOG_INFO(Input, "DSU: found {}", winrt::to_string(Dsu::FormatAddress(address)));
            }
            if (explicit_search) {
                g.searching = false;
                g.last_found = added;
            }
            if (added != 0) {
                SaveLocked();
                g_has_servers.store(true, std::memory_order_relaxed);
            }
        }
        if (explicit_search || added != 0)
            LOG_INFO(Input, "DSU: search on {} network(s) added {} server(s)", networks.size(), added);
        explicit_search = false;
        found.clear();
    }

    /// Asks every address of the console's networks (at most a /24 around it each) for its
    /// version: any DSU server answers that.
    void SweepNetworks() {
        const auto version = Dsu::MakeVersionRequest(client_id);
        const auto ports = Dsu::MakePortInfoRequest(client_id);
        for (const LocalNetwork& network : networks) {
            const std::uint8_t prefix = std::max<std::uint8_t>(network.prefix, 24);
            const std::uint32_t mask = prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
            const std::uint32_t base = network.ipv4 & mask;
            const std::uint32_t count = prefix >= 31 ? 0 : (1u << (32 - prefix)) - 2;
            for (std::uint32_t i = 1; i <= count; ++i) {
                const std::uint32_t ip = base + i;
                if (ip == network.ipv4) continue;
                SendTo({ip, Dsu::DefaultPort}, version);
                SendTo({ip, Dsu::DefaultPort}, ports);
            }
        }
    }

    void Receive(std::chrono::milliseconds wait) {
        fd_set read{};
        FD_ZERO(&read);
        FD_SET(socket_, &read);
        timeval timeout{0, static_cast<long>(std::chrono::microseconds(wait).count())};
        if (::select(0, &read, nullptr, nullptr, &timeout) <= 0) return;
        for (int burst = 0; burst < 128; ++burst) {
            u_long pending = 0;
            if (burst > 0 && (ioctlsocket(socket_, FIONREAD, &pending) != 0 || pending == 0)) return;
            std::array<std::uint8_t, 512> buffer{};
            sockaddr_in from{};
            int from_size = sizeof(from);
            const int size = ::recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                                        0, reinterpret_cast<sockaddr*>(&from), &from_size);
            if (size <= 0) continue; // An ICMP error from a sweep: the next datagram may be good.
            const auto response = Dsu::ParseResponse({buffer.data(), static_cast<std::size_t>(size)});
            if (!response) continue;
            Handle(*response, {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)});
        }
    }

    void Handle(const Dsu::Response& response, Dsu::Address sender) {
        if (search_listening) {
            const bool seen = std::any_of(found.begin(), found.end(),
                                          [&](const Dsu::Address& a) { return AddressId(a) == AddressId(sender); });
            if (!seen) found.push_back(sender);
        }
        if (response.type != Dsu::MessageType::PadData) return;
        const auto it = links.find(AddressId(sender));
        if (it == links.end()) return;
        Link& link = it->second;
        const Dsu::PadSample& pad = response.pad;
        const std::size_t slot = pad.info.slot;
        if (slot >= PadsPerServer) return;
        const auto now = Clock::now();
        link.last_data = now;
        if (!link.ever_connected) {
            link.ever_connected = true;
            LOG_INFO(Input, "DSU: connected to {}", winrt::to_string(Dsu::FormatAddress(sender)));
        }
        if (!pad.info.connected) return;
        if (link.last_counter[slot] && pad.counter == *link.last_counter[slot]) return;
        link.last_counter[slot] = pad.counter;

        float dt = link.last_pad[slot] == Clock::time_point{}
                       ? 0.0f
                       : std::chrono::duration<float>(now - link.last_pad[slot]).count();
        const std::uint64_t last_timestamp = link.last_timestamp[slot];
        if (pad.timestamp_us != 0 && last_timestamp != 0 && pad.timestamp_us > last_timestamp) {
            const float pad_dt = static_cast<float>(pad.timestamp_us - last_timestamp) / 1e6f;
            if (pad_dt < 0.2f) dt = pad_dt;
        }
        link.last_timestamp[slot] = pad.timestamp_us;
        link.last_pad[slot] = now;
        ++link.rate_count;
        // A pad without a gyro sends zeros: no gravity at all.
        const bool sends_motion = std::abs(pad.accel_x) + std::abs(pad.accel_y) + std::abs(pad.accel_z) > 0.1f;
        std::optional<MotionSample> motion;
        if (sends_motion) motion = link.filter[slot].Step(Dsu::ToPadFrame(pad), dt);

        std::scoped_lock lock{g.mutex};
        const auto server = std::find_if(g.servers.begin(), g.servers.end(),
                                         [&](const Server& s) { return AddressId(s.address) == AddressId(sender); });
        if (server == g.servers.end()) return;
        PadSlot& entry = server->pads[slot];
        if (!entry.present)
            LOG_INFO(Input, "DSU: {} pad {} connected{}", winrt::to_string(Dsu::FormatAddress(sender)), slot,
                     sends_motion ? " with motion" : "");
        entry.present = true;
        entry.has_motion = entry.has_motion || sends_motion;
        entry.input = Dsu::ToSwitch(pad);
        if (motion) entry.motion = motion;
        entry.at = now;
        ++entry.samples;
    }

    /// Bumps the version when what the menus show changed.
    void Publish() {
        std::scoped_lock lock{g.mutex};
        DsuStatus status = BuildStatusLocked();
        if (status != published) {
            published = std::move(status);
            Bump();
        }
    }

    std::stop_token stop;
    SOCKET socket_{INVALID_SOCKET};
    std::uint32_t client_id{};
    std::vector<LocalNetwork> networks;
    std::map<std::uint64_t, Link> links;
    DsuStatus published;
    // Search.
    std::vector<Dsu::Address> found;
    bool search_listening{}, explicit_search{};
    Clock::time_point search_started{}, next_auto_search{};
};

} // namespace

void SetWanted(Wanter who, bool wanted) {
    const auto bit = static_cast<std::uint8_t>(who);
    const std::uint8_t before = g_wanted.load(std::memory_order_relaxed);
    if (((before & bit) != 0) == wanted) return;
    std::scoped_lock lock{g_thread_mutex};
    const std::uint8_t old = wanted ? g_wanted.fetch_or(bit) : g_wanted.fetch_and(static_cast<std::uint8_t>(~bit));
    const std::uint8_t now = wanted ? (old | bit) : (old & ~bit);
    if ((old != 0) == (now != 0)) return;
    if (now != 0) {
        g_thread = std::jthread([](std::stop_token stop) {
            Worker worker{std::move(stop)};
            worker.Run();
        });
    } else {
        g_thread = {}; // Requests stop and joins.
    }
}

bool HasServers() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::scoped_lock lock{g.mutex};
        EnsureLoadedLocked();
    });
    return g_has_servers.load(std::memory_order_relaxed);
}

void Search() {
    std::scoped_lock lock{g.mutex};
    EnsureLoadedLocked();
    g.search_requested = true;
    g.searching = true;
    g.last_found.reset();
    g.ignored.clear();
    Bump();
}

bool Add(std::wstring_view text) {
    const auto address = Dsu::ParseAddress(text);
    if (!address) return false;
    std::scoped_lock lock{g.mutex};
    EnsureLoadedLocked();
    g.ignored.erase(AddressId(*address));
    const bool known = std::any_of(g.servers.begin(), g.servers.end(),
                                   [&](const Server& s) { return AddressId(s.address) == AddressId(*address); });
    if (known) return true;
    if (g.servers.size() >= MaxServers) return false;
    g.servers.push_back({*address});
    LOG_INFO(Input, "DSU: added {}", winrt::to_string(Dsu::FormatAddress(*address)));
    SaveLocked();
    g_has_servers.store(true, std::memory_order_relaxed);
    Bump();
    return true;
}

void Remove(std::size_t index) {
    std::scoped_lock lock{g.mutex};
    EnsureLoadedLocked();
    if (index >= g.servers.size()) return;
    const Dsu::Address address = g.servers[index].address;
    LOG_INFO(Input, "DSU: removed {}", winrt::to_string(Dsu::FormatAddress(address)));
    g.ignored.insert(AddressId(address));
    for (std::uint8_t slot = 0; slot < PadsPerServer; ++slot)
        g.resolved.erase(DsuPadKey(address.ipv4, address.port, slot));
    g.servers.erase(g.servers.begin() + static_cast<std::ptrdiff_t>(index));
    SaveLocked();
    g_has_servers.store(!g.servers.empty(), std::memory_order_relaxed);
    Bump();
}

void SetAssign(std::uint64_t key, DsuAssign assign) {
    std::scoped_lock lock{g.mutex};
    EnsureLoadedLocked();
    const auto it = g.assign.find(key);
    if ((it == g.assign.end() ? DsuAssign::Auto : it->second) == assign) return;
    if (assign == DsuAssign::Auto) g.assign.erase(key);
    else g.assign[key] = assign;
    SaveLocked();
    Bump();
}

void SetGyroPad(std::uint64_t key) {
    std::scoped_lock lock{g.mutex};
    EnsureLoadedLocked();
    if (g.gyro_pad == key) return;
    g.gyro_pad = key;
    SaveLocked();
    Bump();
}

DsuStatus Status() {
    std::scoped_lock lock{g.mutex};
    EnsureLoadedLocked();
    return BuildStatusLocked();
}

std::uint32_t StatusVersion() { return g_version.load(std::memory_order_relaxed); }

std::vector<PadFrame> Pads(std::chrono::milliseconds max_age) {
    std::vector<PadFrame> frames;
    const auto now = Clock::now();
    std::scoped_lock lock{g.mutex};
    for (Server& server : g.servers) {
        for (std::size_t slot = 0; slot < PadsPerServer; ++slot) {
            PadSlot& pad = server.pads[slot];
            if (!pad.present || now - pad.at > max_age) continue;
            const std::uint64_t key = DsuPadKey(server.address.ipv4, server.address.port, static_cast<std::uint8_t>(slot));
            const auto assign = g.assign.find(key);
            frames.push_back({key, assign != g.assign.end() ? assign->second : DsuAssign::Auto, pad.input, pad.motion,
                              pad.samples != 0});
            pad.samples = 0;
        }
    }
    return frames;
}

void SetResolved(std::uint64_t key, std::uint8_t player) {
    std::scoped_lock lock{g.mutex};
    const auto it = g.resolved.find(key);
    if ((it == g.resolved.end() ? 0 : it->second) == player) return;
    if (player == 0) g.resolved.erase(key);
    else g.resolved[key] = player;
    Bump();
}

std::optional<MotionSample> GyroSample(std::chrono::milliseconds max_age) {
    const auto now = Clock::now();
    std::scoped_lock lock{g.mutex};
    struct Candidate {
        std::uint64_t key;
        DsuAssign assign;
        const PadSlot* pad;
    };
    std::vector<Candidate> candidates;
    for (const Server& server : g.servers) {
        for (std::size_t slot = 0; slot < PadsPerServer; ++slot) {
            const PadSlot& pad = server.pads[slot];
            if (!pad.present || !pad.motion || now - pad.at > max_age) continue;
            const std::uint64_t key = DsuPadKey(server.address.ipv4, server.address.port, static_cast<std::uint8_t>(slot));
            const auto assign = g.assign.find(key);
            candidates.push_back({key, assign != g.assign.end() ? assign->second : DsuAssign::Auto, &pad});
        }
    }
    const auto pick = [&](auto&& match) -> const PadSlot* {
        const auto it = std::find_if(candidates.begin(), candidates.end(), match);
        return it != candidates.end() ? it->pad : nullptr;
    };
    const PadSlot* chosen = nullptr;
    if (g.gyro_pad != 0) chosen = pick([&](const Candidate& c) { return c.key == g.gyro_pad; });
    if (!chosen) chosen = pick([](const Candidate& c) { return c.assign == DsuAssign::GyroOnly; });
    if (!chosen)
        chosen = pick([&](const Candidate& c) {
            const auto it = g.resolved.find(c.key);
            return c.assign != DsuAssign::Off && it != g.resolved.end() && it->second == 1;
        });
    if (!chosen) chosen = pick([](const Candidate& c) { return c.assign != DsuAssign::Off; });
    if (!chosen) return std::nullopt;
    return chosen->motion;
}

} // namespace Jadyx::DsuClient
