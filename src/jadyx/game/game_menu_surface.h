// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The in-game menu drawn with real text: Direct2D on a CPU bitmap (WIC), so the game's D3D12
// device is not shared and nothing is resident on the GPU. The menu and its cheats page take the
// whole screen over the paused frame, blurred; the game's keyboard is a panel the renderer copies
// over the running game. A surface lives only while it is open: closed, it costs nothing.

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "jadyx/game/error_display.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace Jadyx {

class GameMenu;
struct WarmupView;
struct TextEntryPanelView;
namespace Cheats {
struct CheatsView;
}

/// What the screen says about the paused game, beside the menu.
struct GameMenuScene {
    std::wstring_view title;
    std::wstring_view details; ///< Developer and version, as the game's control data has them.
    std::wstring_view session; ///< How long this session has run, "1:42:18".
};

class GameMenuSurface {
public:
    /// Cheats in view at once on the menu's cheats page.
    static constexpr std::size_t CheatRows = 5;

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
    D3D12::MenuImage DrawError(const ErrorDisplay::Message& message, std::pair<u32, u32> screen);
    /// The keyboard a game opened (game_keyboard.h); B's prompt only when the game lets it cancel.
    D3D12::MenuImage DrawKeyboard(const TextEntryPanelView& view, bool can_cancel, std::pair<u32, u32> screen);

    /// Startup preparation, using the same canvas, palette and controller prompts.
    D3D12::MenuImage DrawWarmup(const WarmupView& view, std::pair<u32, u32> screen);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Jadyx
