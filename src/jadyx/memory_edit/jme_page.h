// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit's page in the in-game menu: the numeric pad, the list of results, and what each
// button does on them. No core or Windows types, so a test can drive it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "jadyx/memory_edit/jme_value.h"
#include "jadyx/ui/list_navigation.h"
#include "jadyx/ui/ui_actions.h"

namespace Jadyx::MemoryEdit {

inline constexpr std::array<char, 12> PAD_KEYS{'7', '8', '9', '4', '5', '6', '1', '2', '3', '-', '0', '.'};
inline constexpr std::size_t PAD_COLUMNS = 3;
inline constexpr std::size_t PAD_ROWS = PAD_KEYS.size() / PAD_COLUMNS;
/// The 5: as the pad wraps at its edges, no key is more than three presses from it.
inline constexpr std::size_t PAD_START = 4;
inline constexpr std::size_t MAX_TYPED = 24;
inline constexpr std::size_t LIST_ROWS = 5;
/// Automatic, then each kind.
inline constexpr std::size_t KIND_CHOICES = VALUE_KINDS + 1;

enum class PageMode : u8 { Warning, Entry, Searching, List };
enum class EntryPurpose : u8 { Search, Refine, Edit };

/// A result the player marked: a game tends to keep the same number in more than one place, and the
/// marked ones are written together.
struct Marked {
    u64 va{};
    ValueKind kind{};

