// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Online tab: join a multiplayer room by its address, or one from the public list. Two
// columns: at the left the connection (its state and the nickname) over the direct join (address,
// port, password and the join/leave button); at the right the public rooms. `selected` counts the
// left column's rows first, then one per room. The library routes input
// (library_session_pages.cpp) and edits the text fields with the System page's keyboard; the
// canvas draws from this state.

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

    bool open{}; ///< The tab shows.
    std::size_t selected{Address};
    std::size_t last_form{Address}; ///< Where Left returns from the room list.
    std::size_t last_room{};        ///< Where Right returns to in the room list.
    Online::SavedFields fields;  ///< As saved.
    std::wstring password;       ///< For the direct join; kept until the app closes, never saved.
    Online::Status status;       ///< Copied each frame the Settings tab shows.
    Online::LobbySnapshot lobby; ///< Likewise.
    /// The public room whose password the keyboard is asking for (a copy: the list may refresh).
    std::optional<Online::LobbyRoom> pending_room;
};

inline std::size_t OnlineRowCount(const OnlinePage& page) {
    return OnlinePage::FixedRows + page.lobby.rooms.size();
}

/// The focus is in the room list (the right column).
inline bool OnlineInRooms(const OnlinePage& page) { return page.selected >= OnlinePage::FixedRows; }

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
