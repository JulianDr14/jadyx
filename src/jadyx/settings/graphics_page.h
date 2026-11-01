// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings > Graphics and a game's own Graphics pane: the same three rows, plus one only the game's
// pane has, what each shows and how input changes them. The library routes input here, saves and applies on a change; the
// canvas draws from the same state.

#include <cstddef>
#include <cstdint>
#include <string>

#include "jadyx/i18n/localization.h"
#include "jadyx/settings/graphics_settings.h"

namespace Jadyx {

enum class GraphicsRow : std::size_t { Upscaling, FrameGen, Shaders, GuestClock, Count };
inline constexpr std::size_t GraphicsRowCount = static_cast<std::size_t>(GraphicsRow::GuestClock);
inline constexpr std::size_t GameGraphicsRowCount = static_cast<std::size_t>(GraphicsRow::Count);

struct GraphicsPage {
    bool open{};
    std::size_t selected{};
    GraphicsSettings settings; ///< As saved.
};

enum class GraphicsInput : std::uint8_t { Up, Down, Left, Right, Confirm, Back };

namespace GraphicsPageDetail {
/// Up and Down move the cursor; true when the input was one of them.
inline bool Move(std::size_t& selected, std::size_t count, GraphicsInput input) {
    if (input == GraphicsInput::Up) selected = (selected + count - 1) % count;
    else if (input == GraphicsInput::Down) selected = (selected + 1) % count;
    else return false;
    return true;
}
} // namespace GraphicsPageDetail

/// Applies one input to Settings > Graphics; true when the settings changed (save and apply).
inline bool HandleGraphicsInput(GraphicsPage& page, GraphicsInput input) {
    if (GraphicsPageDetail::Move(page.selected, GraphicsRowCount, input)) return false;
    if (input == GraphicsInput::Back) {
        page.open = false;
        return false;
    }
    const bool back = input == GraphicsInput::Left;
    auto& settings = page.settings;
    switch (static_cast<GraphicsRow>(page.selected)) {
    case GraphicsRow::Upscaling:
        SetUpscaling(settings, StepUpscaling(settings.upscaling, back));
        return true;
    case GraphicsRow::FrameGen:
        SetFrameGen(settings, !settings.frame_gen);
        return true;
    case GraphicsRow::Shaders:
        settings.shaders = StepShaderBuild(settings.shaders, back);
        return true;
    case GraphicsRow::GuestClock:
    case GraphicsRow::Count:
        break;
    }
    return false;
}

/// Applies one input to a game's pane (Back is the caller's); true when the game's overrides
/// changed.
inline bool HandleGameGraphicsInput(GameGraphics& game, std::size_t& selected, GraphicsInput input) {
    if (GraphicsPageDetail::Move(selected, GameGraphicsRowCount, input) || input == GraphicsInput::Back) return false;
    const bool back = input == GraphicsInput::Left;
    switch (static_cast<GraphicsRow>(selected)) {
    case GraphicsRow::Upscaling: StepGameUpscaling(game, back); return true;
    case GraphicsRow::FrameGen: StepGameFrameGen(game, back); return true;
    case GraphicsRow::Shaders: StepGameShaders(game, back); return true;
    case GraphicsRow::GuestClock: game.slow_guest_clock = !game.slow_guest_clock; return true;
    case GraphicsRow::Count: break;
    }
    return false;
}

inline I18n::Text GraphicsRowTitle(GraphicsRow row) {
    using I18n::Text;
    switch (row) {
    case GraphicsRow::FrameGen: return Text::FrameGen;
    case GraphicsRow::Shaders: return Text::ShaderBuild;
    case GraphicsRow::GuestClock: return Text::SlowGuestClock;
    default: return Text::Upscaling;
    }
}

inline I18n::Text GraphicsRowHint(GraphicsRow row) {
    using I18n::Text;
    switch (row) {
    case GraphicsRow::FrameGen: return Text::FrameGenHint;
    case GraphicsRow::Shaders: return Text::ShaderBuildHint;
    case GraphicsRow::GuestClock: return Text::SlowGuestClockHint;
    default: return Text::UpscalingHint;
    }
}

/// A row's value for these settings.
inline std::wstring GraphicsRowValue(const GraphicsSettings& settings, GraphicsRow row) {
    switch (row) {
    case GraphicsRow::FrameGen: return OnOffText(settings.frame_gen);
    case GraphicsRow::Shaders: return std::wstring{I18n::Tr(ShaderBuildText(settings.shaders))};
    default: return std::wstring{I18n::Tr(UpscalingText(settings.upscaling))};
    }
}

/// A game's row: its own value, or "Settings > Graphics · <what the game gets>". The value is
/// what the game runs with, so FSR turned off by the game's frame generation reads as such.
inline std::wstring GameGraphicsRowValue(const GraphicsSettings& global, const GameGraphics& game,
                                         GraphicsRow row) {
    if (row == GraphicsRow::GuestClock) return OnOffText(game.slow_guest_clock);
    const GraphicsSettings resolved = ResolveGraphics(global, game);
    const bool own = row == GraphicsRow::Upscaling ? game.upscaling.has_value()
                     : row == GraphicsRow::FrameGen ? game.frame_gen.has_value()
                                                    : game.shaders.has_value();
    const auto value = GraphicsRowValue(resolved, row);
    return own ? value : I18n::Format(I18n::Text::GraphicsFollowGlobal, {value});
}

} // namespace Jadyx
