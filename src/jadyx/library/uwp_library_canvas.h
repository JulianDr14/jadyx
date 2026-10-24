// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "jadyx/library/compatibility.h"
#include <vector>
#include "jadyx/content/content_session.h"
#include <string_view>
#include "jadyx/cheats/cheat_session.h"
#include "jadyx/cheats/cheats_page.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/game_options_page.h"
#include "jadyx/library/game_information.h"
#include "jadyx/library/library_layout.h"
#include "jadyx/mods/mod_session.h"
#include "jadyx/mods/mods_page.h"
#include "jadyx/settings/system_page.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/input/uwp_controllers.h"
#include "jadyx/storage/uwp_file_manager.h"
namespace Jadyx {
/// The selected game's options page, when open.
struct GameOptionsView {
    const GameOptionsPage& page;
    const LibraryEntry* game;            ///< nullptr when the page is closed.
    const Cheats::CheatSession* cheats;  ///< The open cheats page's.
    const Cheats::CheatsPage& cheats_page;
    std::wstring_view notice;
    const Content::ContentSession* content{};
    const Mods::ModSession* mods{};      ///< The open mods page's.
    const Mods::ModsPage* mods_page{};
    const GameInformationFacts* information{};
    const Compatibility::Table* compatibility{};
    bool other_copies{};                 ///< Another dump of the game stays if this one is deleted.
};
/// The active controller's battery, as Windows.Gaming.Input reports it; unknown when it does not.
struct ControllerBattery {
    bool known{};
    float level{};   ///< 0..1, remaining over full capacity.
    bool charging{};
    bool operator==(const ControllerBattery&) const = default;
};
/// Everything one frame of the library shows.
struct LibraryView {
    const LibraryScan& scan;
    std::size_t selected;
    bool loading;
    bool controller_panel;
    unsigned controller_row;
    const ControllerOptions& options;
    const std::wstring& notice;
    const ControllerPanel& panel;
    const ConfigurationPanel& configuration;
    const SystemPage& system;
    const GameOptionsView& game_options;
    bool missing_content{};
    bool rail{}; ///< The focus is in the played games' row (library_layout.h).
    LibraryLayout::LibraryTab tab{};
    LibraryLayout::Filter filter{};
    const std::vector<std::size_t>* shelf{}; ///< The Library tab's entries, filtered (LibraryLayout::Shelf).
    std::size_t shelf_focus{};               ///< In `shelf`; its size is the import tile.
    bool filter_focus{};                     ///< The Library tab's focus is on the filter bar.
    ControllerBattery battery{};
    const Compatibility::Table* compatibility{};
};
/// What the pointer can press on the library screen.
enum class LibraryHit : unsigned {
    None, Controller, Play, Options, Tab, Filter, RailTile, Tile, ShelfTile, Import,
    GameOptionSelect, ManagerTab, ManagerRow, ManagerCard,
    Category,   ///< A settings category (ConfigurationHomeRow).
    SettingRow, ///< A row of the settings category shown.
    UpdateAction,  ///< A row of the update page, or UpdateBackHit for its Back prompt.
    NetworkAction, ///< Full-screen network page: back, renew code, stop.
    UpdateNotice,  ///< The library's new-version notice: 0 opens the update page, 1 hides it.
};
inline constexpr std::size_t UpdateBackHit = 99;
/// A press: what, and which (the tab, the filter, the entry, the shelf position or the row).
struct LibraryTarget {
    LibraryHit hit{};
    std::size_t index{};
};
class LibraryCanvas {
public:
    LibraryCanvas(void* window, unsigned width, unsigned height);
    ~LibraryCanvas();
    /// Drops decoded covers, e.g. after the library cache is cleared.
    void InvalidateCovers();
    void Draw(const LibraryView& view);
    /// A transition is under way: draw again next frame even if nothing changed.
    bool Animating() const;
    /// What is under (x, y) in layout units, as the last frame drew it.
    LibraryTarget HitAt(float x, float y) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
