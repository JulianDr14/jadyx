// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings > Graphics and a game's own Graphics pane: the same rows, plus the ones only the
// game's pane has, what each shows and how input changes them. The library routes input here, saves and applies on a change; the
// canvas draws from the same state.

#include <cstddef>
#include <cstdint>
#include <string>

#include "jadyx/i18n/localization.h"
#include "jadyx/settings/graphics_settings.h"

namespace Jadyx {

enum class GraphicsRow : std::size_t {
    Upscaling, FrameGen, Shaders, GpuFence, AsyncRecord, GuestClock,
    Anisotropy, TextureQuality, TextureMemory, GpuAccuracy, ShaderWorkers, ConsoleMode, ResetAdvanced,
    Count,
};
inline constexpr std::size_t GraphicsRowCount = static_cast<std::size_t>(GraphicsRow::GuestClock);
inline constexpr std::size_t GameGraphicsRowCount = static_cast<std::size_t>(GraphicsRow::Count);

inline constexpr bool IsAdvancedRow(GraphicsRow row) {
    return row >= GraphicsRow::Anisotropy && row < GraphicsRow::Count;
}

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
    case GraphicsRow::GpuFence:
        settings.gpu_fence = StepAdvanced(settings.gpu_fence, SelectableGpuFences(), back);
        return true;
    case GraphicsRow::AsyncRecord:
        settings.async_record = !settings.async_record;
        return true;
    default:
        break;
    }
    return false;
}

/// Applies one input to a game's pane (Back is the caller's); true when the game's overrides
/// changed.
inline bool HandleGameGraphicsInput(GameGraphics& game, std::size_t& selected, GraphicsInput input) {
    if (GraphicsPageDetail::Move(selected, GameGraphicsRowCount, input) || input == GraphicsInput::Back) return false;
    const bool back = input == GraphicsInput::Left;
    auto& advanced = game.advanced;
    switch (static_cast<GraphicsRow>(selected)) {
    case GraphicsRow::Upscaling: StepGameUpscaling(game, back); return true;
    case GraphicsRow::FrameGen: StepGameFrameGen(game, back); return true;
    case GraphicsRow::Shaders: StepGameShaders(game, back); return true;
    case GraphicsRow::GpuFence: StepGameGpuFence(game, back); return true;
    case GraphicsRow::AsyncRecord: StepGameAsyncRecord(game, back); return true;
    case GraphicsRow::GuestClock: game.slow_guest_clock = !game.slow_guest_clock; return true;
    case GraphicsRow::Anisotropy: advanced.anisotropy = StepAdvanced(advanced.anisotropy, SelectableAnisotropy(), back); return true;
    case GraphicsRow::TextureQuality: advanced.full_textures = !advanced.full_textures; return true;
    case GraphicsRow::TextureMemory: advanced.texture_cap_mib = StepAdvanced(advanced.texture_cap_mib, SelectableTextureCaps(), back); return true;
    case GraphicsRow::GpuAccuracy: advanced.gpu_accuracy = StepAdvanced(advanced.gpu_accuracy, SelectableGpuAccuracy(), back); return true;
    case GraphicsRow::ShaderWorkers: advanced.shader_workers = StepAdvanced(advanced.shader_workers, SelectableShaderWorkers(), back); return true;
    case GraphicsRow::ConsoleMode: advanced.console_mode = StepAdvanced(advanced.console_mode, SelectableConsoleModes(), back); return true;
    case GraphicsRow::ResetAdvanced:
        // Only A: moving along the row must not wipe the choices.
        if (input != GraphicsInput::Confirm || advanced == GameAdvancedGraphics{}) return false;
        advanced = {};
        return true;
    case GraphicsRow::Count: break;
    }
    return false;
}

inline I18n::Text GraphicsRowTitle(GraphicsRow row) {
    using I18n::Text;
    switch (row) {
    case GraphicsRow::FrameGen: return Text::FrameGen;
    case GraphicsRow::Shaders: return Text::ShaderBuild;
    case GraphicsRow::GpuFence: return Text::GpuFence;
    case GraphicsRow::AsyncRecord: return Text::AsyncRecord;
    case GraphicsRow::GuestClock: return Text::SlowGuestClock;
    case GraphicsRow::Anisotropy: return Text::AdvancedAnisotropy;
    case GraphicsRow::TextureQuality: return Text::AdvancedTextureQuality;
    case GraphicsRow::TextureMemory: return Text::AdvancedTextureMemory;
    case GraphicsRow::GpuAccuracy: return Text::AdvancedGpuAccuracy;
    case GraphicsRow::ShaderWorkers: return Text::AdvancedShaderWorkers;
    case GraphicsRow::ConsoleMode: return Text::AdvancedConsoleMode;
    case GraphicsRow::ResetAdvanced: return Text::AdvancedReset;
    default: return Text::Upscaling;
    }
}

