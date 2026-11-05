// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings > Graphics: how the image reaches the screen, what a draw does while its shader builds
// and when the guest learns that the GPU finished. FSR and frame generation exclude each other:
// turning one on turns the other off. Each game may override any of them (nullopt follows
// Settings > Graphics). Pure logic only:
// persistence and applying them are in uwp_system_settings.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "jadyx/i18n/localization.h"
#include "jadyx/settings/system_settings.h"
#include "jadyx/settings/upscaling.h"

namespace Jadyx {

/// What a draw does while its shader (its pipeline) builds on a worker: is skipped, so the game
/// never stops ("async_pipelines=full"); is skipped unless it is a full-screen pass or a target's
/// first draw ("balanced"); or waits, so nothing is missing but the game stutters ("off").
enum class ShaderBuild : std::uint8_t { Skip, Balanced, Wait };

inline std::span<const ShaderBuild> SelectableShaderBuilds() {
    static constexpr std::array values{ShaderBuild::Skip, ShaderBuild::Balanced, ShaderBuild::Wait};
    return values;
}

inline ShaderBuild StepShaderBuild(ShaderBuild current, bool back) {
    const auto values = SelectableShaderBuilds();
    const std::size_t count = values.size();
    std::size_t index = 0;
    while (index < count && values[index] != current) ++index;
    if (index == count) return values.front();
    return values[(index + (back ? count - 1 : 1)) % count];
}

inline std::optional<ShaderBuild> ShaderBuildFromValue(std::int32_t value) {
    for (const ShaderBuild build : SelectableShaderBuilds())
        if (static_cast<std::int32_t>(build) == value) return build;
    return std::nullopt;
}

inline I18n::Text ShaderBuildText(ShaderBuild build) {
    using I18n::Text;
    switch (build) {
    case ShaderBuild::Balanced: return Text::ShaderBuildBalanced;
    case ShaderBuild::Wait: return Text::ShaderBuildWait;
    case ShaderBuild::Skip: break;
    }
    return Text::ShaderBuildSkip;
}

/// When the guest learns from a syncpoint that the GPU finished. Immediate: as soon as the GPU thread
/// reaches it, so what the GPU wrote may land after the guest goes on (the fastest). Safe: the same
/// unless a download or operation queued before it waits on the GPU. Wait: once the host GPU ran the
/// work before it. Balanced and Accurate: as Wait, and they also wait for the host GPU on buffer
/// copies, and Accurate on buffer reads. With a GPU accuracy of Low, Safe and Wait act as Immediate.
enum class GpuFence : std::uint8_t { Immediate, Safe, Wait, Balanced, Accurate };

inline std::span<const GpuFence> SelectableGpuFences() {
    static constexpr std::array values{GpuFence::Immediate, GpuFence::Safe, GpuFence::Wait,
                                       GpuFence::Balanced, GpuFence::Accurate};
    return values;
}

inline I18n::Text GpuFenceText(GpuFence fence) {
    using I18n::Text;
    switch (fence) {
    case GpuFence::Safe: return Text::GpuFenceSafe;
    case GpuFence::Wait: return Text::GpuFenceWait;
    case GpuFence::Balanced: return Text::GpuFenceBalanced;
    case GpuFence::Accurate: return Text::GpuFenceAccurate;
    case GpuFence::Immediate: break;
    }
    return Text::GpuFenceImmediate;
}

struct GraphicsSettings {
    Upscaling upscaling{Upscaling::FsrBalanced}; ///< FSR up to the screen (4K) by default.
    bool frame_gen{};                            ///< Off by default; on is the automatic mode.
    ShaderBuild shaders{ShaderBuild::Balanced}; ///< The only mode with the frame's wait budget.
    GpuFence gpu_fence{GpuFence::Immediate};
    bool async_record{true}; ///< D3D12 command list calls on a thread of their own.

    bool operator==(const GraphicsSettings&) const = default;
};

/// A game's advanced options, only its own. Each default is what the app does with no choice made,
/// so {} is "reset". The session reads them before it starts (boot_session.cpp); texture memory, GPU
/// accuracy and the console's mode can also change from the in-game menu.
struct GameAdvancedGraphics {
    Settings::AnisotropyMode anisotropy{Settings::AnisotropyMode::Default}; ///< Default: as the game asks.
    bool full_textures{};            ///< Big ASTC textures keep their top level: four times their memory.
    std::uint32_t texture_cap_mib{}; ///< 0: the app's own.
    Settings::GpuAccuracy gpu_accuracy{Settings::GpuAccuracy::High};
    std::uint32_t shader_workers{}; ///< 0: the app's own.
    Settings::ConsoleMode console_mode{Settings::ConsoleMode::Docked};

