// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Online play through Eden's multiplayer rooms. A room is a relay server (eden-room, an Eden PC,
// or this console) that forwards the local-wireless (LDN) and LAN packets of everyone in it, so the
// games believe they share one network. The console joins rooms, or hosts a private one (not
// announced in the public lobby) and joins it as its first member.
//
// The membership lives in the network library's global RoomMember (network/network.h), so it
// outlives the library screen: join in the Online tab, then start the game. Joining blocks for
// up to five seconds on the connection, so it runs on a worker; everything here is safe to call
// from the UI thread.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jadyx/network/online_chat.h"

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
    BadRoomName,  ///< Hosting: the name is not 4 to 20 of [ a-zA-Z0-9._-].
    CouldNotHost, ///< Hosting: the port could not be opened (in use, or not allowed).
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
    /// This console hosts the room it is in (or is about to).
    bool hosting{};
    /// Hosting: the console's address on its local network, to share; empty when unknown.
    std::string local_address;
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
/// Leaves the room; when hosting, closes it for everyone.
void Leave();

/// Room sizes a host can pick (Eden's host dialog allows the same).
inline constexpr std::uint32_t MinSlots = 2;
inline constexpr std::uint32_t MaxSlots = 16;
inline constexpr std::uint32_t DefaultSlots = 8;

struct HostOptions {
    std::string name; ///< Already valid (MakeRoomName).
    std::uint32_t slots{DefaultSlots};
    std::uint16_t port{DefaultPort};
    std::string password;
};
/// Opens a private room on this console and joins it (leaving the current room first). Ignored
/// while another join or leave runs.
void Host(const std::string& nickname, const HostOptions& options);
/// Leaves and waits for the worker: the app is closing.
void Shutdown();

/// The room chat (text only: Eden's rooms carry no voice). Lines arrive while joined and are
/// cleared when the room is left.
ChatSnapshot Chat();
/// Changes whenever Chat() would: a cheap check before copying the history.
std::uint64_t ChatVersion();
/// Sends a message to the room and adds it to the history; false when not joined or when the
/// text is empty once cleaned. Longer than MaxChatBytes is cut on a character boundary.
bool SendChat(std::string_view utf8);
/// The chat was seen: the unread count goes back to zero.
void MarkChatRead();
/// Hides a member's messages until the room is left.
void MuteMember(const std::string& nickname, bool mute);
bool IsMemberMuted(const std::string& nickname);
/// This console's nickname in the room; empty when not joined.
std::string OwnNickname();

/// Where a message from the room shows a notice when the chat is closed.
enum class ChatNotices : std::uint8_t { Everywhere, LibraryOnly, Off };
ChatNotices LoadChatNotices();
void SaveChatNotices(ChatNotices notices);

/// The user's name as a room nickname: unsupported characters dropped (accents lose the mark),
/// at most 20, padded to 4 with digits. Empty input gives "Jadyx".
std::string MakeNickname(std::wstring_view name);
/// A room name with the nickname's rules (same characters and lengths); empty input stays empty.
std::string MakeRoomName(std::wstring_view name);
/// A player count typed by the user, or nullopt outside MinSlots..MaxSlots.
std::optional<std::uint32_t> ParseSlots(std::wstring_view text);
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
    std::wstring room_name; ///< Hosting; empty: named after the nickname.
    std::uint32_t slots{DefaultSlots};
    std::uint16_t host_port{DefaultPort};
};
SavedFields LoadSavedFields();
bool SaveFields(const SavedFields& fields);

} // namespace Jadyx::Online
