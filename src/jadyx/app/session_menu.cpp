// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <winrt/base.h>
#include <winrt/Windows.Storage.h>

#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/loader/loader.h"
#include "core/memory/cheat_engine.h"
#include "input_common/drivers/virtual_amiibo.h"
#include "video_core/gpu.h"
#include "video_core/performance_overlay.h"
#include "video_core/renderer_base.h"

#include "jadyx/app/boot_diag.h"
#include "jadyx/app/boot_renderer_decls.h"
#include "jadyx/app/boot_settings.h"
#include "jadyx/app/boot_state.h"
#include "jadyx/app/session_menu.h"
#include "jadyx/memory_edit/jme_guest_memory.h"
#include "jadyx/amiibo/amiibo_store.h"
#include "jadyx/cheats/cheats_view.h"
#include "jadyx/game/game_screenshot.h"
#include "jadyx/game/uwp_software_keyboard.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/mods/mod_settings.h"
#include "jadyx/settings/uwp_system_settings.h"
#include "jadyx/storage/utf8.h"
#include "jadyx/ui/text_entry_input.h"

namespace Jadyx {

namespace {

std::size_t CheatsOn(const Cheats::CheatCatalog& catalog) {
    std::size_t count = 0;
    for (const auto category : {Cheats::CheatCategory::Gameplay, Cheats::CheatCategory::Graphics,
                                Cheats::CheatCategory::Mine})
        count += catalog.EnabledCount(category);
    return count;
}

} // namespace

SessionMenu::SessionMenu(const SessionMenuContext& context)
    : SessionMenuContext{context},
      cheat_game{program_id, system.GetApplicationProcessBuildID()} {}

void SessionMenu::CaptureFrame() {
    system.GPU().RunOnGpuThread([&] { game_frame = D3D12::CaptureGameFrame(system.Renderer()); });
    if (menu_surface) menu_surface->SetBackdrop(game_frame);
}

void SessionMenu::DrawMenu() {
    pulse_screen.reset(); // The menu takes the screen.
    launch_screen.reset();
    const auto screen = D3D12::GameMenuScreenSize(system.Renderer());
    D3D12::MenuImage image;
    try {
        menu_surface->SetPromptMapping(input.MenuPromptMapping());
        const GameMenuScene scene{game_title, game_details, session_text};
        image = memory_edit_open ? menu_surface->DrawMemoryEdit(memory_edit->View(), scene, screen)
                : cheats ? menu_surface->DrawCheats({*cheats, cheats_page, game_title, cheats_notice, true},
                                                  scene, screen)
                : amiibo_screen ? menu_surface->DrawAmiibo({amiibo_screen->rows, amiibo_screen->page,
                                                            amiibo_screen->placed, amiibo_screen->has_key,
                                                            amiibo_screen->placed_name, amiibo_screen->placed_new},
                                                           scene, screen)
                : menu ? menu_surface->DrawMenu(*menu, scene, screen)
                       : menu_surface->DrawKeyboard(game_keyboard.View(), game_keyboard.CanCancel(), screen);
    } catch (const winrt::hresult_error& e) {
        WriteDiag("menu: not drawn: " + winrt::to_string(e.message()));
        return;
    }
    system.GPU().RunOnGpuThread([&] { D3D12::ShowGameMenu(system.Renderer(), std::move(image)); });
}

// Shows, redraws or hides the game's keyboard after it changed (unless the menu covers it).
void SessionMenu::SyncKeyboard() {
    const bool open = game_keyboard.IsOpen();
    const bool was_shown = std::exchange(keyboard_shown, open);
    if (menu) return;
    if (open) {
        if (!was_shown) {
            input.SetMenuOpen(true);
            SetTextEntryActive(true);
            WriteDiag("keyboard: shown over the game");
        }
        if (!menu_surface) {
            try {
                menu_surface.emplace();
            } catch (const winrt::hresult_error& e) {
                WriteDiag("keyboard: no surface: " + winrt::to_string(e.message()));
            }
        }
        if (menu_surface) DrawMenu();
    } else if (was_shown) {
        SetTextEntryActive(false);
        menu_surface.reset();
        system.GPU().RunOnGpuThread([&] { D3D12::HideGameMenu(system.Renderer()); });
        input.SetMenuOpen(false);
        WriteDiag("keyboard: closed");
    }
}

void SessionMenu::KeyboardPress(std::optional<KeyboardReply> reply) {
    if (reply && keyboard_applet) keyboard_applet->Reply(*reply);
    SyncKeyboard();
}

void SessionMenu::Open() {
    void(system.Pause());
    input.SetMenuOpen(true);
    SetTextEntryActive(false); // The real keyboard drives the menu.
    const ControllerOptions options = input.Options();
    {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - session_start)
                                 .count();
        wchar_t text[24]{};
        std::swprintf(text, std::size(text), L"%lld:%02lld:%02lld",
                      static_cast<long long>(seconds / 3600),
                      static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
        session_text = text;
    }
    try {
        menu_surface.emplace();
    } catch (const winrt::hresult_error& e) {
        WriteDiag("menu: no surface: " + winrt::to_string(e.message()));
    }
    CaptureFrame();
    const SystemSettings system_settings = LoadSystemSettings();
    menu.emplace(GameMenuSettings{options.style, options.swap_face_buttons,
                                  options.deadzone, g_can_fullscreen.load(),
                                  g_fullscreen.load(), system_settings.show_performance_stats,
                                  pulse_identity.has_value(),
                                  config.material_capture_menu, config.hunt_menu,
                                  !game_frame.pixels.empty(), LoadGameOfflineMode(program_id),
                                  system_settings.offline_mode, LoadGraphicsSettings(),
                                  LoadGameGraphics(program_id),
                                  program_id != 0 && !mii_editor && MemoryEdit::CanSearchGuest(system),
                                  options.right_stick, input.GameRightStickSettings(),
                                  program_id != 0 && !mii_editor},
                 program_id != 0 && !mii_editor);
    if (memory_edit) menu->SetMemoryEditHits(memory_edit->Hits());
    if (game_title.empty()) {
        std::string title;
        void(system.GetAppLoader().ReadTitle(title));
        game_title = Utf8ToUtf16(title);
        FileSys::NACP nacp;
        if (system.GetAppLoader().ReadControlData(nacp) == Loader::ResultStatus::Success) {
            const std::string developer = nacp.GetDeveloperName();
            const std::string version = nacp.GetVersionString();
            game_details = Utf8ToUtf16(developer);
            if (!version.empty())
                game_details += (game_details.empty() ? L"v" : L" · v") + Utf8ToUtf16(version);
        }
    }
    if (program_id != 0 && !mii_editor) {
        menu->SetCheatsOn(CheatsOn(Cheats::LoadCatalog(
            Cheats::TitleLoadDirectory(program_id), cheat_game.build_id)));
        menu->SetModsOn(Mods::ActiveMods(Mods::DefaultModPaths(), program_id));
        using AmiiboState = InputCommon::VirtualAmiibo::State;
        const auto reader = input.Amiibo().GetCurrentState();
        menu->SetAmiibo(reader == AmiiboState::TagNearby          ? AmiiboReader::Placed
                        : reader == AmiiboState::WaitingForAmiibo ? AmiiboReader::Looking
                                                                  : AmiiboReader::Off,
                        reader == AmiiboState::TagNearby
                            ? Utf8ToUtf16(AmiiboDisplayName(input.Amiibo().GetLastFilePath()))
                            : std::wstring{});
    }
    WriteDiag("menu: opened, guest paused | " + MemoryReport());
    if (menu_surface) DrawMenu();
}

void SessionMenu::HandleCheatsAction(MenuAction action) {
    using Cheats::CheatsEffect;
    const auto pressed = Cheats::CheatsInputForAction(action);
    if (!pressed) return;
    cheats_notice.clear();
    const CheatsEffect effect = Cheats::HandleCheatsInput(cheats_page, *cheats, *pressed,
                                                          GameMenuSurface::CheatRows);
    if (effect == CheatsEffect::Changed) {
        // Hot: the engine (created now if the game booted without cheats) runs
        // the new list from its next tick.
        system.EnsureCheatEngine().Reload(cheats->Catalog().Program());
        WriteDiag("menu: cheats changed, " + std::to_string(cheats->Catalog().UsedWords()) +
                  " VM words");
        if (!cheats->Saved()) cheats_notice = I18n::Tr(I18n::Text::CheatsSaveFailed);
    } else if (effect == CheatsEffect::OverBudget) {
        cheats_notice = I18n::Tr(I18n::Text::CheatsOverBudget);
    } else if (effect == CheatsEffect::Refresh) {
        cheats->Retry();
    } else if (effect == CheatsEffect::Back) {
        menu->SetCheatsOn(CheatsOn(cheats->Catalog()));
        cheats.reset();
    }
    if (menu_surface) DrawMenu();
}

void SessionMenu::SavePulseSession() {
    // The only capture: when the player leaves, never while playing.
    WritePulseLog("saving the session | " + MemoryReport() + " | " + MemoryOwners());
    pulse_screen.emplace(system, PulseScreen::Kind::Save, game_title);
    const auto save_start = std::chrono::steady_clock::now();
    // Samples the app memory while the save runs: the highest use of each
    // step against the use before it, so the step that peaks is known.
    constexpr std::array<const char*, 7> save_steps{
        "idle", "process", "gpu", "write", "verify", "sync", "replace"};
    std::array<std::atomic<u64>, save_steps.size()> step_peak{};
    std::atomic<int> step{-1};
    u64 save_base{}, save_limit{};
    QueryAppMemory(save_base, save_limit);
    std::string reason;
    {
        std::jthread sampler{[&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                const int at = step.load(std::memory_order_relaxed);
                if (u64 used{}, limit{}; at >= 0 && QueryAppMemory(used, limit)) {
                    auto& peak = step_peak[at];
                    u64 seen = peak.load(std::memory_order_relaxed);
                    while (used > seen && !peak.compare_exchange_weak(seen, used)) {
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }};
        reason = PulseSessions::Save(
            system, *pulse_identity,
            [] {
                u64 used{}, limit{};
                if (!QueryAppMemory(used, limit)) return ~u64{};
                return limit > used ? limit - used : u64{};
            },
            [&](const char* name) {
                pulse_screen->SaveStep(name);
                for (int i = 0; i < static_cast<int>(save_steps.size()); ++i) {
                    if (std::string_view{name} == save_steps[i]) step.store(i);
                }
            });
    }
    std::string peaks = "save peak over " + std::to_string(save_base >> 20) +
                        " MiB:";
    for (std::size_t i = 0; i < save_steps.size(); ++i) {
        const u64 peak = step_peak[i].load();
        if (peak == 0) continue;
        peaks += std::string{" "} + save_steps[i] + " " +
                 (peak >= save_base ? "+" + std::to_string((peak - save_base) >> 20)
                                    : "-" + std::to_string((save_base - peak) >> 20));
    }
    WritePulseLog(peaks + " MiB");
    const auto save_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - save_start)
                             .count();
    if (reason.empty()) {
        WritePulseLog("session saved in " + std::to_string(save_ms) + " ms | " +
                      MemoryReport() + " | " + MemoryOwners());
        PulseSessions::SetNotice(I18n::ToUtf8(I18n::Tr(I18n::Text::PulseKept)));
    } else {
        WritePulseLog("no session (" + reason + ") after " +
                  std::to_string(save_ms) + " ms");
        PulseSessions::SetNotice(I18n::ToUtf8(
            I18n::Format(I18n::Text::PulseNotKept, {Utf8ToUtf16(reason)})));
    }
    pulse_screen->Finish(reason.empty());
}

void SessionMenu::OpenCheats() {
    cheats.emplace(cheat_game);
    cheats->Poll();
    cheats_page = {};
    Cheats::ClampCheatsPage(cheats_page, cheats->Catalog(), GameMenuSurface::CheatRows);
    cheats_notice.clear();
}

void SessionMenu::OpenAmiibo() {
    const AmiiboStore store;
    AmiiboScreen screen;
    screen.has_key = store.HasKey();
    screen.placed = input.Amiibo().GetCurrentState() == InputCommon::VirtualAmiibo::State::TagNearby;
    if (screen.placed) {
        screen.placed_name = Utf8ToUtf16(AmiiboDisplayName(input.Amiibo().GetLastFilePath()));
        screen.placed_new = input.Amiibo().IsNewFigure();
        screen.rows.push_back(I18n::Format(I18n::Text::AmiiboRemoveNamed, {screen.placed_name}));
        screen.paths.emplace_back();
    } else {
        for (const AmiiboFile& file : store.List()) {
            screen.rows.push_back(Utf8ToUtf16(AmiiboDisplayName(file.name)));
            screen.paths.push_back(store.PathOf(file.name));
            if (screen.paths.back() == amiibo_last) screen.page.selected = screen.paths.size() - 1;
        }
        ClampListWindow(screen.page.selected, screen.page.first, screen.rows.size(), GameMenuSurface::AmiiboRows);
    }
    WriteDiag("menu: amiibo page, " + std::to_string(screen.rows.size()) + " row(s), key " +
              (screen.has_key ? "yes" : "no") + (screen.placed_new ? ", placed as new figure" : ""));
    amiibo_screen = std::move(screen);
}

bool SessionMenu::HandleAmiiboAction(MenuAction action) {
    const AmiiboEffect effect = HandleAmiiboInput(amiibo_screen->page, action, amiibo_screen->rows.size(),
                                                  GameMenuSurface::AmiiboRows, amiibo_screen->placed);
    if (effect == AmiiboEffect::Choose) {
        amiibo_choice = AmiiboChoice{amiibo_screen->paths[amiibo_screen->page.selected],
                                     !amiibo_screen->placed && amiibo_screen->page.new_figure};
        Close(GameMenuResult::Resume);
        return true;
    }
    if (effect == AmiiboEffect::Back) amiibo_screen.reset();
    if (effect != AmiiboEffect::None && menu_surface) DrawMenu();
    return false;
}

void SessionMenu::SaveMenuScreenshot() {
    try {
        const auto folder = std::filesystem::path{std::wstring{
                                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}} /
                            L"jadyx" / L"captures";
        if (const auto name = SaveScreenshot(game_frame, folder)) {
            menu->SetScreenshotNote(I18n::Format(I18n::Text::MenuScreenshotSaved, {*name}));
            WriteDiag("menu: screenshot saved as " + I18n::ToUtf8(*name));
        } else {
            menu->SetScreenshotNote(std::wstring{I18n::Tr(I18n::Text::MenuScreenshotFailed)});
            WriteDiag("menu: screenshot could not be saved");
        }
    } catch (const winrt::hresult_error& e) {
        menu->SetScreenshotNote(std::wstring{I18n::Tr(I18n::Text::MenuScreenshotFailed)});
        WriteDiag("menu: screenshot failed: " + winrt::to_string(e.message()));
    }
}

void SessionMenu::Close(GameMenuResult result) {
    if (std::exchange(graphics_changed, false)) {
        // On closing rather than on each press: the player may be stepping through
        // the choices, and a resize waits for everything in flight.
        GraphicsSettings graphics = ResolveGraphics(LoadGraphicsSettings(), menu->Settings().graphics);
        if (config.upscaling) graphics.upscaling = *config.upscaling;
        if (config.frame_gen) graphics.frame_gen = *config.frame_gen != D3D12::FrameGenMode::Off;
        const bool screen = UsesScreenResolution(graphics);
        const u32 width = screen ? surface.screen_width : surface.plain_width;
        const u32 height = screen ? surface.screen_height : surface.plain_height;
        if (width != 0 && std::pair{width, height} != D3D12::GameMenuScreenSize(system.Renderer())) {
            bool resized{};
            system.GPU().RunOnGpuThread([&] {
                resized = D3D12::ResizeGameOutput(system.Renderer(), width, height);
                if (resized) emu_window.UpdateCurrentFramebufferLayout(width, height);
            });
            WriteDiag("menu: output " + std::to_string(width) + "x" + std::to_string(height) +
                      (resized ? "" : " failed, the size stays until the next boot") + " | " +
                      MemoryReport());
        }
    }
    if (memory_edit) memory_edit->Stop();
    memory_edit_open = false;
    menu.reset();
    cheats.reset();
    amiibo_screen.reset();
    game_frame = {};
    if (game_keyboard.IsOpen() && menu_surface) {
        // The game's keyboard was under the menu: it comes back.
        SetTextEntryActive(true);
        DrawMenu();
    } else {
        menu_surface.reset();
        system.GPU().RunOnGpuThread([&] { D3D12::HideGameMenu(system.Renderer()); });
        input.SetMenuOpen(false);
        keyboard_shown = false;
    }
    // Arm after hiding/re-presenting the menu, so only guest presents consume
    // the two capture intervals. Reuse the same bounded request as F7.
    if (result == GameMenuResult::CaptureGraphics) {
        D3D12::TraceNextMaterialFrame();
        WriteDiag("menu: bounded graphics capture requested for next guest frames");
    }
    if (result == GameMenuResult::CapturePerformance) {
        D3D12::HuntNextFrames();
        WriteDiag("menu: bottleneck capture requested for next guest frames");
    }
    void(system.Run());
    WriteDiag("menu: closed, guest running");
    // With the guest running, as a tag signals the game's events.
    if (const auto choice = std::exchange(amiibo_choice, std::nullopt)) {
        const bool remove = choice->path.empty();
        const auto info =
            remove ? input.Amiibo().CloseAmiibo() : input.Amiibo().LoadAmiibo(choice->path, choice->new_figure);
        if (!remove) amiibo_last = choice->path;
        const char* what = remove ? "removed" : choice->new_figure ? "placed as new figure" : "placed";
        WriteDiag(std::string{"menu: amiibo "} + what + ", result " + std::to_string(static_cast<int>(info)));
    }
}

void SessionMenu::ApplyChoice(GameMenuResult result) {
    if (result == GameMenuResult::SettingsChanged) {
        ControllerOptions options = input.Options();
        options.style = menu->Settings().style;
        options.swap_face_buttons = menu->Settings().swap_face_buttons;
        options.deadzone = menu->Settings().deadzone;
        input.SetOptions(options);
    }
    if (result == GameMenuResult::RightStickChanged) {
        const GameMenuSettings& settings = menu->Settings();
        if (settings.right_stick_per_game) {
            if (!SaveGameRightStick(program_id, settings.right_stick))
                WriteDiag("menu: right stick settings could not be saved");
            input.SetGameRightStick(settings.right_stick);
        } else {
            ControllerOptions options = input.Options();
            options.right_stick = settings.right_stick_global;
            input.SetOptions(options);
        }
    }
    if (result == GameMenuResult::PerformanceStatsChanged) {
        auto settings = LoadSystemSettings();
        settings.show_performance_stats = menu->Settings().show_performance_stats;
        if (!SaveSystemSettings(settings)) WriteDiag("menu: statistics preference could not be saved");
        VideoCore::performance_overlay_enabled.store(settings.show_performance_stats, std::memory_order_relaxed);
    }
    if (result == GameMenuResult::FullScreen) {
        g_fullscreen_request.store(menu->Settings().fullscreen ? 1 : 0);
    }
    if (result == GameMenuResult::GraphicsChanged) {
        const GameGraphics& game = menu->Settings().graphics;
        const GameGraphics before = LoadGameGraphics(program_id);
        if (!SaveGameGraphics(program_id, game)) WriteDiag("menu: graphics could not be saved");
        ApplyGameGraphics(system, config, game);
        ApplyRunningAdvancedGraphics(system, before.advanced, game.advanced);
        graphics_changed = true;
    }
    if (result == GameMenuResult::OfflineModeChanged) {
        const auto offline = menu->Settings().offline_mode;
        if (!SaveGameOfflineMode(program_id, offline)) WriteDiag("menu: offline mode could not be saved");
        // Seen by the game's next network query; boot.cfg's airplane=1 still wins.
        if (!config.airplane_mode)
            Settings::values.airplane_mode.SetValue(offline.value_or(menu->Settings().offline_default));
        WriteDiag(std::string{"menu: offline mode "} +
                  (Settings::values.airplane_mode.GetValue() ? "on" : "off"));
    }
}

SessionMenu::Outcome SessionMenu::Update() {
    if (keyboard_applet) {
        bool changed = false;
        for (const KeyboardEvent& event : keyboard_applet->TakeEvents())
            changed |= game_keyboard.Apply(event);
        if (changed || game_keyboard.IsOpen() != keyboard_shown) SyncKeyboard();
        for (const TextEntryKey& key : TakeTextEntryKeys()) {
            if (menu || !game_keyboard.IsOpen()) break;
            KeyboardPress(key.command ? game_keyboard.Handle(*key.command)
                                       : game_keyboard.Type(key.character));
        }
    }
    for (const MenuAction action : input.TakeMenuActions()) {
        if (!menu && action != MenuAction::Toggle && game_keyboard.IsOpen()) {
            if (const auto command = TextEntryCommand(action))
                KeyboardPress(game_keyboard.Handle(*command));
            continue;
        }
        if (!menu) {
            if (action != MenuAction::Toggle) continue;
            Open();
            continue;
        }
        if (cheats && action != MenuAction::Toggle) {
            HandleCheatsAction(action);
            continue;
        }
        if (amiibo_screen && action != MenuAction::Toggle) {
            if (HandleAmiiboAction(action)) break;
            continue;
        }
        if (memory_edit_open && action != MenuAction::Toggle) {
            const auto reply = memory_edit->Handle(action);
            if (reply == MemoryEdit::Screen::Reply::Leave) {
                memory_edit_open = false;
                menu->SetMemoryEditHits(memory_edit->Hits());
            }
            if (reply != MemoryEdit::Screen::Reply::Stay && menu_surface) DrawMenu();
            continue;
        }
        const GameMenuResult result = menu->Apply(action);
        WriteDiag("menu: action " + std::to_string(static_cast<int>(action)) + ", row " +
                  std::to_string(menu->Selected()));
        if (result == GameMenuResult::Library ||
            result == GameMenuResult::LibraryWithoutSession) {
            memory_edit.reset();
            if (result == GameMenuResult::Library && pulse_identity) SavePulseSession();
            WriteDiag("menu: back to the library, shutting the game down | " +
                      MemoryReport());
            return Outcome::BackToLibrary;
        }
        if (result == GameMenuResult::Cheats) OpenCheats();
        if (result == GameMenuResult::Amiibo) OpenAmiibo();
        if (result == GameMenuResult::Screenshot) SaveMenuScreenshot();
        if (result == GameMenuResult::MemoryEdit) {
            if (!memory_edit) memory_edit.emplace(system);
            memory_edit->Open();
            memory_edit_open = true;
        }
        if (result == GameMenuResult::Resume || result == GameMenuResult::CaptureGraphics ||
            result == GameMenuResult::CapturePerformance) {
            Close(result);
            break;
        }
        ApplyChoice(result);

        if (menu_surface) DrawMenu();
    }
    // Cheats the database sent while the page is open join the list.
    if (cheats && cheats->Poll()) {
        Cheats::ClampCheatsPage(cheats_page, cheats->Catalog(), GameMenuSurface::CheatRows);
        if (menu_surface) DrawMenu();
    }
    if (memory_edit_open && memory_edit->Tick() && menu_surface) DrawMenu();
    return Outcome::Stay;
}

} // namespace Jadyx
