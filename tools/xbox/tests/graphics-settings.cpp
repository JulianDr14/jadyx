// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/i18n/localization.cpp and src/jadyx/i18n/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include "jadyx/settings/graphics_page.h"

int main() {
    using namespace Jadyx;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));

    // Defaults: FSR up to the screen, frame generation off, draws skipped while shaders build.
    const GraphicsSettings defaults;
    check(defaults.upscaling == Upscaling::FsrBalanced && !defaults.frame_gen && defaults.shaders == ShaderBuild::Skip);
    check(defaults.gpu_fence == GpuFence::Immediate);

    // FSR and frame generation exclude each other, either way round.
    GraphicsSettings settings;
    SetFrameGen(settings, true);
    check(settings.frame_gen && settings.upscaling == Upscaling::Bilinear);
    SetUpscaling(settings, Upscaling::FsrSharp);
    check(!settings.frame_gen && settings.upscaling == Upscaling::FsrSharp);
    SetUpscaling(settings, Upscaling::Bilinear);
    SetFrameGen(settings, true);
    SetUpscaling(settings, Upscaling::Bilinear);
    check(settings.frame_gen); // Bilinear keeps it.

    // The page: rows wrap, Left/Right/A change the focused row, B closes.
    GraphicsPage page{true};
    check(!HandleGraphicsInput(page, GraphicsInput::Up) && page.selected == GraphicsRowCount - 1);
    check(!HandleGraphicsInput(page, GraphicsInput::Down) && page.selected == 0);
    check(GraphicsRowValue(page.settings, GraphicsRow::Upscaling) == L"FSR 1 · equilibrado");
    page.selected = static_cast<std::size_t>(GraphicsRow::FrameGen);
    check(HandleGraphicsInput(page, GraphicsInput::Confirm));
    check(page.settings.frame_gen && page.settings.upscaling == Upscaling::Bilinear);
    check(GraphicsRowValue(page.settings, GraphicsRow::FrameGen) == L"Sí");
    page.selected = static_cast<std::size_t>(GraphicsRow::Upscaling);
    check(HandleGraphicsInput(page, GraphicsInput::Right));
    check(page.settings.upscaling == Upscaling::FsrSharp && !page.settings.frame_gen);
    page.selected = static_cast<std::size_t>(GraphicsRow::Shaders);
    check(HandleGraphicsInput(page, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Balanced);
    check(HandleGraphicsInput(page, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Wait);
    check(HandleGraphicsInput(page, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Skip);
    check(HandleGraphicsInput(page, GraphicsInput::Left) && page.settings.shaders == ShaderBuild::Wait);
    check(GraphicsRowValue(page.settings, GraphicsRow::Shaders) == L"Esperar");
    page.selected = static_cast<std::size_t>(GraphicsRow::GpuFence);
    check(GraphicsRowValue(page.settings, GraphicsRow::GpuFence) == L"Inmediato");
    for (const GpuFence fence : {GpuFence::Safe, GpuFence::Wait, GpuFence::Balanced, GpuFence::Accurate, GpuFence::Immediate})
        check(HandleGraphicsInput(page, GraphicsInput::Right) && page.settings.gpu_fence == fence);
    check(HandleGraphicsInput(page, GraphicsInput::Left) && page.settings.gpu_fence == GpuFence::Accurate);
    check(GraphicsRowValue(page.settings, GraphicsRow::GpuFence) == L"Preciso");
    check(HandleGraphicsInput(page, GraphicsInput::Right) && page.settings.gpu_fence == GpuFence::Immediate);
    // The command thread: on by default, and the page's last row.
    check(defaults.async_record && GraphicsRowValue(page.settings, GraphicsRow::AsyncRecord) == L"Sí");
    check(!HandleGraphicsInput(page, GraphicsInput::Down) && page.selected == static_cast<std::size_t>(GraphicsRow::AsyncRecord));
    check(page.selected == GraphicsRowCount - 1);
    check(HandleGraphicsInput(page, GraphicsInput::Confirm) && !page.settings.async_record);
    check(GraphicsRowValue(page.settings, GraphicsRow::AsyncRecord) == L"No");
    check(!HandleGraphicsInput(page, GraphicsInput::Back) && !page.open);
    check(ShaderBuildFromValue(1) == ShaderBuild::Balanced && !ShaderBuildFromValue(7));

    // A game follows Settings > Graphics until it sets its own.
    const GraphicsSettings global;
    GameGraphics game;
    check(ResolveGraphics(global, game) == global);
    check(UsesScreenResolution(global)); // FSR presents at the TV's resolution
    check(GameGraphicsRowValue(global, game, GraphicsRow::Upscaling) == L"Ajustes · FSR 1 · equilibrado");
    // All games at 4K, this one with frame generation: its FSR goes off, only for it.
    std::size_t row = static_cast<std::size_t>(GraphicsRow::FrameGen);
    check(HandleGameGraphicsInput(game, row, GraphicsInput::Right));
    check(game.frame_gen == true && !game.upscaling);
    auto resolved = ResolveGraphics(global, game);
    check(resolved.frame_gen && resolved.upscaling == Upscaling::Bilinear);
    check(!UsesScreenResolution(resolved)); // frame generation stays at 1080p
    check(GameGraphicsRowValue(global, game, GraphicsRow::FrameGen) == L"Sí");
    check(GameGraphicsRowValue(global, game, GraphicsRow::Upscaling) == L"Ajustes · Bilineal");
    // Its own FSR turns its frame generation off.
    row = static_cast<std::size_t>(GraphicsRow::Upscaling);
    check(HandleGameGraphicsInput(game, row, GraphicsInput::Right));
    check(game.upscaling == Upscaling::Bilinear && game.frame_gen == true);
    check(HandleGameGraphicsInput(game, row, GraphicsInput::Right));
    check(game.upscaling == Upscaling::FsrSharp && game.frame_gen == false);
    resolved = ResolveGraphics(global, game);
    check(!resolved.frame_gen && resolved.upscaling == Upscaling::FsrSharp);
    check(UsesScreenResolution(resolved));
    // Frame generation on again for the game turns its own FSR off.
    row = static_cast<std::size_t>(GraphicsRow::FrameGen);
    check(HandleGameGraphicsInput(game, row, GraphicsInput::Left)); // off -> on
    check(game.frame_gen == true && game.upscaling == Upscaling::Bilinear);
    // Steps go through "follow" both ways.
    check(HandleGameGraphicsInput(game, row, GraphicsInput::Left) && !game.frame_gen);
    // Frame generation on everywhere, FSR for one game: the game's FSR wins.
    GraphicsSettings with_frame_gen;
    SetFrameGen(with_frame_gen, true);
    const GameGraphics sharp{.upscaling = Upscaling::FsrSharp};
    resolved = ResolveGraphics(with_frame_gen, sharp);
    check(!resolved.frame_gen && resolved.upscaling == Upscaling::FsrSharp);
    check(UsesScreenResolution(resolved));
    check(!UsesScreenResolution(ResolveGraphics(with_frame_gen, {}))); // the other games at 1080p
    // Shaders: the game's own over the global ones; B is the caller's.
    row = static_cast<std::size_t>(GraphicsRow::Shaders);
    GameGraphics shaders;
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && shaders.shaders == ShaderBuild::Skip);
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && shaders.shaders == ShaderBuild::Balanced);
    check(ResolveGraphics(global, shaders).shaders == ShaderBuild::Balanced);
    check(!HandleGameGraphicsInput(shaders, row, GraphicsInput::Back));
    // The GPU fence: follows Settings > Graphics until the game has its own, only for that game.
    check(!HandleGameGraphicsInput(shaders, row, GraphicsInput::Down) && row == static_cast<std::size_t>(GraphicsRow::GpuFence));
    GraphicsSettings safe_global;
    safe_global.gpu_fence = GpuFence::Safe;
    check(ResolveGraphics(safe_global, shaders).gpu_fence == GpuFence::Safe);
    check(GameGraphicsRowValue(safe_global, shaders, GraphicsRow::GpuFence) == L"Ajustes · Seguro");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && shaders.gpu_fence == GpuFence::Immediate);
    check(ResolveGraphics(safe_global, shaders).gpu_fence == GpuFence::Immediate);
    check(GameGraphicsRowValue(safe_global, shaders, GraphicsRow::GpuFence) == L"Inmediato");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Left) && !shaders.gpu_fence);
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Left) && shaders.gpu_fence == GpuFence::Accurate);
    // The command thread: follows Settings > Graphics until the game has its own, only for that game.
    check(!HandleGameGraphicsInput(shaders, row, GraphicsInput::Down) && row == static_cast<std::size_t>(GraphicsRow::AsyncRecord));
    GraphicsSettings no_thread;
    no_thread.async_record = false;
    check(!ResolveGraphics(no_thread, shaders).async_record);
    check(GameGraphicsRowValue(no_thread, shaders, GraphicsRow::AsyncRecord) == L"Ajustes · No");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && shaders.async_record == true);
    check(ResolveGraphics(no_thread, shaders).async_record);
    check(GameGraphicsRowValue(no_thread, shaders, GraphicsRow::AsyncRecord) == L"Sí");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && shaders.async_record == false);
    check(!ResolveGraphics(global, shaders).async_record);
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && !shaders.async_record);
    // The guest clock is the game's own: a row only in its pane, off until the player turns it on.
    check(!HandleGameGraphicsInput(shaders, row, GraphicsInput::Down) && row == static_cast<std::size_t>(GraphicsRow::GuestClock));
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GuestClock) == L"No");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Confirm) && shaders.slow_guest_clock);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GuestClock) == L"Sí");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && !shaders.slow_guest_clock);

    // The advanced rows: only the game's, default until changed, and reset only with A.
    GameGraphics tuned;
    const auto go = [&](GraphicsRow target) { row = static_cast<std::size_t>(target); };
    check(IsAdvancedRow(GraphicsRow::Anisotropy) && !IsAdvancedRow(GraphicsRow::GuestClock) && GraphicsRowCount == 5);
    go(GraphicsRow::Anisotropy);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::Anisotropy) == L"Lo que pida el juego");
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Right) && tuned.advanced.anisotropy == Settings::AnisotropyMode::X2);
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Left) && tuned.advanced.anisotropy == Settings::AnisotropyMode::Default);
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Left) && tuned.advanced.anisotropy == Settings::AnisotropyMode::X16);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::Anisotropy) == L"16x");
    go(GraphicsRow::TextureQuality);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureQuality) == L"Ahorro de memoria");
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Confirm) && tuned.advanced.full_textures);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureQuality) == L"Completa");
    go(GraphicsRow::TextureMemory);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureMemory) == L"Automático");
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Right) && tuned.advanced.texture_cap_mib == 512);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureMemory) == L"512 MiB");
    go(GraphicsRow::GpuAccuracy);
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Right) && tuned.advanced.gpu_accuracy == Settings::GpuAccuracy::Low);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::GpuAccuracy) == L"Baja");
    go(GraphicsRow::ShaderWorkers);
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Left) && tuned.advanced.shader_workers == 8);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::ShaderWorkers) == L"8");
    go(GraphicsRow::ConsoleMode);
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Right) && tuned.advanced.console_mode == Settings::ConsoleMode::Handheld);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::ConsoleMode) == L"Portátil");
    check(!HandleGameGraphicsInput(tuned, row, GraphicsInput::Down) && row == static_cast<std::size_t>(GraphicsRow::ResetAdvanced));
    check(!HandleGameGraphicsInput(tuned, row, GraphicsInput::Right) && !HandleGameGraphicsInput(tuned, row, GraphicsInput::Left));
    check(tuned.advanced.full_textures);
    tuned.slow_guest_clock = true;
    tuned.frame_gen = true;
    check(HandleGameGraphicsInput(tuned, row, GraphicsInput::Confirm) && tuned.advanced == GameAdvancedGraphics{});
    check(tuned.slow_guest_clock && tuned.frame_gen == true); // the reset leaves the other rows alone
    check(!HandleGameGraphicsInput(tuned, row, GraphicsInput::Confirm)); // nothing left to reset
    check(!HandleGameGraphicsInput(tuned, row, GraphicsInput::Down) && row == 0);

    std::puts("graphics settings: defaults, FSR or frame generation, page, per-game overrides PASS");
}
