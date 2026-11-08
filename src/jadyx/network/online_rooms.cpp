// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/network/online_rooms.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

#include "common/logging.h"
#include "common/settings.h"
#include "jadyx/network/http.h"
#include "network/network.h"
#include "video_core/performance_overlay.h"

namespace Jadyx::Online {
namespace {

using Network::RoomMember;

constexpr auto NicknameKey = L"online_nickname";
constexpr auto AddressKey = L"online_address";
constexpr auto PortKey = L"online_port";

// The room callbacks run on the RoomMember's loop thread and take this lock. Never hold it while
// calling into RoomMember: Leave() joins that thread.
std::mutex g_mutex;
Status g_status;

std::once_flag g_init_once;
bool g_network_ready{};
RoomMember::CallbackHandle<RoomMember::State> g_on_state;
RoomMember::CallbackHandle<RoomMember::Error> g_on_error;
RoomMember::CallbackHandle<Network::RoomInformation> g_on_room;

std::mutex g_worker_mutex; // Guards g_worker itself; g_busy keeps a second request out.
std::jthread g_worker;
std::atomic<bool> g_busy{};

std::mutex g_lobby_mutex;
LobbySnapshot g_lobby;
std::jthread g_lobby_worker;
std::atomic<bool> g_lobby_busy{};

void Touch() { ++g_status.version; }

Failure Translate(RoomMember::Error error) {
    switch (error) {
    case RoomMember::Error::LostConnection: return Failure::LostConnection;
    case RoomMember::Error::HostKicked: return Failure::Kicked;
    case RoomMember::Error::NameCollision: return Failure::NameCollision;
    case RoomMember::Error::IpCollision: return Failure::IpCollision;
    case RoomMember::Error::WrongVersion: return Failure::WrongVersion;
    case RoomMember::Error::WrongPassword: return Failure::WrongPassword;
    case RoomMember::Error::CouldNotConnect: return Failure::CouldNotConnect;
    case RoomMember::Error::RoomIsFull: return Failure::RoomFull;
    case RoomMember::Error::HostBanned: return Failure::Banned;
    default: return Failure::Unknown;
    }
}

/// The performance HUD's online lines, while in a room or just dropped from one. The HUD font
/// has capitals, digits and a few signs only, and the HUD's own lines are Spanish.
std::vector<VideoCore::OverlayLine> HudLines() {
    Phase phase;
    Failure failure;
    std::size_t members;
    std::uint32_t slots;
    {
        std::scoped_lock lock{g_mutex};
        phase = g_status.phase;
        failure = g_status.failure;
        members = g_status.members.size();
        slots = g_status.slots;
    }
    if (phase == Phase::Joining) return {{"ONLINE CONECTANDO"}};
    if (phase != Phase::Joined) {
        // Dropped mid-game: the game is alone now, and its players should know.
        if (failure == Failure::LostConnection || failure == Failure::Kicked)
            return {{"ONLINE SIN CONEXION", true}};
        return {};
    }
    const auto member = Network::GetRoomMember().lock();
    if (!member) return {};
    const auto stats = member->GetConnectionStats();
    // Rates over the time since the last refresh (the HUD asks every half second).
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last_time;
    static RoomMember::ConnectionStats last;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    const bool rates = last_time != Clock::time_point{} && seconds > 0.1 && seconds < 5 &&
                       stats.bytes_sent >= last.bytes_sent && stats.bytes_received >= last.bytes_received;
    const auto rate = [&](std::uint64_t value, std::uint64_t before) {
        return rates ? static_cast<double>(value - before) / seconds : 0.0;
    };
    const double up_kb = rate(stats.bytes_sent, last.bytes_sent) / 1024;
    const double down_kb = rate(stats.bytes_received, last.bytes_received) / 1024;
    const double sent = stats.game_packets_sent >= last.game_packets_sent
                            ? rate(stats.game_packets_sent, last.game_packets_sent) : 0.0;
    const double received = stats.game_packets_received >= last.game_packets_received
                                ? rate(stats.game_packets_received, last.game_packets_received) : 0.0;
    last_time = now;
    last = stats;
    const double loss = stats.packet_loss * 100.0 / 65536.0;
    // Past these, games that sync every frame start to stutter or drop players.
    const bool slow = stats.round_trip_ms > 150 || stats.round_trip_variance_ms > 50;
    std::vector<VideoCore::OverlayLine> lines;
    lines.push_back({slots ? fmt::format("ONLINE SALA {}/{}", members, slots) : fmt::format("ONLINE SALA {}", members)});
    lines.push_back({fmt::format("PING {} MS VAR {} MS", stats.round_trip_ms, stats.round_trip_variance_ms), slow});
    lines.push_back({fmt::format("PERDIDA {:.1f}%", loss), loss >= 2.0});
    lines.push_back({fmt::format("RED SUBE {:.1f} BAJA {:.1f} KB/S", up_kb, down_kb)});
    lines.push_back({fmt::format("PAQ ENV {:.0f}/S REC {:.0f}/S", sent, received)});
    return lines;
}

std::shared_ptr<RoomMember> BoundMember() {
    std::call_once(g_init_once, [] {
        g_network_ready = Network::Init();
        if (!g_network_ready) {
            LOG_ERROR(Network, "Online: the network library did not start");
            return;
        }
        const auto member = Network::GetRoomMember().lock();
        if (!member) return;
        VideoCore::overlay_extra_lines.store(&HudLines, std::memory_order_relaxed);
        g_on_state = member->BindOnStateChanged([](const RoomMember::State& state) {
            std::scoped_lock lock{g_mutex};
            switch (state) {
            case RoomMember::State::Joining: g_status.phase = Phase::Joining; break;
            case RoomMember::State::Joined:
            case RoomMember::State::Moderator:
                g_status.phase = Phase::Joined;
                g_status.failure = Failure::None;
                break;
            default:
                // Leave() reports Idle before it finishes; the worker ends the Leaving phase.
                if (g_status.phase != Phase::Leaving) {
                    g_status.phase = Phase::Idle;
                    g_status.members.clear();
                }
                break;
            }
            LOG_INFO(Network, "Online: room state {}", Network::GetStateStr(state));
            Touch();
        });
        g_on_error = member->BindOnError([](const RoomMember::Error& error) {
            std::scoped_lock lock{g_mutex};
            g_status.failure = Translate(error);
            LOG_WARNING(Network, "Online: room error {}", Network::GetErrorStr(error));
            Touch();
        });
        g_on_room = member->BindOnRoomInformationChanged(
            [](const Network::RoomInformation& info) {
                // The member list is updated right before this callback, on this same thread.
                std::vector<Member> members;
                if (const auto self = Network::GetRoomMember().lock())
                    for (const auto& entry : self->GetMemberInformation())
                        members.push_back({entry.nickname, entry.game_info.name});
                std::scoped_lock lock{g_mutex};
                g_status.room_name = info.name;
                g_status.slots = info.member_slots;
                g_status.members = std::move(members);
                Touch();
            });
    });
    return g_network_ready ? Network::GetRoomMember().lock() : nullptr;
}

/// The address as a dotted IPv4 (ENet speaks IPv4 only), or empty when it does not resolve.
std::string Resolve(const std::string& address) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(address.c_str(), nullptr, &hints, &result) != 0 || !result) return {};
    std::string text;
    char buffer[INET_ADDRSTRLEN]{};
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) text = buffer;
    freeaddrinfo(result);
    return text;
}

