// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/i18n/localization.cpp and src/jadyx/i18n/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include "jadyx/game/game_menu.h"
#include "jadyx/settings/system_page.h"

int main() {
    using namespace Jadyx;
    auto check = [](bool ok) { if (!ok) std::abort(); };

    // Both pressed together: never reach the game, fire once after a second held.
    {
        MenuComboFilter f;
        int fires = 0;
        for (std::uint32_t t = 0; t <= 1500; t += 4) {
            bool plus = true, minus = true;
            fires += f.Filter(plus, minus, t);
            check(!plus && !minus);
        }
        check(fires == 1);
        bool plus = false, minus = false;
        check(!f.Filter(plus, minus, 1504) && !plus && !minus);
    }
    // View first, Menu 40 ms later: still the combo, - never delivered; released early: no fire.
    {
        MenuComboFilter f;
        bool fired = false;
        for (std::uint32_t t = 0; t < 600; t += 4) {
            bool plus = t >= 40, minus = true;
            fired |= f.Filter(plus, minus, t);
            check(!plus && !minus);
        }
        bool plus = false, minus = false;
        f.Filter(plus, minus, 604);
        check(!fired && !plus && !minus);
    }
    // A quick tap of + reaches the game (on release), then releases.
    {
        MenuComboFilter f;
        bool plus = true, minus = false;
        f.Filter(plus, minus, 0);
        check(!plus);
        plus = true;
        f.Filter(plus, minus, 30);
        check(!plus);
        plus = false;
        f.Filter(plus, minus, 60);
        check(plus && !minus);
        plus = false;
        f.Filter(plus, minus, 64);
        check(!plus);
    }
    // A held + is delivered after the pending window and stays delivered.
    {
        MenuComboFilter f;
        bool delivered = false;
        for (std::uint32_t t = 0; t <= 400; t += 4) {
            bool plus = true, minus = false;
            f.Filter(plus, minus, t);
            if (t >= MenuComboFilter::PendingMs) check(plus);
            else check(!plus);
            delivered |= plus;
        }
        check(delivered);
    }

    // Quick menu, in Spanish: actions, and a page each for the controller and the display.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));
    GameMenu menu{{ConsoleControllerStyle::Auto, false, 0.12f}};
    check(menu.CurrentPage() == GameMenu::Page::Quick && menu.PageTitle() == L"Jadyx Hub");
    check(menu.Rows().size() == 4 && menu.SelectedItem() == GameMenu::Continue);
    check(menu.Rows()[0].kind == GameMenuRow::Kind::Action && menu.Rows()[0].icon == UiIcon::Play);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Back) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Toggle) == GameMenuResult::Resume);
    check(menu.Apply(MenuAction::Up) == GameMenuResult::None && menu.SelectedItem() == GameMenu::Library);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::Library);
    check(menu.Apply(MenuAction::Down) == GameMenuResult::None && menu.SelectedItem() == GameMenu::Continue);
    // The controller's page: left does nothing on its row, A opens it, B comes back to the row.
    menu.Apply(MenuAction::Down);
    check(menu.SelectedItem() == GameMenu::ControllerPage && menu.Rows()[1].kind == GameMenuRow::Kind::Page);
    check(menu.Rows()[1].detail.find(L"12%") != std::wstring::npos);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::None && menu.CurrentPage() == GameMenu::Page::Quick);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::None);
    check(menu.CurrentPage() == GameMenu::Page::Controller && menu.PageTitle() == L"Mando");
    check(menu.Rows().size() == 8 && menu.SelectedItem() == GameMenu::Style && !menu.Rows()[0].icon);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged);
    check(menu.Settings().style == ConsoleControllerStyle::Pro);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::SettingsChanged);
    check(menu.Apply(MenuAction::Left) == GameMenuResult::SettingsChanged);
    check(menu.Settings().style == ConsoleControllerStyle::RightJoycon);
    check(menu.Rows()[0].label == L"La consola lo ve como");
    check(menu.Rows()[0].kind == GameMenuRow::Kind::Choice);
    check(menu.Rows()[0].value.find(L"Joy-Con derecho") != std::wstring::npos);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::SettingsChanged && menu.Settings().swap_face_buttons);
    menu.Apply(MenuAction::Down);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged && menu.Settings().deadzone > 0.15f);
    check(menu.Apply(MenuAction::Right) == GameMenuResult::SettingsChanged && menu.Settings().deadzone < 0.10f);
    check(menu.Rows()[2].value == L"8%");
    // Without a game the right stick's rows change Settings' values, with nothing under them.
    check(menu.Apply(MenuAction::Down) == GameMenuResult::None && menu.SelectedItem() == GameMenu::RStickInvertX);
    check(menu.Rows()[3].label == L"Invertir eje X del stick derecho" && menu.Rows()[3].value == L"No");
    check(menu.Apply(MenuAction::Confirm) == GameMenuResult::RightStickChanged);
    check(menu.Settings().right_stick_global.invert_x && !menu.Settings().right_stick.invert_x);
    check(menu.Rows()[3].value == L"Sí" && menu.Rows()[3].detail.empty());
    menu.Apply(MenuAction::Down);
    menu.Apply(MenuAction::Down);
    check(menu.SelectedItem() == GameMenu::RStickSensitivity && menu.Rows()[5].value == L"100%");
    check(menu.Apply(MenuAction::Left) == GameMenuResult::RightStickChanged);
    check(menu.Settings().right_stick_global.stick_sensitivity == 90 && menu.Rows()[5].value == L"90%");
    menu.Apply(MenuAction::Down);
    menu.Apply(MenuAction::Down);
    check(menu.SelectedItem() == GameMenu::RStickMouseSensitivity);
    check(menu.Rows()[7].label == L"Sensibilidad del mouse");
    check(menu.Apply(MenuAction::Down) == GameMenuResult::None && menu.SelectedItem() == GameMenu::Style);
    check(menu.Apply(MenuAction::Back) == GameMenuResult::None);
    check(menu.CurrentPage() == GameMenu::Page::Quick && menu.SelectedItem() == GameMenu::ControllerPage);
    check(menu.Rows()[1].detail.find(L"8%") != std::wstring::npos);
    // Presses the menu's own rows ignore.
    for (const auto action : {MenuAction::Alternate, MenuAction::Refresh, MenuAction::PreviousTab,
                              MenuAction::NextTab})
        check(menu.Apply(action) == GameMenuResult::None);

    // A game's right stick: a step from what the row shows is the game's own, and landing back on
    // Settings' value follows Settings again.
    {
        GameMenuSettings stick_settings{};
        stick_settings.right_stick_global.stick_sensitivity = 125;
        stick_settings.right_stick.invert_y = true;
        stick_settings.right_stick_per_game = true;
        GameMenu game{stick_settings, true};
        while (game.SelectedItem() != GameMenu::ControllerPage) game.Apply(MenuAction::Down);
        game.Apply(MenuAction::Confirm);
        const auto row = [&](GameMenu::Item item) {
            while (game.SelectedItem() != item) game.Apply(MenuAction::Down);
            return game.Rows()[game.Selected()];
        };
        check(row(GameMenu::RStickInvertY).value == L"Sí" && row(GameMenu::RStickInvertY).detail == L"Solo este juego");
        check(game.Apply(MenuAction::Right) == GameMenuResult::RightStickChanged);
        check(!game.Settings().right_stick.invert_y && !game.Settings().right_stick_global.invert_y);
        check(row(GameMenu::RStickInvertY).detail == L"Como en Ajustes");
        check(row(GameMenu::RStickSensitivity).value == L"125%" && row(GameMenu::RStickSensitivity).detail == L"Como en Ajustes");
        check(game.Apply(MenuAction::Right) == GameMenuResult::RightStickChanged);
        check(game.Settings().right_stick.stick_sensitivity == 150 && game.Settings().right_stick_global.stick_sensitivity == 125);
        check(row(GameMenu::RStickSensitivity).value == L"150%" && row(GameMenu::RStickSensitivity).detail == L"Solo este juego");
        game.Apply(MenuAction::Left);
        check(!game.Settings().right_stick.stick_sensitivity);
        check(row(GameMenu::RStickMouse).value == L"No");
        game.Apply(MenuAction::Confirm);
        check(game.Settings().right_stick.mouse == true && !game.Settings().right_stick_global.mouse);
        check(row(GameMenu::RStickMouseSensitivity).value == L"100%");
        game.Apply(MenuAction::Right);
        check(game.Settings().right_stick.mouse_sensitivity == 125);
    }

    // A game with a title id offers its cheats after the screenshot, with how many are on.
    GameMenuSettings shot_settings{};
    shot_settings.screenshot = true;
    GameMenu with_cheats{shot_settings, true};
    with_cheats.SetCheatsOn(3);
    check(with_cheats.Rows().size() == 8);
    check(with_cheats.Rows()[1].label == L"Captura de pantalla" && with_cheats.Rows()[1].icon == UiIcon::Camera);
    check(with_cheats.Rows()[2].kind == GameMenuRow::Kind::Page && with_cheats.Rows()[2].detail == L"3 activos");
    with_cheats.Apply(MenuAction::Down);
    check(with_cheats.Apply(MenuAction::Confirm) == GameMenuResult::Screenshot);
    with_cheats.SetScreenshotNote(L"Guardada: x.png");
    check(with_cheats.Rows()[1].detail == L"Guardada: x.png");
    check(with_cheats.Apply(MenuAction::Down) == GameMenuResult::None && with_cheats.SelectedItem() == GameMenu::Cheats);
    check(with_cheats.Apply(MenuAction::Right) == GameMenuResult::None);
    check(with_cheats.Apply(MenuAction::Confirm) == GameMenuResult::Cheats);
    with_cheats.SetCheatsOn(1);
    check(with_cheats.Rows()[2].detail == L"1 activo");
    // Mods only inform, after the cheats: no button does anything on them.
    with_cheats.SetModsOn(2);
    check(with_cheats.Rows().size() == 9 && with_cheats.Rows()[3].kind == GameMenuRow::Kind::Info);
    with_cheats.Apply(MenuAction::Down);
    check(with_cheats.SelectedItem() == GameMenu::Mods && with_cheats.Apply(MenuAction::Confirm) == GameMenuResult::None);
    GameMenu mods_only{{}};
    mods_only.SetModsOn(1);
    check(mods_only.Rows().size() == 5);
    mods_only.Apply(MenuAction::Down);
    check(mods_only.SelectedItem() == GameMenu::Mods);

    // A game with a title id has its own offline mode after the display page: as in Settings >
    // System, then on, then off.
    GameMenuSettings offline_settings{};
    offline_settings.offline_default = true;
    GameMenu offline{offline_settings, true};
    while (offline.SelectedItem() != GameMenu::OfflineMode) offline.Apply(MenuAction::Down);
    const auto offline_value = [&] { return offline.Rows()[offline.Selected()].value; };
    check(offline_value() == L"Sistema · Sí" && offline.Rows()[offline.Selected()].icon == UiIcon::Airplane);
    check(offline.Apply(MenuAction::Right) == GameMenuResult::OfflineModeChanged &&
          offline.Settings().offline_mode == true && offline_value() == L"Sí");
    check(offline.Apply(MenuAction::Confirm) == GameMenuResult::OfflineModeChanged &&
          offline.Settings().offline_mode == false && offline_value() == L"No");
    check(offline.Apply(MenuAction::Right) == GameMenuResult::OfflineModeChanged && !offline.Settings().offline_mode);
    check(offline.Apply(MenuAction::Left) == GameMenuResult::OfflineModeChanged &&
          offline.Settings().offline_mode == false);
    check(offline.Apply(MenuAction::Left) == GameMenuResult::OfflineModeChanged &&
          offline.Settings().offline_mode == true);
    check(offline.Apply(MenuAction::Left) == GameMenuResult::OfflineModeChanged && !offline.Settings().offline_mode);
    // Without a title id there is nothing to keep it under.
    for (const auto& row : GameMenu{offline_settings}.Rows()) check(row.icon != UiIcon::Airplane);

    // A game with a title id has a Graphics page, with what can change while it runs.
    {
        GameMenu graphics{{}, true};
        while (graphics.SelectedItem() != GameMenu::GraphicsMenu) graphics.Apply(MenuAction::Down);
        check(graphics.Apply(MenuAction::Confirm) == GameMenuResult::None);
        check(graphics.CurrentPage() == GameMenu::Page::Graphics && graphics.PageTitle() == L"Gráficos");
        check(graphics.Rows().size() == 9);
        for (const auto& row : graphics.Rows()) {
            check(row.label != std::wstring{I18n::Tr(I18n::Text::AdvancedAnisotropy)});
            check(row.label != std::wstring{I18n::Tr(I18n::Text::AdvancedTextureQuality)});
            check(row.label != std::wstring{I18n::Tr(I18n::Text::AdvancedShaderWorkers)});
            check(row.label != std::wstring{I18n::Tr(I18n::Text::AdvancedReset)});
        }
        check(graphics.SelectedItem() == GameMenu::GfxUpscaling);
        check(graphics.Rows()[0].detail == L"Como en Ajustes");
        check(graphics.Apply(MenuAction::Right) == GameMenuResult::GraphicsChanged);
        check(graphics.Settings().graphics.upscaling.has_value() && graphics.Rows()[0].detail == L"Solo este juego");
        // The row steps the game's own value, whichever line of the page it is on.
        while (graphics.SelectedItem() != GameMenu::GfxGuestClock) graphics.Apply(MenuAction::Down);
        check(graphics.Apply(MenuAction::Confirm) == GameMenuResult::GraphicsChanged &&
              graphics.Settings().graphics.slow_guest_clock);
        while (graphics.SelectedItem() != GameMenu::GfxExactGpuReads) graphics.Apply(MenuAction::Down);
        check(!graphics.Settings().graphics.exact_gpu_reads);
        check(graphics.Apply(MenuAction::Confirm) == GameMenuResult::GraphicsChanged &&
              graphics.Settings().graphics.exact_gpu_reads);
        while (graphics.SelectedItem() != GameMenu::GfxTextureMemory) graphics.Apply(MenuAction::Down);
        check(graphics.Apply(MenuAction::Right) == GameMenuResult::GraphicsChanged &&
              graphics.Settings().graphics.advanced.texture_cap_mib == 512);
        // Up and Down stay in the menu's cursor: they change nothing.
        check(graphics.Apply(MenuAction::Down) == GameMenuResult::None &&
              graphics.Settings().graphics.advanced.texture_cap_mib == 512);
        check(graphics.Apply(MenuAction::Back) == GameMenuResult::None);
        check(graphics.CurrentPage() == GameMenu::Page::Quick && graphics.SelectedItem() == GameMenu::GraphicsMenu);

        // The Handheld style already forces the console's mode.
        GameMenuSettings handheld{};
        handheld.style = ConsoleControllerStyle::Handheld;
        GameMenu handheld_menu{handheld, true};
        while (handheld_menu.SelectedItem() != GameMenu::GraphicsMenu) handheld_menu.Apply(MenuAction::Down);
        handheld_menu.Apply(MenuAction::Confirm);
        check(handheld_menu.Rows().size() == 8);

        // Without a title id there is nothing to keep it under.
        GameMenu no_title{{}};
        for (std::size_t i = 0; i < no_title.Rows().size(); ++i) {
            check(no_title.SelectedItem() != GameMenu::GraphicsMenu);
            no_title.Apply(MenuAction::Down);
        }
    }

    // PC: full screen on the display page; the Series menu never shows it.
    GameMenu pc{{ConsoleControllerStyle::Auto, false, 0.12f, true, false}};
    while (pc.SelectedItem() != GameMenu::DisplayPage) pc.Apply(MenuAction::Down);
    check(pc.Apply(MenuAction::Right) == GameMenuResult::None && pc.CurrentPage() == GameMenu::Page::Display);
    check(pc.Rows().size() == 2 && pc.Rows()[1].label == L"Pantalla completa" && pc.Rows()[1].value == L"No");
    pc.Apply(MenuAction::Up);
    check(pc.SelectedItem() == GameMenu::FullScreen && pc.Selected() == 1);
    check(pc.Apply(MenuAction::Confirm) == GameMenuResult::FullScreen && pc.Settings().fullscreen);
    check(pc.Apply(MenuAction::Right) == GameMenuResult::FullScreen && !pc.Settings().fullscreen);
    GameMenu series{{}};
    series.Apply(MenuAction::Down);
    series.Apply(MenuAction::Down);
    series.Apply(MenuAction::Confirm);
    check(series.CurrentPage() == GameMenu::Page::Display && series.Rows().size() == 1);
    for (const auto& row : series.Rows()) check(row.label != L"Pantalla completa");
    // The same menu follows the interface language.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"en"));
    check(pc.PageTitle() == L"Display" && pc.Rows()[1].label == L"Full screen" && pc.Rows()[1].value == L"No");
    check(pc.Apply(MenuAction::Back) == GameMenuResult::None && pc.Rows()[0].label == L"Resume");
    // Both entry points start off and toggle the same persisted setting model.
    SystemPage system_page;
    check(!system_page.settings.show_performance_stats);
    system_page.selected = SystemPage::PerformanceStats;
    check(HandleSystemInput(system_page, SystemInput::Confirm) == SystemPageEffect::Changed);
    check(system_page.settings.show_performance_stats);
    check(HandleSystemInput(system_page, SystemInput::Left) == SystemPageEffect::Changed);
    check(!system_page.settings.show_performance_stats);
    const auto open_display = [&](GameMenu& m) {
        while (m.SelectedItem() != GameMenu::DisplayPage) m.Apply(MenuAction::Down);
        m.Apply(MenuAction::Confirm);
    };
    GameMenu stats_menu{{}};
    check(!stats_menu.Settings().show_performance_stats);
    open_display(stats_menu);
    check(stats_menu.SelectedItem() == GameMenu::PerformanceStats);
    check(stats_menu.Apply(MenuAction::Confirm) == GameMenuResult::PerformanceStatsChanged);
    check(stats_menu.Settings().show_performance_stats);
    auto saved = stats_menu.Settings();
    GameMenu reopened{saved};
    check(reopened.Settings().show_performance_stats);
    check(stats_menu.Apply(MenuAction::Right) == GameMenuResult::PerformanceStatsChanged);
    check(!stats_menu.Settings().show_performance_stats);

    GameMenuSettings diagnostic_settings{};
    diagnostic_settings.material_capture = true;
    GameMenu diagnostic{diagnostic_settings};
    const auto normal_rows = GameMenu{GameMenuSettings{}}.Rows();
    check(diagnostic.Rows().size() == normal_rows.size() + 1);
    while (diagnostic.SelectedItem() != GameMenu::CaptureGraphics) diagnostic.Apply(MenuAction::Down);
    check(diagnostic.Rows()[diagnostic.Selected()].label == L"Capture graphics and resume");
    check(diagnostic.Apply(MenuAction::Left) == GameMenuResult::None);
    check(diagnostic.Apply(MenuAction::Right) == GameMenuResult::None);
    check(diagnostic.Apply(MenuAction::Confirm) == GameMenuResult::CaptureGraphics);
    check(diagnostic.Apply(MenuAction::Back) == GameMenuResult::Resume);
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));
    check(diagnostic.Rows()[diagnostic.Selected()].label == L"Capturar gráficos y continuar");
    for (const auto& row : normal_rows) check(row.label != L"Capture graphics and resume");
    GameMenuSettings hunt_settings{};
    hunt_settings.performance_capture = true;
    GameMenu hunt{hunt_settings};
    check(hunt.Rows().size() == normal_rows.size() + 1);
    while (hunt.SelectedItem() != GameMenu::CapturePerformance) hunt.Apply(MenuAction::Down);
    check(hunt.Rows()[hunt.Selected()].label == L"Capturar rendimiento y continuar");
    check(hunt.Apply(MenuAction::Left) == GameMenuResult::None);
    check(hunt.Apply(MenuAction::Confirm) == GameMenuResult::CapturePerformance);
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"en"));
    check(hunt.Rows()[hunt.Selected()].label == L"Capture performance and resume");
    for (const auto& row : normal_rows) check(row.label != L"Capture performance and resume");
    // The Amiibo row: after the mods, or the cheats, or the first rows; reachable, and it only
    // opens while the game reads tags.
    {
        GameMenu game{GameMenuSettings{}, true};
        const auto without = game.Rows().size();
        game.SetAmiibo(AmiiboReader::Off);
        check(game.Rows().size() == without + 1);
        game.SetAmiibo(AmiiboReader::Looking);
        check(game.Rows().size() == without + 1);
        GameMenu with_mods{GameMenuSettings{}, true};
        with_mods.SetModsOn(2);
        with_mods.SetAmiibo(AmiiboReader::Off);
        while (with_mods.SelectedItem() != GameMenu::Mods) with_mods.Apply(MenuAction::Down);
        with_mods.Apply(MenuAction::Down);
        check(with_mods.SelectedItem() == GameMenu::Amiibo);
        check(with_mods.Rows()[with_mods.Selected()].kind == GameMenuRow::Kind::Info);
        check(with_mods.Rows()[with_mods.Selected()].icon == UiIcon::User);
        check(!with_mods.Rows()[with_mods.Selected()].detail.empty());
        check(with_mods.Apply(MenuAction::Confirm) == GameMenuResult::None);
        check(with_mods.Apply(MenuAction::Right) == GameMenuResult::None);

        with_mods.SetAmiibo(AmiiboReader::Looking);
        check(with_mods.Rows()[with_mods.Selected()].kind == GameMenuRow::Kind::Page);
        check(with_mods.Apply(MenuAction::Left) == GameMenuResult::None);
        check(with_mods.Apply(MenuAction::Right) == GameMenuResult::None);
        check(with_mods.Apply(MenuAction::Confirm) == GameMenuResult::Amiibo);

        with_mods.SetAmiibo(AmiiboReader::Placed, L"Figure");
        check(with_mods.Rows()[with_mods.Selected()].detail == L"Figure");
        check(with_mods.Apply(MenuAction::Confirm) == GameMenuResult::Amiibo);

        GameMenu only_cheats{GameMenuSettings{}, true};
        only_cheats.SetAmiibo(AmiiboReader::Off);
        while (only_cheats.SelectedItem() != GameMenu::Cheats) only_cheats.Apply(MenuAction::Down);
        only_cheats.Apply(MenuAction::Down);
        check(only_cheats.SelectedItem() == GameMenu::Amiibo);
        only_cheats.Apply(MenuAction::Down);
        check(only_cheats.SelectedItem() == GameMenu::ControllerPage);

        // A row nobody asked for stays out of the menu without a game.
        for (const auto& row : GameMenu{GameMenuSettings{}}.Rows()) check(row.icon != UiIcon::User);
    }
    std::puts("game menu: controller combo, pages, navigation, settings, cheats, screenshot, "
              "opt-in capture, amiibo PASS");
}
