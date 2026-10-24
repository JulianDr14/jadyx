// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The selected game's management tabs. Selecting a category activates its pane directly;
// Down moves into the pane; its hero can fold while both navigation bars remain visible.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_icon.h"

namespace Jadyx {

/// Pulse's pane shows the kept session and Remove's what deleting the game takes with it. Both
/// act in place and only on a second confirmation.
enum class GameOption : std::uint8_t { Information, Cheats, Mods, Updates, Dlc, Pulse, Remove };

/// The options shown, in order.
inline std::span<const GameOption> GameOptions() {
    static constexpr std::array options{
        GameOption::Information, GameOption::Mods,  GameOption::Cheats, GameOption::Updates,
        GameOption::Dlc,         GameOption::Pulse, GameOption::Remove};
    return options;
}

/// False for the panes that act in place (with a second press) instead of opening a manager.
inline bool GameOptionHasManager(GameOption option) {
    return option != GameOption::Pulse && option != GameOption::Remove;
}

inline I18n::Text GameOptionTitle(GameOption option) {
    switch (option) {
    case GameOption::Information:
        return I18n::Text::GameInfoTitle;
    case GameOption::Updates: return I18n::Text::ContentUpdates;
    case GameOption::Dlc: return I18n::Text::ContentDlc;
    case GameOption::Cheats: return I18n::Text::CheatsTitle;
    case GameOption::Mods: return I18n::Text::ModsTitle;
    case GameOption::Pulse: return I18n::Text::PulseSession;
    case GameOption::Remove: return I18n::Text::GameRemove;
    }
    return I18n::Text::CheatsTitle;
}

inline I18n::Text GameOptionHint(GameOption option) {
    switch (option) {
    case GameOption::Information:
        return I18n::Text::GameInfoTitle;
    case GameOption::Updates:
    case GameOption::Dlc: return I18n::Text::ContentCategoryHint;
    case GameOption::Cheats: return I18n::Text::CheatsHint;
    case GameOption::Mods: return I18n::Text::ModsHint;
    case GameOption::Pulse: return I18n::Text::PulseNone;
    case GameOption::Remove: return I18n::Text::GameRemoveHint;
    }
    return I18n::Text::CheatsHint;
}

inline UiIcon GameOptionIcon(GameOption option) {
    switch (option) {
    case GameOption::Information:
        return UiIcon::Folder;
    case GameOption::Updates:
    case GameOption::Dlc: return UiIcon::Folder;
    case GameOption::Cheats: return UiIcon::Cheats;
    case GameOption::Mods: return UiIcon::Mods;
    case GameOption::Pulse: return UiIcon::Archive;
    case GameOption::Remove: return UiIcon::Remove;
    }
    return UiIcon::Settings;
}

enum class GameOptionsFocus : std::uint8_t { Sections, ManagerTabs, Content };

struct GameOptionsPage {
    bool open{};
    std::size_t game{};     ///< The library entry the options are for.
    std::size_t selected{}; ///< Active category in GameOptions().
    std::optional<GameOption> page; ///< Selected manager; Pulse has its own pane in the shell.
    bool confirm_discard{}; ///< A was pressed once on Pulse or Remove; a second press acts.
    bool discard{};         ///< Confirmed: the library discards the session or deletes the game.
    /// The pane below the bars: LB/RB switch sections and LT/RT the manager's tabs, so the
    /// stick and the d-pad only move inside the pane. A pointer can still focus a bar.
    GameOptionsFocus focus{GameOptionsFocus::Content};
    bool activate{true};    ///< The coordinator initializes the selected manager once.
};

inline void SelectGameOption(GameOptionsPage& options, std::size_t index) {
    if (index >= GameOptions().size()) return;
    options.selected = index;
    options.page = GameOptionHasManager(GameOptions()[index]) ? std::optional{GameOptions()[index]} :
                   std::nullopt;
    options.confirm_discard = options.discard = false;
    options.focus = GameOptionsFocus::Content;
    options.activate = true;
}

/// Navigation has three explicit levels; entering one never recreates its manager.
inline void EnterGameOptionsPane(GameOptionsPage& options, bool has_tabs) {
    options.confirm_discard = false;
    options.focus = options.focus == GameOptionsFocus::Sections && has_tabs ?
        GameOptionsFocus::ManagerTabs : GameOptionsFocus::Content;
}

inline void LeaveGameOptionsPane(GameOptionsPage& options, bool has_tabs) {
    options.confirm_discard = false;
    options.focus = options.focus == GameOptionsFocus::Content && has_tabs ?
        GameOptionsFocus::ManagerTabs : GameOptionsFocus::Sections;
}

enum class GameOptionsInput : std::uint8_t { Up, Down, Confirm, Back };

/// Pure tab/pane navigation; managers handle their own content inputs.
inline void HandleGameOptionsInput(GameOptionsPage& options, GameOptionsInput input) {
    const std::size_t count = GameOptions().size();
    const bool confirming = std::exchange(options.confirm_discard, false);
    switch (input) {
    case GameOptionsInput::Up:
        SelectGameOption(options, (options.selected + count - 1) % count);
        break;
    case GameOptionsInput::Down:
        SelectGameOption(options, (options.selected + 1) % count);
        break;
    case GameOptionsInput::Confirm:
        if (!GameOptionHasManager(GameOptions()[options.selected])) {
            options.confirm_discard = !confirming;
            options.discard = confirming;
            break;
        }
        EnterGameOptionsPane(options, false);
        break;
    case GameOptionsInput::Back:
        options.open = false;
        break;
    }
}

/// Geometry shared by the canvas and pointer hit tests (1280x720 layout units).
/// The game's line (cover, title, developer, version, build) folds away while the content has
/// the focus; the two tab bars move up with it and stay in sight.
struct GameOptionsLayout {
    static constexpr float GlobalHeaderBottom = 94;
    static constexpr float HeroTop = 100, HeroHeight = 48, HeroClipBottom = 152;
    static constexpr float SectionsTop = 156, SectionsHeight = 44;
    static constexpr float HeroCollapse = HeroClipBottom - GlobalHeaderBottom;
    static constexpr float BodyTop = 210, ListBottom = 620, GridBottom = 656;
    /// One or two lines about the selection, then a hairline and the button prompts.
    static constexpr float Status = 626, FooterLine = 664, Footer = 676;
};

} // namespace Jadyx