void SetFailed(Failure failure) {
    std::scoped_lock lock{g_mutex};
    g_status.phase = Phase::Idle;
    g_status.failure = failure;
    g_status.members.clear();
    Touch();
}

void LeaveNow(RoomMember& member) {
    {
        std::scoped_lock lock{g_mutex};
        if (g_status.phase == Phase::Idle && !member.IsConnected()) return;
        g_status.phase = Phase::Leaving;
        Touch();
    }
    member.Leave();
    std::scoped_lock lock{g_mutex};
    g_status.phase = Phase::Idle;
    g_status.room_name.clear();
    g_status.members.clear();
    g_status.slots = 0;
    Touch();
    LOG_INFO(Network, "Online: left the room");
}

template <typename Job>
void RunOnWorker(Job job) {
    if (g_busy.exchange(true)) return;
    std::scoped_lock lock{g_worker_mutex};
    if (g_worker.joinable()) g_worker.join(); // Finished: g_busy was false.
    g_worker = std::jthread([job = std::move(job)] {
        job();
        g_busy = false;
    });
}

bool IsNicknameChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
           c == L' ' || c == L'.' || c == L'_' || c == L'-';
}

/// Latin letters with a mark lose it; anything else unsupported is dropped.
wchar_t Unmark(wchar_t c) {
    constexpr std::wstring_view from = L"áàâäãåÁÀÂÄÃÅéèêëÉÈÊËíìîïÍÌÎÏóòôöõÓÒÔÖÕúùûüÚÙÛÜñÑçÇýÿÝ";
    constexpr std::wstring_view to = L"aaaaaaAAAAAAeeeeEEEEiiiiIIIIoooooOOOOOuuuuUUUUnNcCyyY";
    static_assert(from.size() == to.size());
    const auto at = from.find(c);
    return at == std::wstring_view::npos ? c : to[at];
}

