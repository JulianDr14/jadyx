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
#include <vector>

#include "common/common_types.h"
#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_session.h"
#include "jadyx/app/headless_emu_window.h"
#include "jadyx/cheats/cheat_session.h"
#include "jadyx/cheats/cheats_page.h"
#include "jadyx/game/amiibo_page.h"
#include "jadyx/game/game_keyboard.h"
#include "jadyx/game/game_menu.h"
#include "jadyx/game/game_menu_surface.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/memory_edit/jme_screen.h"
#include "jadyx/network/chat_page.h"
#include "jadyx/network/online_chat.h"
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
    /// Stops the guest for what needs it still (a capture, Memory Edit, a resize), when the menu
    /// left it running. Resume undoes only a pause of this.
    void PauseGuest();
    void ResumeGuest();
    /// Runs `work` with the guest stopped, and lets it run again after when the menu is live.
    template <typename Work>
    void WithGuestPaused(Work&& work) {
        const bool was_paused = guest_paused;
        PauseGuest();
        work();
        if (!was_paused) ResumeGuest();
    }
    void DrawMenu();
    void SyncKeyboard();
    void KeyboardPress(std::optional<KeyboardReply> reply);
    void Open();
    void HandleCheatsAction(MenuAction action);
    void SavePulseSession();
    void OpenCheats();
    void OpenAmiibo();
    /// True when the choice closed the menu.
    bool HandleAmiiboAction(MenuAction action);
    void SaveMenuScreenshot();
    void Close(GameMenuResult result);
    void ApplyChoice(GameMenuResult result);
    /// The room chat: its page in the menu, the keyboard that writes to it and the notice over
    /// the game when a message arrives with the menu closed.
    void OpenChat();
    void HandleChatAction(MenuAction action);
    void ChatKeyboardPress(std::optional<KeyboardReply> reply);
    void SendChat(const std::wstring& text);
    void UpdateChat();
    void ShowChatNotice(const Online::ChatSnapshot& chat);
    void HideChatNotice();

    std::optional<GameMenu> menu;
    /// The menu was opened in an online room: the game keeps running behind it (a pause would
    /// freeze the player for the whole room and can drop the session), shown as it is.
    bool live{};
    bool guest_paused{};
    bool graphics_changed{}; ///< In the open menu: the output's size may have to follow.
    /// The glitch hunt page's live renderer switches, kept across menu openings.
    GlitchToggles glitch_toggles;
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
    /// The amiibo page while it is open: its rows and, for each, the file it places.
    struct AmiiboScreen {
        std::vector<std::wstring> rows;
        std::vector<std::string> paths;
        AmiiboPage page;
        bool placed{};
        bool has_key{};
        std::wstring placed_name;
        bool placed_new{};
    };
    std::optional<AmiiboScreen> amiibo_screen;
    /// What the player chose, applied with the guest running: a file's path, or empty to remove;
    /// `new_figure` places it with a random UID.
    struct AmiiboChoice {
        std::string path;
        bool new_figure{};
    };
    std::optional<AmiiboChoice> amiibo_choice;
    /// The file placed last, so the page opens on it again after the game asks for another tap.
    std::string amiibo_last;
    /// Memory Edit, from its first use on: its search outlives the menu, as the player goes back to
    /// the game between numbers. What runs in it stops before the guest does.
    std::optional<MemoryEdit::Screen> memory_edit;
    bool memory_edit_open{};
    std::wstring game_title;
    // Beside the menu: the game's maker and version, and how long this session has run.
    std::wstring game_details;
    const std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::now();
    std::wstring session_text;
    // The paused frame while the menu is open: behind it, and what a screenshot saves.
    D3D12::MenuImage game_frame;
    const Cheats::CheatGame cheat_game;

    struct ChatScreen {
        Online::ChatPage page;
        std::wstring notice; ///< A message that could not go out.
    };
    std::optional<ChatScreen> chat_screen;
    /// Writing a message: the keyboard takes the menu's place until it sends or cancels.
    GameKeyboard chat_keyboard;
    /// The chat as last seen: its version, and the newest line's id.
    std::uint64_t chat_version{};
    std::uint64_t chat_last_id{};
    /// The notice over the game, with a surface of its own while it shows.
    std::optional<GameMenuSurface> notice_surface;
    std::chrono::steady_clock::time_point notice_until{};
    bool notice_shown{};
};

} // namespace Jadyx
