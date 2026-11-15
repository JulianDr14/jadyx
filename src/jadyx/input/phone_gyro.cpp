// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/input/phone_gyro.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.h>

#include "common/logging.h"
#include "jadyx/input/dsu_protocol.h"

namespace Jadyx::PhoneGyro {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto AddressKey = L"controller_phone_address";
constexpr auto TypedKey = L"controller_phone_typed";

/// How often the phone is asked again (servers stop after ~5 s without a request), when it
/// counts as lost, and when a found (not typed) phone that stays lost is looked for again: its
/// address may have changed.
constexpr auto RequestEvery = 1s;
constexpr auto LostAfter = 2500ms;
constexpr auto SearchAgainAfter = 8s;
/// How long a search listens for answers, and the wait before the next one while none came.
constexpr auto SearchListen = 1500ms;
constexpr auto SearchEvery = 4s;

struct Shared {
    std::mutex mutex;
    PhoneGyroStatus status;
    std::optional<MotionSample> latest;
    Clock::time_point latest_at{};
    // Commands for the worker.
    bool search_requested{};
    std::optional<Dsu::Address> typed_request;
};
Shared g;
std::atomic<std::uint32_t> g_version{1};
std::mutex g_thread_mutex;
std::jthread g_thread;
std::atomic<bool> g_wanted{};

void Bump() { g_version.fetch_add(1, std::memory_order_relaxed); }

void Publish(PhoneGyroState state, std::optional<Dsu::Address> address, bool typed, std::uint32_t rate,
             const std::wstring& console) {
    std::scoped_lock lock{g.mutex};
    PhoneGyroStatus next{state, address ? Dsu::FormatAddress(*address) : std::wstring{}, typed, rate, console};
    auto& now = g.status;
    if (now.state != next.state || now.address != next.address || now.typed != next.typed ||
        now.rate_hz != next.rate_hz || now.console_address != next.console_address) {
        now = std::move(next);
        Bump();
    }
}

struct Saved {
    std::optional<Dsu::Address> address;
    bool typed{};
};
Saved LoadSaved() {
    Saved saved;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(AddressKey))
            saved.address = Dsu::ParseAddress(winrt::unbox_value<winrt::hstring>(values.Lookup(AddressKey)).c_str());
        if (values.HasKey(TypedKey)) saved.typed = winrt::unbox_value<bool>(values.Lookup(TypedKey));
    } catch (...) {}
    if (!saved.address) saved.typed = false;
    return saved;
}
void Save(const std::optional<Dsu::Address>& address, bool typed) {
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (address) values.Insert(AddressKey, winrt::box_value(winrt::hstring{Dsu::FormatAddress(*address)}));
        else values.Remove(AddressKey);
        values.Insert(TypedKey, winrt::box_value(typed && address.has_value()));
    } catch (...) {}
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
            LOG_ERROR(Input, "Phone gyro: no sockets");
            return;
        }
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            LOG_ERROR(Input, "Phone gyro: no UDP socket ({})", WSAGetLastError());
            WSACleanup();
            return;
        }
        sockaddr_in any{};
        any.sin_family = AF_INET;
        ::bind(socket_, reinterpret_cast<const sockaddr*>(&any), sizeof(any));

        const Saved saved = LoadSaved();
        target = saved.address;
        typed = saved.typed;
        RefreshConsoleAddress();
        if (target) StartConnecting();
        else StartSearch();

        while (!stop.stop_requested()) {
            TakeCommands();
            Tick();
            Receive(20ms);
        }
        closesocket(socket_);
        WSACleanup();
        Publish(PhoneGyroState::Off, std::nullopt, false, 0, {});
        std::scoped_lock lock{g.mutex};
        g.latest.reset();
    }