std::string LobbyUrl() {
    std::string base = Settings::values.web_api_url.GetValue();
    if (base.empty()) base = "api.ynet-fun.xyz";
    if (!base.starts_with("http://") && !base.starts_with("https://")) base = "https://" + base;
    while (base.ends_with('/')) base.pop_back();
    return base + "/lobby";
}

template <typename T>
T Field(const nlohmann::json& object, const char* key, T fallback) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

void ParseLobby(const std::string& body, LobbySnapshot& out) {
    const auto json = nlohmann::json::parse(body);
    const auto rooms = json.find("rooms");
    if (rooms == json.end() || !rooms->is_array()) throw std::runtime_error("no rooms");
    for (const auto& entry : *rooms) {
        if (!entry.is_object()) continue;
        if (Field<std::uint32_t>(entry, "netVersion", 0) != Network::network_version) {
            ++out.other_versions;
            continue;
        }
        LobbyRoom room;
        room.name = Field<std::string>(entry, "name", "");
        room.description = Field<std::string>(entry, "description", "");
        room.owner = Field<std::string>(entry, "owner", "");
        room.game = Field<std::string>(entry, "preferredGameName", "");
        room.address = Field<std::string>(entry, "address", "");
        room.port = Field<std::uint16_t>(entry, "port", DefaultPort);
        room.max_players = Field<std::uint32_t>(entry, "maxPlayers", 0);
        room.password = Field<bool>(entry, "hasPassword", false);
        if (const auto players = entry.find("players"); players != entry.end() && players->is_array())
            room.players = static_cast<std::uint32_t>(players->size());
        if (room.address.empty() || room.port == 0) continue;
        out.rooms.push_back(std::move(room));
    }
    // Rooms with people first: that is where a game can start.
    std::ranges::stable_sort(out.rooms, [](const LobbyRoom& a, const LobbyRoom& b) {
        if (a.players != b.players) return a.players > b.players;
        return a.name < b.name;
    });
}

} // namespace

Status Snapshot() {
    std::scoped_lock lock{g_mutex};
    return g_status;
}

bool InRoom() {
    std::scoped_lock lock{g_mutex};
    return g_status.phase == Phase::Joined;
}

void Join(const std::string& nickname, const std::string& address, std::uint16_t port,
          const std::string& password) {
    RunOnWorker([nickname, address, port, password] {
        {
            std::scoped_lock lock{g_mutex};
            g_status.phase = Phase::Joining;
            g_status.failure = Failure::None;
            g_status.address = address;
            g_status.port = port;
            g_status.room_name.clear();
            g_status.members.clear();
            Touch();
        }
        if (nickname.size() < MinNicknameLength || nickname.size() > MaxNicknameLength) {
            SetFailed(Failure::BadNickname);
            return;
        }
        const auto member = BoundMember();
        if (!member) {
            SetFailed(Failure::Unknown);
            return;
        }
        LeaveNow(*member);
        const std::string ip = Resolve(address);
        if (ip.empty()) {
            LOG_WARNING(Network, "Online: '{}' does not resolve to an IPv4 address", address);
            SetFailed(Failure::BadAddress);
            return;
        }
        {
            std::scoped_lock lock{g_mutex};
            g_status.phase = Phase::Joining;
            Touch();
        }
        LOG_INFO(Network, "Online: joining {}:{} ({}) as '{}'", address, port, ip, nickname);
        // Blocks up to five seconds for the connection; the room's answer arrives on its thread.
        member->Join(nickname, ip.c_str(), port, 0, Network::NoPreferredIP, password);
    });
}

