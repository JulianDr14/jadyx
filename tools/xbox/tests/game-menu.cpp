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
    check(menu.CurrentPage() == GameMenu::Page::Quick && menu.PageTitle() == L"Menú rápido");
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
    check(menu.Rows().size() == 3 && menu.SelectedItem() == GameMenu::Style && !menu.Rows()[0].icon);
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
    check(menu.Apply(MenuAction::Down) == GameMenuResult::None && menu.SelectedItem() == GameMenu::Style);
    check(menu.Apply(MenuAction::Back) == GameMenuResult::None);
    check(menu.CurrentPage() == GameMenu::Page::Quick && menu.SelectedItem() == GameMenu::ControllerPage);
    check(menu.Rows()[1].detail.find(L"8%") != std::wstring::npos);
    // Presses the menu's own rows ignore.
    for (const auto action : {MenuAction::Alternate, MenuAction::Refresh, MenuAction::PreviousTab,
                              MenuAction::NextTab})
        check(menu.Apply(action) == GameMenuResult::None);

    // A game with a title id offers its cheats after the screenshot, with how many are on.
    GameMenuSettings shot_settings{};
    shot_settings.screenshot = true;
    GameMenu with_cheats{shot_settings, true};
    with_cheats.SetCheatsOn(3);
    check(with_cheats.Rows().size() == 6);
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
    check(with_cheats.Rows().size() == 7 && with_cheats.Rows()[3].kind == GameMenuRow::Kind::Info);
    with_cheats.Apply(MenuAction::Down);
    check(with_cheats.SelectedItem() == GameMenu::Mods && with_cheats.Apply(MenuAction::Confirm) == GameMenuResult::None);
    GameMenu mods_only{{}};
    mods_only.SetModsOn(1);
    check(mods_only.Rows().size() == 5);
    mods_only.Apply(MenuAction::Down);
    check(mods_only.SelectedItem() == GameMenu::Mods);

    // PC: full screen on the display page; the Series menu never shows it.
    GameMenu pc{{ConsoleControllerStyle::Auto, false, 0.12f, true, false}};
    while (pc.SelectedItem() != GameMenu::DisplayPage) pc.Apply(MenuAction::Down);
    check(pc.Apply(MenuAction::Right) == GameMenuResult::None && pc.CurrentPage() == GameMenu::Page::Display);
    check(pc.Rows().size() == 3 && pc.Rows()[2].label == L"Pantalla completa" && pc.Rows()[2].value == L"No");
    pc.Apply(MenuAction::Up);
    check(pc.SelectedItem() == GameMenu::FullScreen && pc.Selected() == 2);
    check(pc.Apply(MenuAction::Confirm) == GameMenuResult::FullScreen && pc.Settings().fullscreen);
    check(pc.Apply(MenuAction::Right) == GameMenuResult::FullScreen && !pc.Settings().fullscreen);
    GameMenu series{{}};
    series.Apply(MenuAction::Down);
    series.Apply(MenuAction::Down);
    series.Apply(MenuAction::Confirm);
    check(series.CurrentPage() == GameMenu::Page::Display && series.Rows().size() == 2);
    for (const auto& row : series.Rows()) check(row.label != L"Pantalla completa");
    // The same menu follows the interface language.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"en"));
    check(pc.PageTitle() == L"Display" && pc.Rows()[2].label == L"Full screen" && pc.Rows()[2].value == L"No");
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
    stats_menu.Apply(MenuAction::Down);
    check(stats_menu.SelectedItem() == GameMenu::PerformanceStats);
    check(stats_menu.Apply(MenuAction::Confirm) == GameMenuResult::PerformanceStatsChanged);
    check(stats_menu.Settings().show_performance_stats);
    auto saved = stats_menu.Settings();
    GameMenu reopened{saved};
    check(reopened.Settings().show_performance_stats);
    check(stats_menu.Apply(MenuAction::Right) == GameMenuResult::PerformanceStatsChanged);
    check(!stats_menu.Settings().show_performance_stats);

    // Image upscaling: first on the display page, a choice that steps both ways and wraps around.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));
    GameMenu scaling_menu{{}};
    open_display(scaling_menu);
    check(scaling_menu.SelectedItem() == GameMenu::ImageUpscaling);
    const auto scaling_row = [&] { return scaling_menu.Rows()[scaling_menu.Selected()]; };
    check(scaling_row().label == L"Escalado de imagen" && scaling_row().value == L"Bilineal");
    check(scaling_row().kind == GameMenuRow::Kind::Choice);
    check(scaling_menu.Apply(MenuAction::Right) == GameMenuResult::UpscalingChanged);
    check(scaling_menu.Settings().upscaling == Upscaling::FsrSharp && scaling_row().value == L"FSR 1 · nítido");
    check(scaling_menu.Apply(MenuAction::Confirm) == GameMenuResult::UpscalingChanged);
    check(scaling_row().value == L"FSR 1 · equilibrado");
    check(scaling_menu.Apply(MenuAction::Left) == GameMenuResult::UpscalingChanged);
    check(scaling_menu.Apply(MenuAction::Left) == GameMenuResult::UpscalingChanged);
    check(scaling_menu.Settings().upscaling == Upscaling::Bilinear);
    check(scaling_menu.Apply(MenuAction::Left) == GameMenuResult::UpscalingChanged);
    check(scaling_menu.Settings().upscaling == Upscaling::FsrSoft && scaling_row().value == L"FSR 1 · suave");
    GameMenu scaling_reopened{scaling_menu.Settings()};
    check(scaling_reopened.Settings().upscaling == Upscaling::FsrSoft);
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"en"));
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
    std::puts("game menu: controller combo, pages, navigation, settings, upscaling, cheats, screenshot, "
              "opt-in capture PASS");
}