private:
    void RefreshConsoleAddress() {
        networks = LocalNetworks();
        console_address = networks.empty() ? std::wstring{} : Dsu::FormatAddress({networks.front().ipv4});
    }

    void TakeCommands() {
        bool search = false;
        std::optional<Dsu::Address> typed_request;
        {
            std::scoped_lock lock{g.mutex};
            search = std::exchange(g.search_requested, false);
            typed_request = std::exchange(g.typed_request, std::nullopt);
        }
        if (typed_request) {
            target = typed_request;
            typed = true;
            Save(target, true);
            LOG_INFO(Input, "Phone gyro: typed address {}", winrt::to_string(Dsu::FormatAddress(*target)));
            StartConnecting();
        } else if (search) {
            typed = false;
            StartSearch();
        }
    }

    void StartSearch() {
        state = PhoneGyroState::Searching;
        RefreshConsoleAddress();
        found.clear();
        search_sent = false;
        search_started = Clock::now();
        filter.Reset();
        Publish(state, std::nullopt, false, 0, console_address);
    }

    void StartConnecting() {
        state = PhoneGyroState::Connecting;
        slot.reset();
        last_counter.reset();
        filter.Reset();
        last_pad = {};
        connect_started = Clock::now();
        last_request = {};
        rate_count = 0;
        rate_hz = 0;
        rate_since = Clock::now();
        Publish(state, target, typed, 0, console_address);
    }

    void SendTo(Dsu::Address address, const Dsu::Packet& packet) {
        const sockaddr_in to = ToSockaddr(address);
        ::sendto(socket_, reinterpret_cast<const char*>(packet.bytes.data()), static_cast<int>(packet.size), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void Tick() {
        const auto now = Clock::now();
        if (state == PhoneGyroState::Searching || state == PhoneGyroState::NotFound) {
            if (!search_sent && now >= search_started) {
                SweepNetworks();
                search_sent = true;
            }
            if (search_sent && now - search_started >= SearchListen) {
                if (!found.empty()) {
                    // A server with a connected pad first; else the first that answered.
                    const auto pick = std::find_if(found.begin(), found.end(), [](const Found& f) { return f.has_pad; });
                    target = (pick != found.end() ? *pick : found.front()).address;
                    typed = false;
                    Save(target, false);
                    LOG_INFO(Input, "Phone gyro: found {} ({} answered)", winrt::to_string(Dsu::FormatAddress(*target)),
                             found.size());
                    StartConnecting();
                    return;
                }
                if (state == PhoneGyroState::Searching) {
                    state = PhoneGyroState::NotFound;
                    LOG_INFO(Input, "Phone gyro: no phone answered on {} network(s)", networks.size());
                    Publish(state, std::nullopt, false, 0, console_address);
                }
                // Look again a little later, as the player opens the app.
                search_started = now + SearchEvery - SearchListen;
                search_sent = false;
                found.clear();
                RefreshConsoleAddress();
            }
            return;
        }
        if (!target) return;
        if (now - last_request >= RequestEvery) {
            SendTo(*target, Dsu::MakePortInfoRequest(client_id));
            SendTo(*target, Dsu::MakePadDataRequest(client_id));
            last_request = now;
        }
        if (now - rate_since >= 1s) {
            rate_hz = rate_count;
            rate_count = 0;
            rate_since = now;
            if (state == PhoneGyroState::Connected) Publish(state, target, typed, rate_hz, console_address);
        }
        const auto since = now - (last_pad == Clock::time_point{} ? connect_started : last_pad);
        if (state == PhoneGyroState::Connected && since >= LostAfter) {
            state = PhoneGyroState::Lost;
            LOG_INFO(Input, "Phone gyro: {} stopped sending", winrt::to_string(Dsu::FormatAddress(*target)));
            Publish(state, target, typed, 0, console_address);
        }
        if ((state == PhoneGyroState::Connecting && since >= LostAfter)) {
            state = PhoneGyroState::Lost;
            Publish(state, target, typed, 0, console_address);
        }
        // A found phone may have a new address now: look for it again. A typed one is kept.
        if (state == PhoneGyroState::Lost && !typed && since >= SearchAgainAfter) StartSearch();
    }

    /// Asks every address of the console's networks (at most a /24 around it each) for its
    /// version: any DSU server answers that, a phone app included.
    void SweepNetworks() {
        const auto version = Dsu::MakeVersionRequest(client_id);
        const auto ports = Dsu::MakePortInfoRequest(client_id);
        std::uint16_t port = Dsu::DefaultPort;
        if (const Saved saved = LoadSaved(); saved.address) port = saved.address->port;
        for (const LocalNetwork& network : networks) {
            const std::uint8_t prefix = std::max<std::uint8_t>(network.prefix, 24);
            const std::uint32_t mask = prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
            const std::uint32_t base = network.ipv4 & mask;
            const std::uint32_t count = prefix >= 31 ? 0 : (1u << (32 - prefix)) - 2;
            for (std::uint32_t i = 1; i <= count; ++i) {
                const std::uint32_t ip = base + i;
                if (ip == network.ipv4) continue;
                SendTo({ip, port}, version);
                SendTo({ip, port}, ports);
            }
        }
    }

    void Receive(std::chrono::milliseconds wait) {
        fd_set read{};
        FD_ZERO(&read);
        FD_SET(socket_, &read);
        timeval timeout{0, static_cast<long>(std::chrono::microseconds(wait).count())};
        if (::select(0, &read, nullptr, nullptr, &timeout) <= 0) return;
        for (int burst = 0; burst < 64; ++burst) {
            std::array<std::uint8_t, 512> buffer{};
            sockaddr_in from{};
            int from_size = sizeof(from);
            u_long pending = 0;
            if (burst > 0 && (ioctlsocket(socket_, FIONREAD, &pending) != 0 || pending == 0)) return;
            const int size = ::recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                                        0, reinterpret_cast<sockaddr*>(&from), &from_size);
            if (size <= 0) return; // ICMP "port unreachable" from a sweep also lands here.
            const auto response = Dsu::ParseResponse({buffer.data(), static_cast<std::size_t>(size)});
            if (!response) continue;
            const Dsu::Address sender{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
            Handle(*response, sender);
        }
    }

    void Handle(const Dsu::Response& response, Dsu::Address sender) {
        if (state == PhoneGyroState::Searching || state == PhoneGyroState::NotFound) {
            auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) {
                return f.address.ipv4 == sender.ipv4 && f.address.port == sender.port;
            });
            if (it == found.end()) {
                found.push_back({sender, false});
                it = found.end() - 1;
            }
            if (response.type == Dsu::MessageType::PortInfo && response.slot.connected) it->has_pad = true;
            return;
        }
        if (!target || sender.ipv4 != target->ipv4 || response.type != Dsu::MessageType::PadData) return;
        const Dsu::PadSample& pad = response.pad;
        if (!pad.info.connected) return;
        // The first slot that sends motion is the phone's; others (a second pad) are ignored.
        if (!slot) slot = pad.info.slot;
        if (pad.info.slot != *slot) return;
        if (last_counter && pad.counter == *last_counter) return;
        last_counter = pad.counter;

        const auto now = Clock::now();
        float dt = last_pad == Clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - last_pad).count();
        if (pad.timestamp_us != 0 && last_timestamp != 0 && pad.timestamp_us > last_timestamp) {
            const float phone_dt = static_cast<float>(pad.timestamp_us - last_timestamp) / 1e6f;
            if (phone_dt < 0.2f) dt = phone_dt;
        }
        last_timestamp = pad.timestamp_us;
        last_pad = now;
        ++rate_count;
        const MotionSample sample = filter.Step(Dsu::ToPadFrame(pad), dt);
        {
            std::scoped_lock lock{g.mutex};
            g.latest = sample;
            g.latest_at = now;
        }
        if (state != PhoneGyroState::Connected) {
            state = PhoneGyroState::Connected;
            LOG_INFO(Input, "Phone gyro: connected to {} slot {}", winrt::to_string(Dsu::FormatAddress(*target)),
                     pad.info.slot);
            Publish(state, target, typed, rate_hz, console_address);
        }
    }

    struct Found {
        Dsu::Address address;
        bool has_pad{};
    };

    std::stop_token stop;
    SOCKET socket_{INVALID_SOCKET};
    std::uint32_t client_id{};
    PhoneGyroState state{PhoneGyroState::Off};
    std::optional<Dsu::Address> target;
    bool typed{};
    std::vector<LocalNetwork> networks;
    std::wstring console_address;
    // Search.
    std::vector<Found> found;
    bool search_sent{};
    Clock::time_point search_started{};
    // Connection.
    std::optional<std::uint8_t> slot;
    std::optional<std::uint32_t> last_counter;
    std::uint64_t last_timestamp{};
    Clock::time_point connect_started{}, last_request{}, last_pad{}, rate_since{};
    std::uint32_t rate_count{}, rate_hz{};
    Dsu::PhoneMotionFilter filter;
};

} // namespace