void Leave() {
    RunOnWorker([] {
        if (const auto member = BoundMember()) LeaveNow(*member);
    });
}

void Shutdown() {
    {
        std::scoped_lock lock{g_worker_mutex};
        if (g_worker.joinable()) g_worker.join();
    }
    if (g_lobby_worker.joinable()) {
        g_lobby_worker.request_stop();
        g_lobby_worker.join();
    }
    if (g_network_ready)
        if (const auto member = Network::GetRoomMember().lock()) LeaveNow(*member);
}

std::string MakeNickname(std::wstring_view name) {
    std::string out;
    for (const wchar_t raw : name) {
        const wchar_t c = Unmark(raw);
        if (!IsNicknameChar(c)) continue;
        if (c == L' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(static_cast<char>(c));
        if (out.size() == MaxNicknameLength) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return "Jadyx";
    for (char digit = '1'; out.size() < MinNicknameLength; ++digit) out.push_back(digit);
    return out;
}

std::optional<std::uint16_t> ParsePort(std::wstring_view text) {
    if (text.empty() || text.size() > 5) return std::nullopt;
    std::uint32_t value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(c - L'0');
    }
    if (value == 0 || value > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

void RefreshLobby() {
    if (g_lobby_busy.exchange(true)) return;
    if (g_lobby_worker.joinable()) g_lobby_worker.join();
    {
        std::scoped_lock lock{g_lobby_mutex};
        g_lobby.state = LobbyState::Loading;
        ++g_lobby.version;
    }
    g_lobby_worker = std::jthread([](std::stop_token stop) {
        LobbySnapshot fresh;
        fresh.state = LobbyState::Failed;
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
            const std::string url = LobbyUrl();
            try {
                if (const auto body = Http::Get(url, stop, 4u << 20)) {
                    ParseLobby(*body, fresh);
                    fresh.state = LobbyState::Ready;
                    LOG_INFO(Network, "Online: lobby has {} joinable rooms ({} for other versions)",
                             fresh.rooms.size(), fresh.other_versions);
                } else {
                    LOG_WARNING(Network, "Online: no answer from {}", url);
                }
            } catch (const std::exception& e) {
                fresh.rooms.clear();
                LOG_WARNING(Network, "Online: unreadable lobby from {}: {}", url, e.what());
            } catch (const winrt::hresult_error&) {
                fresh.rooms.clear();
                LOG_WARNING(Network, "Online: lobby request to {} failed", url);
            }
        }
        {
            std::scoped_lock lock{g_lobby_mutex};
            fresh.version = g_lobby.version + 1;
            // A failed refresh keeps the rooms already shown.
            if (fresh.state == LobbyState::Failed) fresh.rooms = std::move(g_lobby.rooms);
            g_lobby = std::move(fresh);
        }
        g_lobby_busy = false;
    });
}

LobbySnapshot Lobby() {
    std::scoped_lock lock{g_lobby_mutex};
    return g_lobby;
}

SavedFields LoadSavedFields() {
    SavedFields fields;
    try {
        const auto values =
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(NicknameKey))
            fields.nickname = winrt::unbox_value<winrt::hstring>(values.Lookup(NicknameKey)).c_str();
        if (values.HasKey(AddressKey))
            fields.address = winrt::unbox_value<winrt::hstring>(values.Lookup(AddressKey)).c_str();
        if (values.HasKey(PortKey)) {
            const auto port = winrt::unbox_value<int32_t>(values.Lookup(PortKey));
            if (port > 0 && port <= 65535) fields.port = static_cast<std::uint16_t>(port);
        }
    } catch (...) { /* Missing or invalid values keep the defaults. */ }
    if (fields.nickname.size() > MaxNicknameLength) fields.nickname.resize(MaxNicknameLength);
    if (fields.address.size() > MaxAddressLength) fields.address.resize(MaxAddressLength);
    return fields;
}

bool SaveFields(const SavedFields& fields) {
    try {
        const auto values =
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(NicknameKey, winrt::box_value(winrt::hstring{fields.nickname}));
        values.Insert(AddressKey, winrt::box_value(winrt::hstring{fields.address}));
        values.Insert(PortKey, winrt::box_value(static_cast<int32_t>(fields.port)));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace Jadyx::Online
