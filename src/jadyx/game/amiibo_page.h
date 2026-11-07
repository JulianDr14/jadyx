// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The in-game menu's amiibo page: the player's files while the game waits for a tag, or the single
// row that takes the current one away. Choosing closes the menu: the session applies it with the
// guest running. No core or Windows types, so a test can drive it.
//
// "New figure" (X) places the file with a random UID, so a game that takes each figure once a day
// sees one it has not met. It starts off every time the page opens: companions that keep their
// progress on the figure need their own UID, and the plain A press must always give it.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "jadyx/ui/list_navigation.h"
#include "jadyx/ui/ui_actions.h"

namespace Jadyx {

struct AmiiboPage {
    std::size_t selected{};
    std::size_t first{};
    bool new_figure{}; ///< The next placement reports a random UID.
};

/// What the page shows; the rows belong to the session.
struct AmiiboMenuView {
    std::span<const std::wstring> rows;
    const AmiiboPage& page;
    bool placed{};  ///< The game has a tag: the only row removes it.
    bool has_key{}; ///< Without key_retail.bin the game can read a tag but not save on it.
    std::wstring_view placed_name{}; ///< The tag the game reads, when `placed`.
    bool placed_new{};               ///< That tag went in as a new figure.
};

enum class AmiiboEffect { None, Moved, Choose, Back, Toggled };

/// `placed`: the page only offers to remove the tag, so there is no figure to choose a UID for.
inline AmiiboEffect HandleAmiiboInput(AmiiboPage& page, MenuAction action, std::size_t rows, std::size_t visible,
                                      bool placed = false) {
    switch (action) {
    case MenuAction::Alternate:
        if (placed || rows == 0) return AmiiboEffect::None;
        page.new_figure = !page.new_figure;
        return AmiiboEffect::Toggled;
    case MenuAction::Up:
    case MenuAction::Down: {
        if (rows < 2) return AmiiboEffect::None;
        page.selected = action == MenuAction::Up ? (page.selected + rows - 1) % rows : (page.selected + 1) % rows;
        ClampListWindow(page.selected, page.first, rows, visible);
        return AmiiboEffect::Moved;
    }
    case MenuAction::Confirm:
        return rows != 0 ? AmiiboEffect::Choose : AmiiboEffect::None;
    case MenuAction::Back:
        return AmiiboEffect::Back;
    default:
        return AmiiboEffect::None;
    }
}

/// How a stored file reads on screen: its name, without folders or the .bin extension.
inline std::string AmiiboDisplayName(std::string_view path) {
    if (const auto slash = path.find_last_of("/\\"); slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    if (path.size() > 4 && path[path.size() - 4] == '.') path.remove_suffix(4);
    return std::string{path};
}

} // namespace Jadyx