void SetWanted(bool wanted) {
    if (g_wanted.load(std::memory_order_relaxed) == wanted) return;
    std::scoped_lock lock{g_thread_mutex};
    if (g_wanted.exchange(wanted) == wanted) return;
    if (wanted) {
        g_thread = std::jthread([](std::stop_token stop) {
            Worker worker{std::move(stop)};
            worker.Run();
        });
    } else {
        g_thread = {}; // Requests stop and joins.
    }
}

void Search() {
    std::scoped_lock lock{g.mutex};
    g.search_requested = true;
    g.typed_request.reset();
}

bool ConnectTo(std::wstring_view text) {
    const auto address = Dsu::ParseAddress(text);
    if (!address) return false;
    std::scoped_lock lock{g.mutex};
    g.typed_request = address;
    g.search_requested = false;
    return true;
}

PhoneGyroStatus Status() {
    std::scoped_lock lock{g.mutex};
    return g.status;
}

std::uint32_t StatusVersion() { return g_version.load(std::memory_order_relaxed); }

std::optional<MotionSample> Latest(std::chrono::milliseconds max_age) {
    std::scoped_lock lock{g.mutex};
    if (!g.latest || Clock::now() - g.latest_at > max_age) return std::nullopt;
    return g.latest;
}

} // namespace Jadyx::PhoneGyro
