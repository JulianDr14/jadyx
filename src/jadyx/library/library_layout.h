// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The library screen's geometry and focus, shared by the canvas that draws it and the input that
// moves on it (1280x720 layout units). Three tabs under one header: Home, where the focused game
// fills the hero and one row of square covers sits in focus under it (the pinned and played games
// first, the rail, then the whole collection seven to a row); Library, the collection as a grid
// under a filter; and Settings. Moving between rows scrolls so the focused row lands in the same
// place.

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <string_view>
#include <vector>

#include "jadyx/library/game_library.h"
#include "jadyx/ui/ui_theme.h"

namespace Jadyx::LibraryLayout {
using Canvas = ThemeTokens::Canvas;

inline constexpr float Margin = Canvas::Unit64;
inline constexpr std::size_t Columns = 7;
inline constexpr float Tile = Canvas::Unit144;
inline constexpr float Gutter = Canvas::Unit24;
inline constexpr float Stride = Tile + Gutter; ///< 7 tiles and 6 gutters fill 1152 = 1280 - 2x64.
/// The focused row's covers, on screen.
inline constexpr float RowTop = Canvas::Unit474;
/// One row: the cover, its title and meta line, and the gap to the next.
inline constexpr float RowHeight = Canvas::Unit216;
/// A section title above a section's first row.
inline constexpr float HeadingHeight = Canvas::Unit42;

// The header's two buttons and the hero's actions (their widths follow their labels: the canvas
// reports them for the pointer, see Hits).
inline constexpr float HeaderTop = Canvas::Unit34;
inline constexpr float HeaderButtonHeight = Canvas::Unit40;
inline constexpr float PillTop = Canvas::Unit360;
inline constexpr float PillHeight = Canvas::Unit44;
inline constexpr float FooterTop = Canvas::Unit664;

/// The rail holds the pinned games, then the played ones (the library's order puts them first),
/// at most one row of them.
inline std::size_t RailCount(const std::vector<LibraryEntry>& entries) {
    std::size_t count = 0;
    while (count < std::min(entries.size(), Columns) &&
           (entries[count].pinned || entries[count].last_played > 0))
        ++count;
    return count;
}

enum class LibraryTab : unsigned { Home, Library, Settings, Count };

/// The focus: an entry, and whether it is focused in the rail (the same game also has a place
/// in the collection).
struct Focus {
    std::size_t selected{};
    bool rail{};
};

/// The row the focus is on, counting the rail as row 0 when there is one.
inline std::size_t RowOf(Focus focus, std::size_t rail_count) {
    if (focus.rail) return 0;
    return (rail_count ? 1 : 0) + focus.selected / Columns;
}

/// Where a row's covers are in the scrolling content (the rail, or the first row, at 0).
inline float RowContentTop(std::size_t row, std::size_t rail_count) {
    if (rail_count && row > Canvas::Unit0)
        return RowHeight + HeadingHeight + (row - Canvas::Unit1) * RowHeight;
    return static_cast<float>(row) * RowHeight;
}

inline float TileLeft(std::size_t column) {
    return Margin + static_cast<float>(column) * Stride;
}

inline std::size_t ColumnOf(Focus focus) {
    return focus.rail ? focus.selected : focus.selected % Columns;
}

/// The focus after a d-pad move; unchanged when there is nowhere to go.
enum class Move { Left, Right, Up, Down };
inline Focus Moved(Focus focus, Move move, std::size_t count, std::size_t rail_count) {
    if (count == 0) return focus;
    switch (move) {
    case Move::Left:
        if (focus.selected > 0) --focus.selected;
        break;
    case Move::Right:
        if (focus.selected + 1 < (focus.rail ? rail_count : count)) ++focus.selected;
        break;
    case Move::Up:
        if (focus.rail) break;
        if (focus.selected >= Columns) focus.selected -= Columns;
        else if (rail_count) focus = {std::min(focus.selected, rail_count - 1), true};
        break;
    case Move::Down:
        if (focus.rail) focus = {focus.selected, false};
        else if (focus.selected + Columns < count) focus.selected += Columns;
        // The last row may be shorter: its last game, as long as it is a row further down.
        else if (focus.selected / Columns < (count - 1) / Columns) focus.selected = count - 1;
        break;
    }
    return focus;
}

/// The entries whose covers the screen may show around the focus: the rail and the focused row's
/// neighbours, nearest first. Covers outside are released.
struct CoverWindow {
    std::size_t first; ///< Collection rows [first, end) as entry indices.
    std::size_t end;
    std::size_t rail;  ///< Entries [0, rail) for the rail.
    bool Contains(std::size_t index) const {
        return index < rail || (index >= first && index < end);
    }
};
inline CoverWindow CoversAround(Focus focus, std::size_t count, std::size_t rail_count) {
    const std::size_t row = focus.rail ? 0 : focus.selected / Columns;
    const std::size_t first = (row > 0 ? row - 1 : 0) * Columns;
    return {first, std::min(count, (row + 3) * Columns), rail_count};
}

// ---- The Library tab: the collection as a grid under a filter ----

enum class Filter : unsigned { All, Played, Alphabetical, Pinned, Recent, Pulse, Count };
inline constexpr std::size_t FilterCount = static_cast<std::size_t>(Filter::Count);

/// The heading, its count and the filter bar on one line; the grid under them.
inline constexpr float ShelfHeadingTop = Canvas::Unit100;
inline constexpr float FilterTop = Canvas::Unit106;
inline constexpr float FilterHeight = Canvas::Unit36;
inline constexpr float ShelfTop = Canvas::Unit206;
inline constexpr float ShelfRowHeight = Canvas::Unit204;

/// The entries the filter shows, as indices into `entries`, in the order it shows them. All keeps
/// the library's order (pinned and played first); A-Z is by title; Recently added newest first.
inline std::vector<std::size_t> Shelf(const std::vector<LibraryEntry>& entries, Filter filter) {
    std::vector<std::size_t> shelf;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const bool shown = filter == Filter::Played ? entry.last_played > 0
                         : filter == Filter::Pinned ? entry.pinned
                         : filter == Filter::Pulse  ? !entry.pulse_saved.empty()
                                                    : true;
        if (shown) shelf.push_back(i);
    }
    if (filter == Filter::Alphabetical) {
        std::stable_sort(shelf.begin(), shelf.end(), [&](std::size_t a, std::size_t b) {
            const std::wstring_view x = entries[a].name, y = entries[b].name;
            return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(), [](wchar_t l, wchar_t r) {
                return std::towlower(l) < std::towlower(r);
            });
        });
    } else if (filter == Filter::Recent) {
        std::stable_sort(shelf.begin(), shelf.end(),
                         [&](std::size_t a, std::size_t b) { return entries[a].added > entries[b].added; });
    } else if (filter == Filter::Played) {
        std::stable_sort(shelf.begin(), shelf.end(), [&](std::size_t a, std::size_t b) {
            return entries[a].last_played > entries[b].last_played;
        });
    }
    return shelf;
}

/// The whole collection ends with the tile that opens the file manager to import more.
inline bool HasImportTile(Filter filter) {
    return filter == Filter::All;
}

/// Where the grid's rows are scrolled to: the focused row stays on the first two lines.
inline float ShelfScroll(std::size_t focus) {
    const std::size_t row = focus / Columns;
    return static_cast<float>(row > 0 ? row - 1 : 0) * ShelfRowHeight;
}

/// The shelf focus after a d-pad move over `count` tiles (the import tile included); Up from the
/// first row leaves the grid for the filter bar, which the caller handles.
inline std::size_t ShelfMoved(std::size_t focus, Move move, std::size_t count) {
    if (count == 0) return 0;
    switch (move) {
    case Move::Left: return focus > 0 ? focus - 1 : focus;
    case Move::Right: return focus + 1 < count ? focus + 1 : focus;
    case Move::Up: return focus >= Columns ? focus - Columns : focus;
    case Move::Down:
        if (focus + Columns < count) return focus + Columns;
        return focus / Columns < (count - 1) / Columns ? count - 1 : focus;
    }
    return focus;
}

} // namespace Jadyx::LibraryLayout