inline I18n::Text GraphicsRowHint(GraphicsRow row) {
    using I18n::Text;
    switch (row) {
    case GraphicsRow::FrameGen: return Text::FrameGenHint;
    case GraphicsRow::Shaders: return Text::ShaderBuildHint;
    case GraphicsRow::GpuFence: return Text::GpuFenceHint;
    case GraphicsRow::AsyncRecord: return Text::AsyncRecordHint;
    case GraphicsRow::GuestClock: return Text::SlowGuestClockHint;
    case GraphicsRow::Anisotropy: return Text::AdvancedAnisotropyHint;
    case GraphicsRow::TextureQuality: return Text::AdvancedTextureQualityHint;
    case GraphicsRow::TextureMemory: return Text::AdvancedTextureMemoryHint;
    case GraphicsRow::GpuAccuracy: return Text::AdvancedGpuAccuracyHint;
    case GraphicsRow::ShaderWorkers: return Text::AdvancedShaderWorkersHint;
    case GraphicsRow::ConsoleMode: return Text::AdvancedConsoleModeHint;
    case GraphicsRow::ResetAdvanced: return Text::AdvancedResetHint;
    default: return Text::UpscalingHint;
    }
}

/// A row's value for these settings.
inline std::wstring GraphicsRowValue(const GraphicsSettings& settings, GraphicsRow row) {
    switch (row) {
    case GraphicsRow::FrameGen: return OnOffText(settings.frame_gen);
    case GraphicsRow::Shaders: return std::wstring{I18n::Tr(ShaderBuildText(settings.shaders))};
    case GraphicsRow::GpuFence: return std::wstring{I18n::Tr(GpuFenceText(settings.gpu_fence))};
    case GraphicsRow::AsyncRecord: return OnOffText(settings.async_record);
    default: return std::wstring{I18n::Tr(UpscalingText(settings.upscaling))};
    }
}

/// An advanced row's value. The reset row has none.
inline std::wstring AdvancedRowValue(const GameAdvancedGraphics& advanced, GraphicsRow row) {
    using I18n::Text;
    const auto text = [](Text id) { return std::wstring{I18n::Tr(id)}; };
    const auto number = [&](std::uint32_t value, const wchar_t* unit) {
        return value == 0 ? text(Text::Automatic) : std::to_wstring(value) + unit;
    };
    switch (row) {
    case GraphicsRow::Anisotropy:
        switch (advanced.anisotropy) {
        case Settings::AnisotropyMode::X2: return L"2x";
        case Settings::AnisotropyMode::X4: return L"4x";
        case Settings::AnisotropyMode::X8: return L"8x";
        case Settings::AnisotropyMode::X16: return L"16x";
        default: return text(Text::AdvancedAnisotropyGame);
        }
    case GraphicsRow::TextureQuality:
        return text(advanced.full_textures ? Text::AdvancedTextureFull : Text::AdvancedTextureSaver);
    case GraphicsRow::TextureMemory: return number(advanced.texture_cap_mib, L" MiB");
    case GraphicsRow::GpuAccuracy:
        return text(advanced.gpu_accuracy == Settings::GpuAccuracy::Low ? Text::AdvancedGpuLow : Text::AdvancedGpuHigh);
    case GraphicsRow::ShaderWorkers: return number(advanced.shader_workers, L"");
    case GraphicsRow::ConsoleMode:
        return text(advanced.console_mode == Settings::ConsoleMode::Handheld ? Text::AdvancedHandheld : Text::AdvancedDocked);
    default: return {};
    }
}

/// Whether a game has its own value for one of the rows Settings > Graphics also has.
inline bool HasGameOverride(const GameGraphics& game, GraphicsRow row) {
    switch (row) {
    case GraphicsRow::FrameGen: return game.frame_gen.has_value();
    case GraphicsRow::Shaders: return game.shaders.has_value();
    case GraphicsRow::GpuFence: return game.gpu_fence.has_value();
    case GraphicsRow::AsyncRecord: return game.async_record.has_value();
    default: return game.upscaling.has_value();
    }
}

/// A game's row: its own value, or "Settings > Graphics · <what the game gets>". The value is
/// what the game runs with, so FSR turned off by the game's frame generation reads as such.
inline std::wstring GameGraphicsRowValue(const GraphicsSettings& global, const GameGraphics& game,
                                         GraphicsRow row) {
    if (row == GraphicsRow::GuestClock) return OnOffText(game.slow_guest_clock);
    if (IsAdvancedRow(row)) return AdvancedRowValue(game.advanced, row);
    const GraphicsSettings resolved = ResolveGraphics(global, game);
    const auto value = GraphicsRowValue(resolved, row);
    return HasGameOverride(game, row) ? value : I18n::Format(I18n::Text::GraphicsFollowGlobal, {value});
}

} // namespace Jadyx
