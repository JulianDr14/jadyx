// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The in-game menu drawn with real text: Direct2D on a CPU bitmap (WIC), so the game's D3D12
// device is not shared and nothing is resident on the GPU. The menu and its cheats page take the
// whole screen over the paused frame, blurred; the game's keyboard is a panel the renderer copies
// over the running game. A surface lives only while it is open: closed, it costs nothing.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"
#include "jadyx/network/chat_page.h"

namespace Jadyx {

class GameMenu;
struct AmiiboMenuView;
struct TextEntryPanelView;
namespace Cheats {
struct CheatsView;
}
namespace MemoryEdit {
struct MenuView;
}
namespace Online {
struct ChatPage;
}

/// The room chat's page: the history, or the quick phrases while the page picks one.
struct ChatMenuView {
    std::span<const ChatMenuLine> lines;
    std::wstring_view subtitle; ///< The room and its players.
    const Online::ChatPage& page;
    std::span<const std::wstring> phrases;
    std::wstring_view notice; ///< A message that could not go out.
};

/// The newest messages that arrived with the chat closed, for the notice over the game.
struct ChatNoticeView {
    std::span<const ChatMenuLine> lines;
    std::size_t more{}; ///< Unread besides those shown.
};

/// What the screen says about the paused game, beside the menu.
struct GameMenuScene {
    std::wstring_view title;
    std::wstring_view details; ///< Developer and version, as the game's control data has them.
    std::wstring_view session; ///< How long this session has run, "1:42:18".
    /// The online room the console is in; empty outside one. `online_dropped`: the room let go of
    /// the console during this game.
    std::wstring_view online;
    bool online_dropped{};
    /// The game keeps running behind the menu (in an online room, where a pause would freeze the
    /// player for everyone): the menu takes the screen's right side only, and the rest shows the
    /// live game, neither blurred nor dimmed.
    bool live{};
};

class GameMenuSurface {
public:
    /// Cheats in view at once on the menu's cheats page.
    static constexpr std::size_t CheatRows = 5;
    /// Files in view at once on the menu's amiibo page, under its "New figure" card.
    static constexpr std::size_t AmiiboRows = 4;
    static constexpr float AmiiboCardHeight = 104;

    GameMenuSurface();
    ~GameMenuSurface();
    GameMenuSurface(const GameMenuSurface&) = delete;
    GameMenuSurface& operator=(const GameMenuSurface&) = delete;

    /// Physical face labels and colors, from the same selected pad as input.
    void SetPromptMapping(u8 mapping);
    /// The paused frame (D3D12::CaptureGameFrame) behind the menu, blurred; empty: plain.
    void SetBackdrop(const D3D12::MenuImage& frame);

    /// The menu, for a screen of `screen` pixels.
    D3D12::MenuImage DrawMenu(const GameMenu& menu, const GameMenuScene& scene, std::pair<u32, u32> screen);
    /// The cheats page.
    D3D12::MenuImage DrawCheats(const Cheats::CheatsView& view, const GameMenuScene& scene,
                                std::pair<u32, u32> screen);
    /// The amiibo page.
    D3D12::MenuImage DrawAmiibo(const AmiiboMenuView& view, const GameMenuScene& scene, std::pair<u32, u32> screen);
    /// Memory Edit's page.
    D3D12::MenuImage DrawMemoryEdit(const MemoryEdit::MenuView& view, const GameMenuScene& scene,
                                    std::pair<u32, u32> screen);
    /// The room chat's page.
    D3D12::MenuImage DrawChat(const ChatMenuView& view, const GameMenuScene& scene, std::pair<u32, u32> screen);
    /// A small card for the screen's top right corner with the newest messages; placed.
    D3D12::MenuImage DrawChatNotice(const ChatNoticeView& view, std::pair<u32, u32> screen);
    /// The keyboard a game opened (game_keyboard.h); B's prompt only when the game lets it cancel.
    D3D12::MenuImage DrawKeyboard(const TextEntryPanelView& view, bool can_cancel, std::pair<u32, u32> screen);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Jadyx