    bool operator==(const Marked&) const = default;
};

struct Page {
    PageMode mode{PageMode::Entry};
    EntryPurpose purpose{EntryPurpose::Search};
    std::string text;
    std::size_t key{PAD_START};
    std::size_t kind_choice{};
    std::size_t selected{};
    std::size_t first{};
    std::vector<Marked> marked;
    /// The number the list came from: refining starts on it, as the next one is often close.
    std::string searched;
    /// Menu put the page away to show the game, still paused.
    bool hidden{};
};

[[nodiscard]] inline bool IsMarked(const Page& page, const Marked& result) {
    return std::find(page.marked.begin(), page.marked.end(), result) != page.marked.end();
}

/// Marks the result, or unmarks it if it was.
inline void ToggleMark(Page& page, const Marked& result) {
    if (const auto found = std::find(page.marked.begin(), page.marked.end(), result); found != page.marked.end()) {
        page.marked.erase(found);
    } else {
        page.marked.push_back(result);
    }
}

enum class PageEffect : u8 {
    None,
    Redraw,
    Accept, ///< The player took the warning.
    Start,  ///< Search, refine or write what is typed, as `purpose` says.
    Stop,   ///< Stop the search.
    Pick,   ///< Edit the marked results, or the selected one if none is.
    Mark,   ///< Mark or unmark the selected result.
    Leave,  ///< Back to the menu.
};

/// Empty: automatic.
[[nodiscard]] inline std::optional<ValueKind> ChosenKind(const Page& page) {
    if (page.kind_choice == 0) return std::nullopt;
    return static_cast<ValueKind>(page.kind_choice - 1);
}

inline void EnterNumber(Page& page, EntryPurpose purpose, std::string_view text = {}) {
    page.mode = PageMode::Entry;
    page.purpose = purpose;
    page.text = text;
    page.key = PAD_START;
}

/// A sign goes in front whatever the moment it is pressed, and pressed again it goes away.
inline void PressKey(Page& page, char key) {
    if (key == '-') {
        if (!page.text.empty() && page.text.front() == '-') {
            page.text.erase(0, 1);
        } else if (page.text.size() < MAX_TYPED) {
            page.text.insert(0, 1, '-');
        }
        return;
    }
    if (page.text.size() >= MAX_TYPED) return;
    if (key == '.' && page.text.find('.') != std::string::npos) return;
    page.text.push_back(key);
}

namespace Detail {

inline PageEffect HandleEntry(Page& page, MenuAction action, bool has_list) {
    const std::size_t column = page.key % PAD_COLUMNS;
    const std::size_t row = page.key / PAD_COLUMNS;
    switch (action) {
    case MenuAction::Up:
        page.key = (row + PAD_ROWS - 1) % PAD_ROWS * PAD_COLUMNS + column;
        return PageEffect::Redraw;
    case MenuAction::Down:
        page.key = (row + 1) % PAD_ROWS * PAD_COLUMNS + column;
        return PageEffect::Redraw;
    case MenuAction::Left:
        page.key = row * PAD_COLUMNS + (column + PAD_COLUMNS - 1) % PAD_COLUMNS;
        return PageEffect::Redraw;
    case MenuAction::Right:
        page.key = row * PAD_COLUMNS + (column + 1) % PAD_COLUMNS;
        return PageEffect::Redraw;
    case MenuAction::Confirm:
        PressKey(page, PAD_KEYS[page.key]);
        return PageEffect::Redraw;
    case MenuAction::Alternate:
        if (page.text.empty()) return PageEffect::None;
        page.text.pop_back();
        return PageEffect::Redraw;
    case MenuAction::Refresh:
        return page.text.empty() ? PageEffect::None : PageEffect::Start;
    case MenuAction::PreviousTab:
    case MenuAction::NextTab:
        // A refined search keeps each result's kind, and an edit writes the one it has.
        if (page.purpose != EntryPurpose::Search) return PageEffect::None;
        page.kind_choice = action == MenuAction::NextTab ? (page.kind_choice + 1) % KIND_CHOICES
                                                         : (page.kind_choice + KIND_CHOICES - 1) % KIND_CHOICES;
        return PageEffect::Redraw;
    case MenuAction::Back:
        if (!has_list) return PageEffect::Leave;
        page.mode = PageMode::List;
        return PageEffect::Redraw;
    default:
        return PageEffect::None;
    }
}

inline PageEffect HandleList(Page& page, MenuAction action, std::size_t rows) {
    switch (action) {
    case MenuAction::Up:
    case MenuAction::Down:
        if (rows < 2) return PageEffect::None;
        page.selected = action == MenuAction::Up ? (page.selected + rows - 1) % rows : (page.selected + 1) % rows;
        break;
    case MenuAction::Left:
    case MenuAction::Right:
        return rows != 0 ? PageEffect::Mark : PageEffect::None;
    // The menu's input does not repeat a held button: a page at a time is how a long list goes by.
    case MenuAction::PreviousTab:
        if (page.selected == 0) return PageEffect::None;
        page.selected -= std::min(page.selected, LIST_ROWS);
        break;
    case MenuAction::NextTab:
        if (rows == 0 || page.selected + 1 >= rows) return PageEffect::None;
        page.selected = std::min(page.selected + LIST_ROWS, rows - 1);
        break;
    case MenuAction::Confirm:
        return rows != 0 ? PageEffect::Pick : PageEffect::None;
    case MenuAction::Refresh:
        EnterNumber(page, EntryPurpose::Refine, page.searched);
        return PageEffect::Redraw;
    case MenuAction::Alternate:
        // The list stays until a new search really starts: B comes back to it.
        EnterNumber(page, EntryPurpose::Search);
        return PageEffect::Redraw;
    case MenuAction::Back:
        return PageEffect::Leave;
    default:
        return PageEffect::None;
    }
    ClampListWindow(page.selected, page.first, rows, LIST_ROWS);
    return PageEffect::Redraw;
}

} // namespace Detail

/// `rows`: the results the list shows. `has_list`: there are any to go back to.
inline PageEffect HandlePageInput(Page& page, MenuAction action, std::size_t rows, bool has_list) {
    // Menu shows the game behind, to read the number on it, and brings the page back; so does B.
    if (action == MenuAction::Menu || (page.hidden && action == MenuAction::Back)) {
        page.hidden = !page.hidden;
        return PageEffect::Redraw;
    }
    if (page.hidden) return PageEffect::None;
    switch (page.mode) {
    case PageMode::Warning:
        if (action == MenuAction::Confirm) return PageEffect::Accept;
        return action == MenuAction::Back ? PageEffect::Leave : PageEffect::None;
    case PageMode::Entry:
        return Detail::HandleEntry(page, action, has_list);
    case PageMode::Searching:
        return action == MenuAction::Back ? PageEffect::Stop : PageEffect::None;
    case PageMode::List:
        return Detail::HandleList(page, action, rows);
    }
    return PageEffect::None;
}

struct ResultRow {
    std::wstring value;
    std::wstring details;
    bool marked{};
};

/// What the page shows; all of it belongs to the screen.
struct MenuView {
    const Page& page;
    std::span<const ResultRow> rows; ///< Those from `page.first` on.
    u64 hits{};
    u64 shown{};
    bool searching_disk{};
    u32 permille{};
    std::wstring_view kind; ///< The kind in use on the pad. Empty when it is not for the player to see.
    std::wstring_view notice;
    bool notice_warns{};
    std::wstring_view hint;
};

} // namespace Jadyx::MemoryEdit