    bool operator==(const GameAdvancedGraphics&) const = default;
};

inline std::span<const Settings::AnisotropyMode> SelectableAnisotropy() {
    using Settings::AnisotropyMode;
    static constexpr std::array values{AnisotropyMode::Default, AnisotropyMode::X2, AnisotropyMode::X4,
                                       AnisotropyMode::X8, AnisotropyMode::X16};
    return values;
}

inline std::span<const std::uint32_t> SelectableTextureCaps() {
    static constexpr std::array<std::uint32_t, 4> values{0, 512, 640, 1024};
    return values;
}

inline std::span<const Settings::GpuAccuracy> SelectableGpuAccuracy() {
    static constexpr std::array values{Settings::GpuAccuracy::High, Settings::GpuAccuracy::Low};
    return values;
}

inline std::span<const std::uint32_t> SelectableShaderWorkers() {
    static constexpr std::array<std::uint32_t, 6> values{0, 1, 2, 4, 6, 8};
    return values;
}

inline std::span<const Settings::ConsoleMode> SelectableConsoleModes() {
    static constexpr std::array values{Settings::ConsoleMode::Docked, Settings::ConsoleMode::Handheld};
    return values;
}

/// The value after `current`, wrapping; a value not offered goes to the first.
template <typename T>
T StepAdvanced(T current, std::span<const T> values, bool back) {
    const std::size_t count = values.size();
    std::size_t index = 0;
    while (index < count && values[index] != current) ++index;
    if (index == count) return values.front();
    return values[(index + (back ? count - 1 : 1)) % count];
}

/// One game's overrides; nullopt follows Settings > Graphics.
struct GameGraphics {
    std::optional<Upscaling> upscaling;
    std::optional<bool> frame_gen;
    std::optional<ShaderBuild> shaders;
    std::optional<GpuFence> gpu_fence;
    std::optional<bool> async_record;
    bool slow_guest_clock{}; ///< Only a game's own: there is no global setting to follow.
    GameAdvancedGraphics advanced;

    bool operator==(const GameGraphics&) const = default;
};

/// What the guest's clock runs at for a game with slow_guest_clock on.
inline constexpr std::uint32_t SlowGuestClockPercent = 90;

inline bool IsFsr(Upscaling upscaling) {
    return upscaling != Upscaling::Bilinear;
}

/// FSR on turns frame generation off.
inline void SetUpscaling(GraphicsSettings& settings, Upscaling upscaling) {
    settings.upscaling = upscaling;
    if (IsFsr(upscaling)) settings.frame_gen = false;
}

/// Frame generation on turns FSR off (the bilinear blit).
inline void SetFrameGen(GraphicsSettings& settings, bool on) {
    settings.frame_gen = on;
    if (on && IsFsr(settings.upscaling)) settings.upscaling = Upscaling::Bilinear;
}

/// What a game runs with. When the game asks for one of FSR and frame generation and Settings >
/// Graphics for the other, the game's own choice wins.
inline GraphicsSettings ResolveGraphics(const GraphicsSettings& global, const GameGraphics& game) {
    GraphicsSettings resolved{
        .upscaling = game.upscaling.value_or(global.upscaling),
        .frame_gen = game.frame_gen.value_or(global.frame_gen),
        .shaders = game.shaders.value_or(global.shaders),
        .gpu_fence = game.gpu_fence.value_or(global.gpu_fence),
        .async_record = game.async_record.value_or(global.async_record),
    };
    if (resolved.frame_gen && IsFsr(resolved.upscaling)) {
        if (game.frame_gen && !game.upscaling) resolved.upscaling = Upscaling::Bilinear;
        else resolved.frame_gen = false;
    }
    return resolved;
}

/// Whether a game presents at the TV's resolution (4K on a 4K TV): FSR enlarges its image up to
/// it. Frame generation and the bilinear blit present at 1080p, where generating frames is cheap.
inline bool UsesScreenResolution(const GraphicsSettings& settings) {
    return IsFsr(settings.upscaling) && !settings.frame_gen;
}

/// Steps a game's upscaling through "as Settings > Graphics" and every choice. FSR chosen for
/// the game turns its frame generation off.
inline void StepGameUpscaling(GameGraphics& game, bool back) {
    game.upscaling = StepChoice(game.upscaling, SelectableUpscalings(), back);
    if (game.upscaling && IsFsr(*game.upscaling) && game.frame_gen == true) game.frame_gen = false;
}

/// Steps a game's frame generation through "as Settings > Graphics", on and off. On for the game
/// turns the game's own FSR off.
inline void StepGameFrameGen(GameGraphics& game, bool back) {
    static constexpr std::array values{true, false};
    game.frame_gen = StepChoice<bool>(game.frame_gen, values, back);
    if (game.frame_gen == true && game.upscaling && IsFsr(*game.upscaling))
        game.upscaling = Upscaling::Bilinear;
}

inline void StepGameShaders(GameGraphics& game, bool back) {
    game.shaders = StepChoice(game.shaders, SelectableShaderBuilds(), back);
}

inline void StepGameGpuFence(GameGraphics& game, bool back) {
    game.gpu_fence = StepChoice(game.gpu_fence, SelectableGpuFences(), back);
}

inline void StepGameAsyncRecord(GameGraphics& game, bool back) {
    static constexpr std::array values{true, false};
    game.async_record = StepChoice<bool>(game.async_record, values, back);
}

inline std::wstring OnOffText(bool on) {
    return std::wstring{I18n::Tr(on ? I18n::Text::Yes : I18n::Text::No)};
}

} // namespace Jadyx
