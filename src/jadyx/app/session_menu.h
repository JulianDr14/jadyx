// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// In-game menu (game_menu.h): the guest pauses while it is open. The frontend draws it with real
// text (game_menu_surface.h) and the renderer copies it over the last frame. Its cheats page changes
// the running game's cheats at once: the engine reloads them on its next tick after the guest
// resumes. Closed, the menu keeps nothing.

#include <chrono>
#include <optional>
#include <string>

#include "common/common_types.h"
#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_session.h"
#include "jadyx/app/headless_emu_window.h"
#include "jadyx/cheats/cheat_session.h"
#include "jadyx/cheats/cheats_page.h"
#include "jadyx/game/game_keyboard.h"
#include "jadyx/game/game_menu.h"
#include "jadyx/game/game_menu_surface.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/pulse/pulse_screen.h"
#include "jadyx/pulse/pulse_sessions.h"
#include "jadyx/ui/launch_splash.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace Core {
class System;
}

namespace Jadyx {

class UwpSoftwareKeyboard;

/// What the menu works on, all of it the session's.
struct SessionMenuContext {
    Core::System& system;
    GamepadInput& input;
    HeadlessEmuWindow& emu_window;
    const BootConfig& config;
    const BootSurface& surface;
    std::optional<PulseScreen>& pulse_screen;
    std::optional<LaunchScreen>& launch_screen;
    const std::optional<Common::Pulse::Identity>& pulse_identity;
    UwpSoftwareKeyboard* keyboard_applet;
    u64 program_id;
    bool mii_editor;
};

class SessionMenu : private SessionMenuContext {
public:
    enum class Outcome { Stay, BackToLibrary };

    explicit SessionMenu(const SessionMenuContext& context);

    [[nodiscard]] bool IsOpen() const { return menu.has_value(); }

    /// One tick of the play loop: the game's keyboard, the menu's input and the cheats database.
    /// BackToLibrary: the player left (any session is already saved) and the game must end.
    Outcome Update();

private:
    void CaptureFrame();
    void DrawMenu();
    void SyncKeyboard();
    void KeyboardPress(std::optional<KeyboardReply> reply);
    void Open();
    void HandleCheatsAction(MenuAction action);
    void SavePulseSession();
    void OpenCheats();
    void SaveMenuScreenshot();
    void Close(GameMenuResult result);
    void ApplyChoice(GameMenuResult result);

    std::optional<GameMenu> menu;
    bool graphics_changed{}; ///< In the open menu: the output's size may have to follow.
    std::optional<GameMenuSurface> menu_surface;
    // A keyboard the game opened (game_keyboard.h): drawn on the same surface as the menu, over
    // the running game. The menu opens on top of it and gives it back when it closes.
    GameKeyboard game_keyboard;
    bool keyboard_shown = false;
    struct TextEntryOff {
        ~TextEntryOff() { SetTextEntryActive(false); }
    } text_entry_off;
    std::optional<Cheats::CheatSession> cheats;
    Cheats::CheatsPage cheats_page;
    std::wstring cheats_notice;
    std::wstring game_title;
    // Beside the menu: the game's maker and version, and how long this session has run.
    std::wstring game_details;
    const std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::now();
    std::wstring session_text;
    // The paused frame while the menu is open: behind it, and what a screenshot saves.
    D3D12::MenuImage game_frame;
    const Cheats::CheatGame cheat_game;
};

} // namespace Jadyx
