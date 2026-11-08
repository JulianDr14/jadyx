// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Online play through Eden's multiplayer rooms, as a client. A room is a relay server (eden-room,
// or an Eden PC hosting one) that forwards the local-wireless (LDN) and LAN packets of everyone in
// it, so the games believe they share one network. The console joins rooms; it never hosts one.
//
// The membership lives in the network library's global RoomMember (network/network.h), so it
// outlives the library screen: join in Settings > Online, then start the game. Joining blocks for
// up to five seconds on the connection, so it runs on a worker; everything here is safe to call
// from the UI thread.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx::Online {

/// The rooms' default UDP port (Network::DefaultRoomPort).
inline constexpr std::uint16_t DefaultPort = 24872;
/// What a room accepts as a nickname: 4 to 20 of [ a-zA-Z0-9._-] (Room::IsValidNickname).
inline constexpr std::size_t MinNicknameLength = 4;
inline constexpr std::size_t MaxNicknameLength = 20;
inline constexpr std::size_t MaxAddressLength = 48;
inline constexpr std::size_t MaxPasswordLength = 32;

enum class Phase : std::uint8_t { Idle, Joining, Joined, Leaving };

/// Why the last join failed or the room closed the connection (RoomMember::Error, plus the
/// failures this module finds before asking the room).
enum class Failure : std::uint8_t {
    None,
    LostConnection,
    Kicked,
    Unknown,
    NameCollision,
    IpCollision,
    WrongVersion,
    WrongPassword,
    CouldNotConnect,
    RoomFull,
    Banned,
    BadNickname,
    BadAddress,
};

struct Member {
    std::string nickname;
    std::string game; ///< What the member is playing; empty in the menus.
};

struct Status {
    Phase phase{Phase::Idle};
    Failure failure{Failure::None};
    std::string room_name;
    std::string address;
    std::uint16_t port{};
    std::uint32_t slots{};
    std::vector<Member> members;
    /// Changes whenever anything above does, so the screen redraws only then.
    std::uint64_t version{};
};

/// The room's view of the connection, copied under a lock.
Status Snapshot();
/// Joined: the game's sockets and local wireless go through the room.
bool InRoom();
/// Starts joining (leaving the current room first). Ignored while another join or leave runs.
/// The nickname must already be valid (MakeNickname).
void Join(const std::string& nickname, const std::string& address, std::uint16_t port,
          const std::string& password);
void Leave();
/// Leaves and waits for the worker: the app is closing.
void Shutdown();

/// The user's name as a room nickname: unsupported characters dropped (accents lose the mark),
/// at most 20, padded to 4 with digits. Empty input gives "Jadyx".
std::string MakeNickname(std::wstring_view name);
/// A port typed by the user, or nullopt when it is not 1..65535.
std::optional<std::uint16_t> ParsePort(std::wstring_view text);

/// One room of the public lobby (Eden's multiplayer list).
struct LobbyRoom {
    std::string name, description, owner, game, address;
    std::uint16_t port{};
    std::uint32_t players{}, max_players{};
    bool password{};
};

enum class LobbyState : std::uint8_t { Empty, Loading, Ready, Failed };

struct LobbySnapshot {
    LobbyState state{LobbyState::Empty};
    std::vector<LobbyRoom> rooms; ///< Only the rooms this build can join (same network version).
    std::size_t other_versions{}; ///< Rooms left out for another network version.
    std::uint64_t version{};
};

/// Downloads the room list again in the background. Ignored while a download runs.
void RefreshLobby();
LobbySnapshot Lobby();

/// What the Online page remembers between launches (the password is never saved).
struct SavedFields {
    std::wstring nickname; ///< Empty: the system user name.
    std::wstring address;
    std::uint16_t port{DefaultPort};
};
SavedFields LoadSavedFields();
bool SaveFields(const SavedFields& fields);

} // namespace Jadyx::Online
