// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"

#include <memory>

#include "jadyx/amiibo/amiibo_store.h"

namespace Jadyx {
using namespace winrt::Windows::UI::Core;
namespace {
double ReadOversize(const CoreWindow& window, unsigned width) {
    const auto bounds = window.Bounds();
    const auto dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
    const double window_width = bounds.Width * dpi;
    return window_width >= 1.0 ? std::max(1.0, width / window_width) : 1.0;
}

std::uint64_t ReadAppMemoryLimit() {
    std::uint64_t app_memory_limit{};
    try { app_memory_limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit(); } catch (...) {}
    LOG_INFO(Frontend, "Library: app memory limit {} MiB ({} mode)", app_memory_limit >> 20,
             IsAppModeMemory(app_memory_limit) ? "App" : "Game");
    return app_memory_limit;
}

PortalAmiibo AmiiboForPortal() {
    const auto store = std::make_shared<AmiiboStore>();
    return {
        .list = [store] {
            std::vector<PortalAmiiboFile> files;
            for (const AmiiboFile& file : store->List()) files.push_back({file.name, file.bytes});
            return files;
        },
        .has_key = [store] { return store->HasKey(); },
        .add = [store](const std::string& name, const std::string& bytes, bool replace) {
            return store->Add(name, bytes, replace);
        },
        .remove = [store](const std::string& name) { return store->Remove(name); },
        .set_key = [store](const std::string& bytes) { return store->SetKey(bytes); },
        .remove_key = [store] { return store->RemoveKey(); },
    };
}
} // namespace

LibrarySession::LibrarySession(void* window_abi, unsigned swap_width, unsigned swap_height,
                               const std::filesystem::path& library_root, const std::function<void()>& seed_files,
                               std::string_view pick, std::string_view first_notice, std::uint64_t* program_id_out)
    : core_window{window_abi}, width{swap_width}, height{swap_height}, root{library_root}, seed{seed_files},
      auto_pick{pick}, initial_notice{first_notice}, selected_program_id{program_id_out},
      window{CoreWindow::GetForCurrentThread()},
      canvas{std::make_unique<LibraryCanvas>(core_window, width, height)},
      oversize{ReadOversize(window, width)}, app_memory_limit{ReadAppMemoryLimit()},
      app_mode{IsAppModeMemory(app_memory_limit)} {
    network_portal.SetLogDirectory(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir));
    network_portal.SetLibrary(portal_library.View());
    network_portal.SetAmiibo(AmiiboForPortal());
    network_portal.SetUpdatePackage([&] { return updater.PrepareAssistedDownload(); });
    network_suspending = winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending(
        winrt::auto_revoke, [&](const auto&, const winrt::Windows::ApplicationModel::SuspendingEventArgs&) {
            updater.Pause();
            network_portal.Stop();
        });
    system_page.updated = updater.CurrentPhase() == Updates::Phase::Confirmed;
    system_page.settings = LoadSystemSettings();
    graphics_page.settings = LoadGraphicsSettings();
    system_page.locale = DetectSystemLocale();
    title_language = ApplySystemSettings().game_language;
    panel.xbox = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
    panel.keyboard.bindings = LoadKeyboardBindings();
    play_order = system_page.settings.track_play_time;
    size_changed = window.SizeChanged(winrt::auto_revoke, [&](auto&&, WindowSizeChangedEventArgs const& e) {
        const auto scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        width = std::max(1U, static_cast<unsigned>(e.Size().Width * scale * oversize + 0.5));
        height = std::max(1U, static_cast<unsigned>(e.Size().Height * scale * oversize + 0.5));
        resize = true;
    });
    closed = window.Closed(winrt::auto_revoke, [&](auto&&, CoreWindowEventArgs const&) { window_closed = true; });
    visible = window.VisibilityChanged(winrt::auto_revoke, [&](auto&&, VisibilityChangedEventArgs const&) { dirty = true; });
    activation = window.Activated(winrt::auto_revoke, [&](auto&&, WindowActivatedEventArgs const& e) {
        if (e.WindowActivationState() == CoreWindowActivationState::Deactivated) {
            panel.keyboard.capturing = panel.pad_map.capturing = false; dirty = true;
        }
    });
    // Typing on a real keyboard while the name editor is open; KeyDown handles the rest.
    characters = window.CharacterReceived(winrt::auto_revoke, [&](auto&&, CharacterReceivedEventArgs const& e) {
        if (!system_page.name_editor) return;
        system_page.name_editor->Type(static_cast<wchar_t>(e.KeyCode()));
        system_page.editor_notice.clear();
        dirty = true;
        e.Handled(true);
    });
    keyboard = window.KeyDown(winrt::auto_revoke, [this](auto&&, KeyEventArgs const& e) { OnKeyDown(e); });
    pointer = window.PointerPressed(winrt::auto_revoke, [this](auto&&, PointerEventArgs const& e) { OnPointerPressed(e); });
    wheel = window.PointerWheelChanged(winrt::auto_revoke, [this](auto&&, PointerEventArgs const& e) { OnWheel(e); });
    StartScan();
}

