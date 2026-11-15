// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings > Graphics and a game's own Graphics pane: the same rows, plus the ones only the
// game's pane has, what each shows and how input changes them. The rows are grouped in sections
// by what they affect (image, performance, fixes, memory): each page lists the sections and opens
// one at a time. The library routes input here, saves and applies on a change; the canvas draws
// from the same state.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "jadyx/i18n/localization.h"
#include "jadyx/settings/graphics_settings.h"

namespace Jadyx {

enum class GraphicsRow : std::size_t {
    Upscaling, FrameGen, Shaders, GpuFence, AsyncRecord, CpuWriteQueue, GpuFastPaths, FixedCoreMap, AstcArrayCap, GuestClock, ExactGpuReads,
    Anisotropy, TextureQuality, TextureMemory, GpuAccuracy, ShaderWorkers, ConsoleMode, ResetAdvanced,
    Count,
};
inline constexpr std::size_t GraphicsRowCount = static_cast<std::size_t>(GraphicsRow::GuestClock);
inline constexpr std::size_t GameGraphicsRowCount = static_cast<std::size_t>(GraphicsRow::Count);

inline constexpr bool IsAdvancedRow(GraphicsRow row) {
    return row >= GraphicsRow::Anisotropy && row < GraphicsRow::Count;
}

/// Settings > Graphics has the rows below GraphicsRowCount; a game's pane has them all.
inline constexpr bool IsGlobalRow(GraphicsRow row) {
    return static_cast<std::size_t>(row) < GraphicsRowCount;
}

/// What the rows affect: each page lists these and opens one at a time.
enum class GraphicsSection : std::uint8_t { Image, Performance, Fixes, Memory };
inline constexpr std::size_t GraphicsSectionCount = 4;

/// A section's rows in the order shown, the game-only ones included.
inline std::span<const GraphicsRow> GraphicsSectionAllRows(GraphicsSection section) {
    using R = GraphicsRow;
    static constexpr R image[]{R::Upscaling, R::FrameGen, R::Anisotropy, R::ConsoleMode};
    static constexpr R performance[]{R::Shaders,      R::ShaderWorkers, R::GuestClock,  R::AsyncRecord,
                                     R::GpuFastPaths, R::CpuWriteQueue, R::FixedCoreMap};
    static constexpr R fixes[]{R::GpuFence, R::GpuAccuracy, R::ExactGpuReads, R::AstcArrayCap};
    static constexpr R memory[]{R::TextureQuality, R::TextureMemory};
    switch (section) {
    case GraphicsSection::Performance: return performance;
    case GraphicsSection::Fixes: return fixes;
    case GraphicsSection::Memory: return memory;
    case GraphicsSection::Image: break;
    }
    return image;
}

/// The rows a page shows in a section: Settings > Graphics only the ones it has.
inline std::vector<GraphicsRow> GraphicsSectionRows(GraphicsSection section, bool game) {
    std::vector<GraphicsRow> rows;
    for (const GraphicsRow row : GraphicsSectionAllRows(section))
        if (game || IsGlobalRow(row)) rows.push_back(row);
    return rows;
}

/// The sections a page lists: Settings > Graphics leaves out the ones with none of its rows.
inline std::vector<GraphicsSection> GraphicsSections(bool game) {
    std::vector<GraphicsSection> sections;
    for (std::size_t i = 0; i < GraphicsSectionCount; ++i) {
        const auto section = static_cast<GraphicsSection>(i);
        if (!GraphicsSectionRows(section, game).empty()) sections.push_back(section);
    }
    return sections;
}

/// The section a row is in.
inline GraphicsSection GraphicsSectionOf(GraphicsRow row) {
    for (std::size_t i = 0; i < GraphicsSectionCount; ++i) {
        const auto section = static_cast<GraphicsSection>(i);
        const auto rows = GraphicsSectionAllRows(section);
        if (std::find(rows.begin(), rows.end(), row) != rows.end()) return section;
    }
    return GraphicsSection::Image;
}

/// Where a page's cursor is: the list of sections (in a game's pane, with the reset row after
/// them) or one section's rows.
struct GraphicsCursor {
    std::optional<GraphicsSection> section; ///< None: the list of sections.
    std::size_t selected{};
    bool operator==(const GraphicsCursor&) const = default;
};

/// How many rows the cursor's list has.
inline std::size_t GraphicsListSize(const GraphicsCursor& cursor, bool game) {
    if (cursor.section) return GraphicsSectionRows(*cursor.section, game).size();
    return GraphicsSections(game).size() + (game ? 1 : 0);
}

/// The setting under the cursor; none on a section's entry.
inline std::optional<GraphicsRow> GraphicsCursorRow(const GraphicsCursor& cursor, bool game) {
    if (cursor.section) {
        const auto rows = GraphicsSectionRows(*cursor.section, game);
        return cursor.selected < rows.size() ? std::optional{rows[cursor.selected]} : std::nullopt;
    }
    if (game && cursor.selected == GraphicsSections(game).size()) return GraphicsRow::ResetAdvanced;
    return std::nullopt;
}

struct GraphicsPage {
    bool open{};
    GraphicsCursor cursor;
    GraphicsSettings settings; ///< As saved.
};

enum class GraphicsInput : std::uint8_t { Up, Down, Left, Right, Confirm, Back };

namespace GraphicsPageDetail {
/// Up and Down move the cursor; true when the input was one of them.
inline bool Move(std::size_t& selected, std::size_t count, GraphicsInput input) {
    if (count == 0) return false;
    if (input == GraphicsInput::Up) selected = (selected + count - 1) % count;
    else if (input == GraphicsInput::Down) selected = (selected + 1) % count;
    else return false;
    return true;
}

enum class Step : std::uint8_t { None, Change, Leave };
/// Moves the cursor, opens a section (A or Right on its entry) and closes it (B, back on its
/// entry). Change: the input is for the setting under the cursor. Leave: B on the list of sections.
inline Step Navigate(GraphicsCursor& cursor, bool game, GraphicsInput input) {
    const std::size_t count = GraphicsListSize(cursor, game);
    if (cursor.selected >= count) cursor.selected = 0;
    if (Move(cursor.selected, count, input)) return Step::None;
    const auto sections = GraphicsSections(game);
    if (input == GraphicsInput::Back) {
        if (!cursor.section) return Step::Leave;
        cursor.selected = static_cast<std::size_t>(
            std::find(sections.begin(), sections.end(), *cursor.section) - sections.begin());
        cursor.section.reset();
        return Step::None;
    }
    if (!cursor.section && cursor.selected < sections.size()) {
        if (input == GraphicsInput::Confirm || input == GraphicsInput::Right) {
            cursor.section = sections[cursor.selected];
            cursor.selected = 0;
        }
        return Step::None;
    }
    return Step::Change;
}
} // namespace GraphicsPageDetail

/// Applies Left, Right or A to one of Settings > Graphics' rows; true when the settings changed.
inline bool ChangeGraphicsRow(GraphicsSettings& settings, GraphicsRow row, GraphicsInput input) {
    if (input != GraphicsInput::Left && input != GraphicsInput::Right && input != GraphicsInput::Confirm) return false;
    const bool back = input == GraphicsInput::Left;
    switch (row) {
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
    case GraphicsRow::CpuWriteQueue:
        settings.cpu_write_queue = !settings.cpu_write_queue;
        return true;
    case GraphicsRow::GpuFastPaths:
        settings.gpu_fast_paths = !settings.gpu_fast_paths;
        return true;
    case GraphicsRow::FixedCoreMap:
        settings.fixed_core_map = !settings.fixed_core_map;
        return true;
    case GraphicsRow::AstcArrayCap:
        settings.astc_array_cap_mib = StepAdvanced(settings.astc_array_cap_mib, SelectableAstcArrayCaps(), back);
        return true;
    default:
        break;
    }
    return false;
}

/// Applies one input to Settings > Graphics; true when the settings changed (save and apply).
inline bool HandleGraphicsInput(GraphicsPage& page, GraphicsInput input) {
    switch (GraphicsPageDetail::Navigate(page.cursor, false, input)) {
    case GraphicsPageDetail::Step::Leave:
        page.open = false;
        return false;
    case GraphicsPageDetail::Step::Change: {
        const auto row = GraphicsCursorRow(page.cursor, false);
        return row && ChangeGraphicsRow(page.settings, *row, input);
    }
    case GraphicsPageDetail::Step::None: break;
    }
    return false;
}

/// Applies Left, Right or A to one of a game's rows; true when the game's overrides changed.
inline bool ChangeGameGraphicsRow(GameGraphics& game, GraphicsRow row, GraphicsInput input) {
    if (input != GraphicsInput::Left && input != GraphicsInput::Right && input != GraphicsInput::Confirm) return false;
    const bool back = input == GraphicsInput::Left;
    auto& advanced = game.advanced;
    switch (row) {
    case GraphicsRow::Upscaling: StepGameUpscaling(game, back); return true;
    case GraphicsRow::FrameGen: StepGameFrameGen(game, back); return true;
    case GraphicsRow::Shaders: StepGameShaders(game, back); return true;
    case GraphicsRow::GpuFence: StepGameGpuFence(game, back); return true;
    case GraphicsRow::AsyncRecord: StepGameAsyncRecord(game, back); return true;
    case GraphicsRow::CpuWriteQueue: StepGameCpuWriteQueue(game, back); return true;
    case GraphicsRow::GpuFastPaths: StepGameGpuFastPaths(game, back); return true;
    case GraphicsRow::FixedCoreMap: StepGameFixedCoreMap(game, back); return true;
    case GraphicsRow::AstcArrayCap: StepGameAstcArrayCap(game, back); return true;
    case GraphicsRow::GuestClock: game.slow_guest_clock = !game.slow_guest_clock; return true;
    case GraphicsRow::ExactGpuReads: game.exact_gpu_reads = !game.exact_gpu_reads; return true;
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

/// Applies one input to a game's pane; true when the game's overrides changed. B in a section goes
/// back to the list of sections; B on the list is the caller's (it closes the pane).
inline bool HandleGameGraphicsInput(GameGraphics& game, GraphicsCursor& cursor, GraphicsInput input) {
    if (GraphicsPageDetail::Navigate(cursor, true, input) != GraphicsPageDetail::Step::Change) return false;
    const auto row = GraphicsCursorRow(cursor, true);
    return row && ChangeGameGraphicsRow(game, *row, input);
}

/// Settings > Graphics back to the emulator's defaults (Y on the page); false when they already are.
inline bool ResetGraphicsSettings(GraphicsSettings& settings) {
    if (settings == GraphicsSettings{}) return false;
    settings = {};
    return true;
}

/// A game back to following Settings > Graphics (Y in its pane and its in-game menu): its own
/// values go, and the rows only a game has (guest clock, exact reads, advanced) go back to their
/// defaults, as there is nothing global for them to follow. False when nothing was its own.
inline bool SyncGameGraphics(GameGraphics& game) {
    if (game == GameGraphics{}) return false;
    game = {};
    return true;
}

inline I18n::Text GraphicsSectionTitle(GraphicsSection section) {
    using I18n::Text;
    switch (section) {
    case GraphicsSection::Performance: return Text::GraphicsSectionPerformance;
    case GraphicsSection::Fixes: return Text::GraphicsSectionFixes;
    case GraphicsSection::Memory: return Text::GraphicsSectionMemory;
    case GraphicsSection::Image: break;
    }
    return Text::GraphicsSectionImage;
}

/// What a section's rows affect: under its entry and as its page's subtitle.
inline I18n::Text GraphicsSectionHint(GraphicsSection section) {
    using I18n::Text;
    switch (section) {
    case GraphicsSection::Performance: return Text::GraphicsSectionPerformanceHint;
    case GraphicsSection::Fixes: return Text::GraphicsSectionFixesHint;
    case GraphicsSection::Memory: return Text::GraphicsSectionMemoryHint;
    case GraphicsSection::Image: break;
    }
    return Text::GraphicsSectionImageHint;
}

inline I18n::Text GraphicsRowTitle(GraphicsRow row) {
    using I18n::Text;
    switch (row) {
    case GraphicsRow::FrameGen: return Text::FrameGen;
    case GraphicsRow::Shaders: return Text::ShaderBuild;
    case GraphicsRow::GpuFence: return Text::GpuFence;
    case GraphicsRow::AsyncRecord: return Text::AsyncRecord;
    case GraphicsRow::CpuWriteQueue: return Text::CpuWriteQueue;
    case GraphicsRow::GpuFastPaths: return Text::GpuFastPaths;
    case GraphicsRow::FixedCoreMap: return Text::FixedCoreMap;
    case GraphicsRow::AstcArrayCap: return Text::AstcArrayCap;
    case GraphicsRow::GuestClock: return Text::SlowGuestClock;
    case GraphicsRow::ExactGpuReads: return Text::ExactGpuReads;
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

/// What a row gains and what it costs, in words a player follows. The reset row has none.
struct GraphicsGainCost {
    I18n::Text gain, cost;
};
inline std::optional<GraphicsGainCost> GraphicsRowGainCost(GraphicsRow row) {
    using I18n::Text;
    using G = GraphicsGainCost;
    switch (row) {
    case GraphicsRow::Upscaling: return G{Text::UpscalingGain, Text::UpscalingCost};
    case GraphicsRow::FrameGen: return G{Text::FrameGenGain, Text::FrameGenCost};
    case GraphicsRow::Shaders: return G{Text::ShaderBuildGain, Text::ShaderBuildCost};
    case GraphicsRow::GpuFence: return G{Text::GpuFenceGain, Text::GpuFenceCost};
    case GraphicsRow::AsyncRecord: return G{Text::AsyncRecordGain, Text::AsyncRecordCost};
    case GraphicsRow::CpuWriteQueue: return G{Text::CpuWriteQueueGain, Text::CpuWriteQueueCost};
    case GraphicsRow::GpuFastPaths: return G{Text::GpuFastPathsGain, Text::GpuFastPathsCost};
    case GraphicsRow::FixedCoreMap: return G{Text::FixedCoreMapGain, Text::FixedCoreMapCost};
    case GraphicsRow::AstcArrayCap: return G{Text::AstcArrayCapGain, Text::AstcArrayCapCost};
    case GraphicsRow::GuestClock: return G{Text::SlowGuestClockGain, Text::SlowGuestClockCost};
    case GraphicsRow::ExactGpuReads: return G{Text::ExactGpuReadsGain, Text::ExactGpuReadsCost};
    case GraphicsRow::Anisotropy: return G{Text::AdvancedAnisotropyGain, Text::AdvancedAnisotropyCost};
    case GraphicsRow::TextureQuality: return G{Text::AdvancedTextureQualityGain, Text::AdvancedTextureQualityCost};
    case GraphicsRow::TextureMemory: return G{Text::AdvancedTextureMemoryGain, Text::AdvancedTextureMemoryCost};
    case GraphicsRow::GpuAccuracy: return G{Text::AdvancedGpuAccuracyGain, Text::AdvancedGpuAccuracyCost};
    case GraphicsRow::ShaderWorkers: return G{Text::AdvancedShaderWorkersGain, Text::AdvancedShaderWorkersCost};
    case GraphicsRow::ConsoleMode: return G{Text::AdvancedConsoleModeGain, Text::AdvancedConsoleModeCost};
    case GraphicsRow::ResetAdvanced:
    case GraphicsRow::Count: break;
    }
    return std::nullopt;
}

/// A row's line under its title: "Gain: … · Cost: …", or the reset row's own hint.
inline std::wstring GraphicsRowHint(GraphicsRow row) {
    const auto gain_cost = GraphicsRowGainCost(row);
    if (!gain_cost) return std::wstring{I18n::Tr(I18n::Text::AdvancedResetHint)};
    return I18n::Format(I18n::Text::GraphicsGainCost, {I18n::Tr(gain_cost->gain), I18n::Tr(gain_cost->cost)});
}

/// A row's value for these settings.
inline std::wstring GraphicsRowValue(const GraphicsSettings& settings, GraphicsRow row) {
    switch (row) {
    case GraphicsRow::FrameGen: return OnOffText(settings.frame_gen);
    case GraphicsRow::Shaders: return std::wstring{I18n::Tr(ShaderBuildText(settings.shaders))};
    case GraphicsRow::GpuFence: return std::wstring{I18n::Tr(GpuFenceText(settings.gpu_fence))};
    case GraphicsRow::AsyncRecord: return OnOffText(settings.async_record);
    case GraphicsRow::CpuWriteQueue: return OnOffText(settings.cpu_write_queue);
    case GraphicsRow::GpuFastPaths: return OnOffText(settings.gpu_fast_paths);
    case GraphicsRow::FixedCoreMap: return OnOffText(settings.fixed_core_map);
    case GraphicsRow::AstcArrayCap: return std::to_wstring(settings.astc_array_cap_mib) + L" MiB";
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
    case GraphicsRow::CpuWriteQueue: return game.cpu_write_queue.has_value();
    case GraphicsRow::GpuFastPaths: return game.gpu_fast_paths.has_value();
    case GraphicsRow::FixedCoreMap: return game.fixed_core_map.has_value();
    case GraphicsRow::AstcArrayCap: return game.astc_array_cap_mib.has_value();
    default: return game.upscaling.has_value();
    }
}

/// A game's row: its own value, or "Settings > Graphics · <what the game gets>". The value is
/// what the game runs with, so FSR turned off by the game's frame generation reads as such.
inline std::wstring GameGraphicsRowValue(const GraphicsSettings& global, const GameGraphics& game,
                                         GraphicsRow row) {
    if (row == GraphicsRow::GuestClock) return OnOffText(game.slow_guest_clock);
    if (row == GraphicsRow::ExactGpuReads) return OnOffText(game.exact_gpu_reads);
    if (IsAdvancedRow(row)) return AdvancedRowValue(game.advanced, row);
    const GraphicsSettings resolved = ResolveGraphics(global, game);
    const auto value = GraphicsRowValue(resolved, row);
    return HasGameOverride(game, row) ? value : I18n::Format(I18n::Text::GraphicsFollowGlobal, {value});
}

} // namespace Jadyx
