// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The in-game menu's amiibo page: the player's files while the game waits for a tag, or the single
// row that takes the current one away. Choosing closes the menu: the session applies it with the
// guest running. No core or Windows types, so a test can drive it.

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
};

/// What the page shows; the rows belong to the session.
struct AmiiboMenuView {
    std::span<const std::wstring> rows;
    const AmiiboPage& page;
    bool placed{};  ///< The game has a tag: the only row removes it.
    bool has_key{}; ///< Without key_retail.bin the game can read a tag but not save on it.
};

enum class AmiiboEffect { None, Moved, Choose, Back };

inline AmiiboEffect HandleAmiiboInput(AmiiboPage& page, MenuAction action, std::size_t rows, std::size_t visible) {
    switch (action) {
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
