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
    // The guest clock is the game's own: a row only in its pane, off until the player turns it on.
    check(!HandleGameGraphicsInput(shaders, row, GraphicsInput::Down) && row == static_cast<std::size_t>(GraphicsRow::GuestClock));
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GuestClock) == L"No");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Confirm) && shaders.slow_guest_clock);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GuestClock) == L"Sí");
    check(HandleGameGraphicsInput(shaders, row, GraphicsInput::Right) && !shaders.slow_guest_clock);
    check(!HandleGameGraphicsInput(shaders, row, GraphicsInput::Down) && row == 0);

    std::puts("graphics settings: defaults, FSR or frame generation, page, per-game overrides PASS");
}
