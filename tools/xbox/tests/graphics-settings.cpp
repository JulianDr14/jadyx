// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/i18n/localization.cpp and src/jadyx/i18n/locales/*.cpp.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "jadyx/settings/graphics_page.h"

int main() {
    using namespace Jadyx;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));

    // Defaults: FSR up to the screen, frame generation off, balanced shader builds.
    const GraphicsSettings defaults;
    check(defaults.upscaling == Upscaling::FsrBalanced && !defaults.frame_gen && defaults.shaders == ShaderBuild::Balanced);
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

    // The page: sections, then their rows; lists wrap, B goes back and then closes.
    GraphicsPage page{true};
    // Sections first: Settings > Graphics has no memory rows, so it lists three.
    check(GraphicsSections(false).size() == 3 && GraphicsSections(true).size() == 4);
    check(!HandleGraphicsInput(page, GraphicsInput::Up) && page.cursor.selected == 2 && !page.cursor.section);
    check(!HandleGraphicsInput(page, GraphicsInput::Down) && page.cursor.selected == 0);
    // A opens Image, which only holds its global rows; B goes back onto its entry.
    check(!HandleGraphicsInput(page, GraphicsInput::Confirm) && page.cursor.section == GraphicsSection::Image);
    check(page.cursor.selected == 0 && GraphicsListSize(page.cursor, false) == 2);
    check(GraphicsCursorRow(page.cursor, false) == GraphicsRow::Upscaling);
    check(!HandleGraphicsInput(page, GraphicsInput::Back) && !page.cursor.section && page.cursor.selected == 0 && page.open);
    check(!HandleGraphicsInput(page, GraphicsInput::Down) && !HandleGraphicsInput(page, GraphicsInput::Right));
    check(page.cursor.section == GraphicsSection::Performance && page.cursor.selected == 0);
    check(GraphicsCursorRow(page.cursor, false) == GraphicsRow::Shaders);
    check(HandleGraphicsInput(page, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Wait);
    check(HandleGraphicsInput(page, GraphicsInput::Left) && page.settings.shaders == ShaderBuild::Balanced);
    check(!HandleGraphicsInput(page, GraphicsInput::Back) && page.cursor.selected == 1);
    // Every row lives in one section, and says what it gains and what it costs.
    for (std::size_t i = 0; i < static_cast<std::size_t>(GraphicsRow::ResetAdvanced); ++i) {
        const auto row = static_cast<GraphicsRow>(i);
        const auto rows = GraphicsSectionAllRows(GraphicsSectionOf(row));
        check(std::count(rows.begin(), rows.end(), row) == 1 && GraphicsRowGainCost(row).has_value());
    }
    check(!GraphicsRowGainCost(GraphicsRow::ResetAdvanced));
    check(GraphicsRowHint(GraphicsRow::FrameGen) == L"Gana: se ve más fluido · Cuesta: más retraso, bordes raros y sin FSR");
    // The rows themselves: Left/Right/A change them.
    const auto set = [&](GraphicsRow row, GraphicsInput input) { return ChangeGraphicsRow(page.settings, row, input); };
    check(!set(GraphicsRow::FrameGen, GraphicsInput::Down) && !set(GraphicsRow::FrameGen, GraphicsInput::Back));
    check(GraphicsRowValue(page.settings, GraphicsRow::Upscaling) == L"FSR 1 · equilibrado");
    check(set(GraphicsRow::FrameGen, GraphicsInput::Confirm));
    check(page.settings.frame_gen && page.settings.upscaling == Upscaling::Bilinear);
    check(GraphicsRowValue(page.settings, GraphicsRow::FrameGen) == L"Sí");
    check(set(GraphicsRow::Upscaling, GraphicsInput::Right));
    check(page.settings.upscaling == Upscaling::FsrSharp && !page.settings.frame_gen);
    check(set(GraphicsRow::Shaders, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Wait);
    check(set(GraphicsRow::Shaders, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Skip);
    check(set(GraphicsRow::Shaders, GraphicsInput::Right) && page.settings.shaders == ShaderBuild::Balanced);
    check(set(GraphicsRow::Shaders, GraphicsInput::Left) && page.settings.shaders == ShaderBuild::Skip);
    check(set(GraphicsRow::Shaders, GraphicsInput::Left) && page.settings.shaders == ShaderBuild::Wait);
    check(GraphicsRowValue(page.settings, GraphicsRow::Shaders) == L"Esperar");
    check(GraphicsRowValue(page.settings, GraphicsRow::GpuFence) == L"Inmediato");
    for (const GpuFence fence : {GpuFence::Safe, GpuFence::Wait, GpuFence::Balanced, GpuFence::Accurate, GpuFence::Immediate})
        check(set(GraphicsRow::GpuFence, GraphicsInput::Right) && page.settings.gpu_fence == fence);
    check(set(GraphicsRow::GpuFence, GraphicsInput::Left) && page.settings.gpu_fence == GpuFence::Accurate);
    check(GraphicsRowValue(page.settings, GraphicsRow::GpuFence) == L"Preciso");
    check(set(GraphicsRow::GpuFence, GraphicsInput::Right) && page.settings.gpu_fence == GpuFence::Immediate);
    // The command thread: on by default.
    check(defaults.async_record && GraphicsRowValue(page.settings, GraphicsRow::AsyncRecord) == L"Sí");
    check(set(GraphicsRow::AsyncRecord, GraphicsInput::Confirm) && !page.settings.async_record);
    check(GraphicsRowValue(page.settings, GraphicsRow::AsyncRecord) == L"No");
    // The CPU write queue: off by default; on, every write is queued (no row of its own).
    check(!defaults.cpu_write_queue && GraphicsRowValue(page.settings, GraphicsRow::CpuWriteQueue) == L"No");
    check(set(GraphicsRow::CpuWriteQueue, GraphicsInput::Confirm) && page.settings.cpu_write_queue);
    check(GraphicsRowValue(page.settings, GraphicsRow::CpuWriteQueue) == L"Sí");
    // The GPU thread's fast paths: on by default.
    check(defaults.gpu_fast_paths && GraphicsRowValue(page.settings, GraphicsRow::GpuFastPaths) == L"Sí");
    check(set(GraphicsRow::GpuFastPaths, GraphicsInput::Confirm) && !page.settings.gpu_fast_paths);
    check(GraphicsRowValue(page.settings, GraphicsRow::GpuFastPaths) == L"No");
    // The fixed core map: off by default (only the GPU thread pinned).
    check(!defaults.fixed_core_map && GraphicsRowValue(page.settings, GraphicsRow::FixedCoreMap) == L"No");
    check(set(GraphicsRow::FixedCoreMap, GraphicsInput::Confirm) && page.settings.fixed_core_map);
    check(GraphicsRowValue(page.settings, GraphicsRow::FixedCoreMap) == L"Sí");
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
    check(ChangeGameGraphicsRow(game, static_cast<GraphicsRow>(row), GraphicsInput::Right));
    check(game.frame_gen == true && !game.upscaling);
    auto resolved = ResolveGraphics(global, game);
    check(resolved.frame_gen && resolved.upscaling == Upscaling::Bilinear);
    check(!UsesScreenResolution(resolved)); // frame generation stays at 1080p
    check(GameGraphicsRowValue(global, game, GraphicsRow::FrameGen) == L"Sí");
    check(GameGraphicsRowValue(global, game, GraphicsRow::Upscaling) == L"Ajustes · Bilineal");
    // Its own FSR turns its frame generation off.
    row = static_cast<std::size_t>(GraphicsRow::Upscaling);
    check(ChangeGameGraphicsRow(game, static_cast<GraphicsRow>(row), GraphicsInput::Right));
    check(game.upscaling == Upscaling::Bilinear && game.frame_gen == true);
    check(ChangeGameGraphicsRow(game, static_cast<GraphicsRow>(row), GraphicsInput::Right));
    check(game.upscaling == Upscaling::FsrSharp && game.frame_gen == false);
    resolved = ResolveGraphics(global, game);
    check(!resolved.frame_gen && resolved.upscaling == Upscaling::FsrSharp);
    check(UsesScreenResolution(resolved));
    // Frame generation on again for the game turns its own FSR off.
    row = static_cast<std::size_t>(GraphicsRow::FrameGen);
    check(ChangeGameGraphicsRow(game, static_cast<GraphicsRow>(row), GraphicsInput::Left)); // off -> on
    check(game.frame_gen == true && game.upscaling == Upscaling::Bilinear);
    // Steps go through "follow" both ways.
    check(ChangeGameGraphicsRow(game, static_cast<GraphicsRow>(row), GraphicsInput::Left) && !game.frame_gen);
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
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.shaders == ShaderBuild::Skip);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.shaders == ShaderBuild::Balanced);
    check(ResolveGraphics(global, shaders).shaders == ShaderBuild::Balanced);
    check(!ChangeGameGraphicsRow(shaders, GraphicsRow::Shaders, GraphicsInput::Back));
    // The GPU fence: follows Settings > Graphics until the game has its own, only for that game.
    row = static_cast<std::size_t>(GraphicsRow::GpuFence);
    GraphicsSettings safe_global;
    safe_global.gpu_fence = GpuFence::Safe;
    check(ResolveGraphics(safe_global, shaders).gpu_fence == GpuFence::Safe);
    check(GameGraphicsRowValue(safe_global, shaders, GraphicsRow::GpuFence) == L"Ajustes · Seguro");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.gpu_fence == GpuFence::Immediate);
    check(ResolveGraphics(safe_global, shaders).gpu_fence == GpuFence::Immediate);
    check(GameGraphicsRowValue(safe_global, shaders, GraphicsRow::GpuFence) == L"Inmediato");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Left) && !shaders.gpu_fence);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Left) && shaders.gpu_fence == GpuFence::Accurate);
    // The command thread: follows Settings > Graphics until the game has its own, only for that game.
    row = static_cast<std::size_t>(GraphicsRow::AsyncRecord);
    GraphicsSettings no_thread;
    no_thread.async_record = false;
    check(!ResolveGraphics(no_thread, shaders).async_record);
    check(GameGraphicsRowValue(no_thread, shaders, GraphicsRow::AsyncRecord) == L"Ajustes · No");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.async_record == true);
    check(ResolveGraphics(no_thread, shaders).async_record);
    check(GameGraphicsRowValue(no_thread, shaders, GraphicsRow::AsyncRecord) == L"Sí");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.async_record == false);
    check(!ResolveGraphics(global, shaders).async_record);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.async_record);
    // The CPU write queue: off for a game that follows the default, on only where the game or
    // Settings > Graphics asks, and a game can keep it off when Settings > Graphics has it on.
    row = static_cast<std::size_t>(GraphicsRow::CpuWriteQueue);
    check(!ResolveGraphics(global, shaders).cpu_write_queue);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::CpuWriteQueue) == L"Ajustes · No");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.cpu_write_queue == true);
    check(ResolveGraphics(global, shaders).cpu_write_queue);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::CpuWriteQueue) == L"Sí");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.cpu_write_queue == false);
    GraphicsSettings queue_global;
    queue_global.cpu_write_queue = true;
    check(!ResolveGraphics(queue_global, shaders).cpu_write_queue);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.cpu_write_queue);
    check(ResolveGraphics(queue_global, shaders).cpu_write_queue);
    // The GPU thread's fast paths: on for a game that follows the default, off where the game asks.
    row = static_cast<std::size_t>(GraphicsRow::GpuFastPaths);
    check(ResolveGraphics(global, shaders).gpu_fast_paths);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GpuFastPaths) == L"Ajustes · Sí");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.gpu_fast_paths == true);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.gpu_fast_paths == false);
    check(!ResolveGraphics(global, shaders).gpu_fast_paths);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GpuFastPaths) == L"No");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.gpu_fast_paths);
    // The fixed core map: follows Settings > Graphics until the game has its own.
    row = static_cast<std::size_t>(GraphicsRow::FixedCoreMap);
    check(!ResolveGraphics(global, shaders).fixed_core_map);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::FixedCoreMap) == L"Ajustes · No");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.fixed_core_map == true);
    check(ResolveGraphics(global, shaders).fixed_core_map);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.fixed_core_map == false);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.fixed_core_map);
    // The ASTC array RGBA8 cap: follows Settings > Graphics (160 MiB) until the game has its own.
    row = static_cast<std::size_t>(GraphicsRow::AstcArrayCap);
    check(ResolveGraphics(global, shaders).astc_array_cap_mib == 160);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::AstcArrayCap) == L"Ajustes · 160 MiB");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && shaders.astc_array_cap_mib == 64u);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Left) && !shaders.astc_array_cap_mib);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Left) && shaders.astc_array_cap_mib == 1024u);
    check(ResolveGraphics(global, shaders).astc_array_cap_mib == 1024);
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.astc_array_cap_mib);
    // The guest clock is the game's own: a row only in its pane, off until the player turns it on.
    row = static_cast<std::size_t>(GraphicsRow::GuestClock);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GuestClock) == L"No");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Confirm) && shaders.slow_guest_clock);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::GuestClock) == L"Sí");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.slow_guest_clock);
    // Exact GPU reads are the game's own too, and off until the player turns them on.
    row = static_cast<std::size_t>(GraphicsRow::ExactGpuReads);
    check(!IsAdvancedRow(GraphicsRow::ExactGpuReads) && GameGraphicsRowValue(global, shaders, GraphicsRow::ExactGpuReads) == L"No");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Confirm) && shaders.exact_gpu_reads);
    check(GameGraphicsRowValue(global, shaders, GraphicsRow::ExactGpuReads) == L"Sí");
    check(ChangeGameGraphicsRow(shaders, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !shaders.exact_gpu_reads);

    // The advanced rows: only the game's, default until changed, and reset only with A.
    GameGraphics tuned;
    const auto go = [&](GraphicsRow target) { row = static_cast<std::size_t>(target); };
    check(IsAdvancedRow(GraphicsRow::Anisotropy) && !IsAdvancedRow(GraphicsRow::GuestClock) && GraphicsRowCount == 9);
    go(GraphicsRow::Anisotropy);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::Anisotropy) == L"Lo que pida el juego");
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Right) && tuned.advanced.anisotropy == Settings::AnisotropyMode::X2);
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Left) && tuned.advanced.anisotropy == Settings::AnisotropyMode::Default);
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Left) && tuned.advanced.anisotropy == Settings::AnisotropyMode::X16);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::Anisotropy) == L"16x");
    go(GraphicsRow::TextureQuality);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureQuality) == L"Ahorro de memoria");
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Confirm) && tuned.advanced.full_textures);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureQuality) == L"Completa");
    go(GraphicsRow::TextureMemory);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureMemory) == L"Automático");
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Right) && tuned.advanced.texture_cap_mib == 512);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::TextureMemory) == L"512 MiB");
    go(GraphicsRow::GpuAccuracy);
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Right) && tuned.advanced.gpu_accuracy == Settings::GpuAccuracy::Low);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::GpuAccuracy) == L"Baja");
    go(GraphicsRow::ShaderWorkers);
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Left) && tuned.advanced.shader_workers == 8);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::ShaderWorkers) == L"8");
    go(GraphicsRow::ConsoleMode);
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Right) && tuned.advanced.console_mode == Settings::ConsoleMode::Handheld);
    check(GameGraphicsRowValue(global, tuned, GraphicsRow::ConsoleMode) == L"Portátil");
    row = static_cast<std::size_t>(GraphicsRow::ResetAdvanced);
    check(!ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Right) && !ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Left));
    check(tuned.advanced.full_textures);
    tuned.slow_guest_clock = true;
    tuned.frame_gen = true;
    check(ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Confirm) && tuned.advanced == GameAdvancedGraphics{});
    check(tuned.slow_guest_clock && tuned.frame_gen == true); // the reset leaves the other rows alone
    check(!ChangeGameGraphicsRow(tuned, static_cast<GraphicsRow>(row), GraphicsInput::Confirm)); // nothing left to reset

    // The game's pane: four sections and the reset row; B in a section goes back to the list.
    GraphicsCursor cursor;
    GameGraphics navigated;
    check(GraphicsListSize(cursor, true) == 5);
    check(!HandleGameGraphicsInput(navigated, cursor, GraphicsInput::Up) && cursor.selected == 4);
    check(GraphicsCursorRow(cursor, true) == GraphicsRow::ResetAdvanced);
    check(!HandleGameGraphicsInput(navigated, cursor, GraphicsInput::Up) && cursor.selected == 3);
    check(!HandleGameGraphicsInput(navigated, cursor, GraphicsInput::Confirm) && cursor.section == GraphicsSection::Memory);
    check(GraphicsCursorRow(cursor, true) == GraphicsRow::TextureQuality);
    check(HandleGameGraphicsInput(navigated, cursor, GraphicsInput::Confirm) && navigated.advanced.full_textures);
    check(!HandleGameGraphicsInput(navigated, cursor, GraphicsInput::Back) && !cursor.section && cursor.selected == 3);
    check(!HandleGameGraphicsInput(navigated, cursor, GraphicsInput::Back)); // the caller's

    // Y: Settings > Graphics back to the emulator's defaults; a game back on Settings > Graphics.
    GraphicsSettings changed_global;
    check(!ResetGraphicsSettings(changed_global));
    SetFrameGen(changed_global, true);
    changed_global.fixed_core_map = true;
    check(ResetGraphicsSettings(changed_global) && changed_global == GraphicsSettings{});
    GameGraphics own{.gpu_fence = GpuFence::Safe, .slow_guest_clock = true};
    own.advanced.full_textures = true;
    check(SyncGameGraphics(own) && own == GameGraphics{} && !SyncGameGraphics(own));
    check(ResolveGraphics(safe_global, own) == safe_global);

    std::puts("graphics settings: defaults, FSR or frame generation, sections, gain/cost hints, per-game overrides PASS");
}