std::optional<std::string> LibrarySession::Run() {
    while (true) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        const auto now = std::chrono::steady_clock::now();
        network_portal.SetUploadsAvailable(!configuration.busy && !picker && !validation.valid() &&
            !game_options.open && !content_session && !settings && !system_page.open &&
            updater.CurrentPhase() != Updates::Phase::Downloading && updater.CurrentPhase() != Updates::Phase::Verifying &&
            updater.CurrentPhase() != Updates::Phase::Preparing && updater.CurrentPhase() != Updates::Phase::Requested);
        PollNetwork(now);
        ExpireNotices(now);
        UpdateControllers(now);
        ReadPad(now);
        ReadBattery(now);
        if (window_closed || QuitRequested()) return std::nullopt;
        if (app_mode) {
            if (actions & Quit) return std::nullopt;
            actions = 0;
        }
        if (actions && system_page.updated) { system_page.updated = false; dirty = true; }
        HandleImportConfirm();
        HandleNetworkScreen();
        if (finish_name_editor) {
            ApplySystemEffect(FinishNameEditor(system_page, *finish_name_editor), now);
            finish_name_editor.reset();
            dirty = true;
        }
        HandlePicker();
        if (HandleMissingContent()) return launch;
        if (PollValidation()) return launch;
        PollSetup();
        PollImport();
        // Windows can replace us after accepting a deployment. Do not launch a game or
        // begin another storage operation while the old process is waiting to close.
        if (updater.CurrentPhase() == Updates::Phase::Preparing || updater.CurrentPhase() == Updates::Phase::Requested) {
            actions = 0;
            pressed_tab.reset();
        }
        HandleUpdatesPage();
        HandleUpdateNotice();
        // Leaving the update page to use another feature pauses network work before dispatching it.
        if (!system_page.updates_open && actions &&
            (updater.CurrentPhase() == Updates::Phase::Downloading || updater.CurrentPhase() == Updates::Phase::Verifying)) updater.Pause();
        const bool top_level = !settings && !system_page.open && !graphics_page.open && !configuration.files && !configuration.busy &&
                               !picker && !validation.valid() && !game_options.open && !missing_content;
        HandlePressedTab(top_level);
        HandleTabBumpers(top_level);
        if (HandleSystemPage(now)) return launch;
        HandleGraphicsPage(now);
        const bool on_game = tab == LibraryTab::Home ||
                             (tab == LibraryTab::Library && !filter_focus && shelf_focus < shelf.size());
        OpenGameOptions(on_game);
        if (game_options.open) {
            // A rescan may have replaced the list under the page.
            if (!content_session && game_options.game >= scan.entries.size()) {
                game_options = {};
                cheat_session.reset();
                mod_session.reset();
                actions = 0; dirty = true;
                continue;
            }
            HandleGameOptions(now);
        }
        if (!game_options.open && !content_session) {
            mod_session.reset(); cheat_session.reset(); requested_manager_tab.reset();
        }
        if (HandleQuit()) return launch;
        HandlePageShortcuts();
        HandleConfigurationPage();
        if (!settings) panel.notice.clear();
        if (!panel.keyboard.open) panel.keyboard.notice.clear();
        if (!panel.pad_map.open) panel.pad_map.notice.clear();
        FinishScan();
        AutoPick();
        StartValidation(on_game);
        if (settings) HandleControllerPanel();
        else HandleNavigation();
        HandlePin(on_game);
        SaveControllerChoices(now);
        if (actions & Refresh && !loading && !picker && !validation.valid()) StartScan();
        MergeMetadata();
        ScheduleMetadata();
        actions = 0;
        if (resize) {
            canvas.reset();
            canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
            resize = false;
            dirty = true;
        }
        const bool draw = dirty || canvas->Animating();
        if (draw) Draw();
        // A drawn frame already waited for the display in Present(1); sleeping after it too made
        // every press answer a frame late. Idle, poll the pad every half frame.
        if (!draw) std::this_thread::sleep_for(std::chrono::milliseconds{8});
    }
}

} // namespace Jadyx
