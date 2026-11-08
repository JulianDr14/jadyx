// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Online tab: join a multiplayer room by its address or from the public list, or host a private
// one. Two columns: at the left the connection (its state and the nickname) over a card with two
// modes, Join (address, port, password and the join/leave button) and Host (room name, players and
// port side by side, password and the create/close button); at the right the public rooms while
// joining, or how friends reach the hosted room. In Join mode `selected` counts the left column's
// rows first, then one per room; in Host mode `host_selected` walks the host rows. The library
// routes input (library_session_pages.cpp) and edits the text fields with the System page's
// keyboard; the canvas draws from this state.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "common/string_util.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/network/online_rooms.h"

namespace Jadyx {

struct OnlinePage {
    enum Row : std::size_t { Nickname, Address, Port, Password, Connect, FixedRows };
    /// Host mode's rows. Slots and HostPort share a line: Left and Right move between them.
    enum HostRow : std::size_t { HostNickname, HostName, HostSlots, HostPort, HostPassword, HostCreate, HostRows };

    bool open{}; ///< The tab shows.
    bool host{}; ///< The left card is in Host mode (Y switches it).
    std::size_t host_selected{HostName};
    std::size_t selected{Address};
    std::size_t last_form{Address}; ///< Where Left returns from the room list.
    std::size_t last_room{};        ///< Where Right returns to in the room list.
    Online::SavedFields fields;  ///< As saved.
    std::wstring password;       ///< For the direct join; kept until the app closes, never saved.
    std::wstring host_password;  ///< For the hosted room; likewise.
    Online::Status status;       ///< Copied each frame the Settings tab shows.
    Online::LobbySnapshot lobby; ///< Likewise.
    /// The public room whose password the keyboard is asking for (a copy: the list may refresh).
    std::optional<Online::LobbyRoom> pending_room;
};

inline std::size_t OnlineRowCount(const OnlinePage& page) {
    return OnlinePage::FixedRows + page.lobby.rooms.size();
}

/// The focus is in the room list (the right column, which lists rooms in Join mode only).
inline bool OnlineInRooms(const OnlinePage& page) { return !page.host && page.selected >= OnlinePage::FixedRows; }

/// The joined room is this public one (the lobby lists the address the room announces).
inline bool OnlineIsCurrentRoom(const Online::Status& status, const Online::LobbyRoom& room) {
    return status.phase == Online::Phase::Joined && status.port == room.port &&
           (status.address == room.address || (!status.room_name.empty() && status.room_name == room.name));
}

/// The public room a row shows, or null for the fixed rows.
inline const Online::LobbyRoom* OnlineRoomAt(const OnlinePage& page, std::size_t row) {
    if (row < OnlinePage::FixedRows) return nullptr;
    const std::size_t index = row - OnlinePage::FixedRows;
    return index < page.lobby.rooms.size() ? &page.lobby.rooms[index] : nullptr;
}

/// The nickname rooms see: the one typed here, or the console's user name made acceptable.
inline std::string OnlineNickname(const OnlinePage& page, std::wstring_view user_name) {
    return Online::MakeNickname(page.fields.nickname.empty() ? user_name : page.fields.nickname);
}

/// The hosted room's name: the one typed here, or one made from the nickname.
inline std::string OnlineRoomName(const OnlinePage& page, std::wstring_view user_name) {
    if (!page.fields.room_name.empty()) return Online::MakeRoomName(page.fields.room_name);
    const std::wstring nickname = Common::UTF8ToUTF16W(OnlineNickname(page, user_name));
    return Online::MakeRoomName(I18n::Format(I18n::Text::OnlineRoomOf, {nickname}));
}

/// The console is in a room it hosts, or opening one.
inline bool OnlineHosting(const Online::Status& status) {
    return status.hosting && (status.phase == Online::Phase::Joined || status.phase == Online::Phase::Joining);
}

inline std::wstring OnlineWiden(const std::string& text) { return Common::UTF8ToUTF16W(text); }
inline std::string OnlineNarrow(std::wstring_view text) { return Common::UTF16ToUTF8(text); }

inline I18n::Text OnlineFailureText(Online::Failure failure) {
    using I18n::Text;
    using Online::Failure;
    switch (failure) {
    case Failure::LostConnection:  return Text::OnlineFailLost;
    case Failure::Kicked:          return Text::OnlineFailKicked;
    case Failure::NameCollision:   return Text::OnlineFailName;
    case Failure::IpCollision:     return Text::OnlineFailIp;
    case Failure::WrongVersion:    return Text::OnlineFailVersion;
    case Failure::WrongPassword:   return Text::OnlineFailPassword;
    case Failure::CouldNotConnect: return Text::OnlineFailConnect;
    case Failure::RoomFull:        return Text::OnlineFailFull;
    case Failure::Banned:          return Text::OnlineFailBanned;
    case Failure::BadNickname:     return Text::OnlineFailNickname;
    case Failure::BadAddress:      return Text::OnlineFailAddress;
    case Failure::BadRoomName:     return Text::OnlineFailRoomName;
    case Failure::CouldNotHost:    return Text::OnlineFailHost;
    default:                       return Text::OnlineFailUnknown;
    }
}

/// Splits "host:port" typed in the address field; the port stays as it was without one.
inline void SplitAddress(std::wstring& address, std::uint16_t& port) {
    while (!address.empty() && address.back() == L' ') address.pop_back();
    while (!address.empty() && address.front() == L' ') address.erase(address.begin());
    const auto colon = address.rfind(L':');
    if (colon == std::wstring::npos || address.find(L':') != colon) return; // None, or IPv6.
    if (const auto typed = Online::ParsePort(std::wstring_view{address}.substr(colon + 1))) {
        port = *typed;
        address.resize(colon);
    }
}

} // namespace Jadyx
