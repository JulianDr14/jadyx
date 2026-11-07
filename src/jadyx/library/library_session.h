// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The library screen while it is up: its state and one method per screen. ShowGameLibrary
// (uwp_library.cpp) builds one and runs it. Private to the library_session*.cpp files.

#include "jadyx/input/stick_navigation.h"
#include "jadyx/input/controller_catalog.h"
#include "jadyx/ui/text_entry_input.h"
#include "jadyx/library/favorites.h"
#include "jadyx/library/game_removal.h"
#include "jadyx/library/library_layout.h"
#include "jadyx/library/uwp_library_canvas.h"
#include "jadyx/storage/uwp_rom_storage.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/storage_location.h"
#include "jadyx/storage/uwp_file_manager.h"
#include "jadyx/storage/nsp_import.h"
#include "jadyx/storage/portal_fault_log.h"
#include "jadyx/storage/network_portal.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.Pickers.h>
#include "jadyx/cheats/cheat_session.h"
#include "jadyx/cheats/cheats_page.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/app_mode.h"
#include "jadyx/library/game_options_page.h"
#include "jadyx/content/content_view.h"
#include "jadyx/mods/mod_session.h"
#include "jadyx/mods/mod_web.h"
#include "jadyx/mods/mods_page.h"
#include "jadyx/mods/mods_view.h"
#include "jadyx/library/play_history.h"
#include "jadyx/library/portal_library.h"
#include "jadyx/pulse/pulse_sessions.h"
#include "jadyx/library/library_cache.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/settings/graphics_page.h"
#include "jadyx/settings/system_page.h"
#include "jadyx/updates/update_ui.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/library/uwp_library_metadata.h"
#include "jadyx/settings/uwp_system_settings.h"
#include <fmt/format.h>
#include "common/logging.h"
#include "common/fs/path_util.h"
#include "core/file_sys/common_funcs.h"

namespace Jadyx {

class LibrarySession {
public:
    LibrarySession(void* window_abi, unsigned swap_width, unsigned swap_height,
                   const std::filesystem::path& library_root, const std::function<void()>& seed_files,
                   std::string_view pick, std::string_view first_notice, std::uint64_t* program_id_out);
    LibrarySession(const LibrarySession&) = delete;
    LibrarySession& operator=(const LibrarySession&) = delete;

    /// Until the player picks something or leaves: see ShowGameLibrary.
    std::optional<std::string> Run();

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    using T = I18n::Text;
    using GamepadButtons = winrt::Windows::Gaming::Input::GamepadButtons;
    using VirtualKey = winrt::Windows::System::VirtualKey;
    using StorageFolder = winrt::Windows::Storage::StorageFolder;
    using LibraryTab = LibraryLayout::LibraryTab;
    using Filter = LibraryLayout::Filter;
    enum : unsigned { Up = 1, Down = 2, Play = 4, Quit = 8, Swap = 16, Zone = 32,
                      Refresh = 64, Panel = 128, Left = 256, Right = 512, AddFolder = 1024,
                      Options = 2048, PreviousTab = 4096, NextTab = 8192, Pin = 16384, PreviousManagerTab = 32768, NextManagerTab = 65536 };
    // What the storage worker runs: one at a time, never alongside a manager or the metadata reader.
    enum class StorageJob { Files, PortalGame, PortalSystem, PortalContent, PortalRemoval, Removal };
    // The entries whose covers the current tab may show, nearest first, and a key that changes
    // when they do.
    struct Covers {
        std::vector<size_t> order;
        std::vector<bool> shown;
        size_t key{};
    };
    struct MetadataBatch {
        std::vector<LibraryEntry> entries;
        unsigned generation{};
        bool icons{};      // The window around the cursor.
        bool stopped{};    // Interrupted: some entries were not read.
    };

    static std::wstring Wide(std::string_view utf8) {
        return winrt::to_hstring(utf8).c_str();
    }

    // library_session_scan.cpp: the games on the shelf and what is known about them.
    void RefreshSetup();
    void ReloadMetadata();
    void ForgetContent(std::uint64_t application);
    void RefreshPlay(std::string_view keep);
    void RefreshPulse();
    Covers CoversForTab();
    void StartScan(StorageFolder added = nullptr);
    void PollSetup();
    void FinishScan();
    void AutoPick();
    void StartValidation(bool on_game);
    bool HandleMissingContent();
    bool PollValidation();
    void MergeMetadata();
    void ScheduleMetadata();

    // library_session_input.cpp: keys, the pointer and the controller become `actions`.
    void SaveKeyboard();
    void SavePadMap(bool reset);
    /// Lists the controllers to map; with only one there is nothing to choose.
    void OpenPadPicker();
    void BuildPadPicker();
    void WatchPadPicker();
    void OpenPadMap(const ControllerDevice& device, size_t number);
    void ClearBinding();
    void RestoreBindings();
    void RefuseBinding(BindingEditor& editor, T reason, size_t action);
    /// Saves a changed pad mapping, or stays on what it lacks.
    void CloseBindingEditor();
    void BeginCapture();
    void OnKeyDown(winrt::Windows::UI::Core::KeyEventArgs const& e);
    void OnPointerPressed(winrt::Windows::UI::Core::PointerEventArgs const& e);
    bool PressDialog(float x, float y);
    bool PressGameOptions(float x, float y);
    bool PressUpdatePage(float x, float y);
    bool PressHeader(float x, float y);
    bool PressConfiguration(float x, float y);
    bool PressKeyboardEditor(float x, float y);
    bool PressPadPicker(float x, float y);
    void PressControllerPanel(float x, float y);
    void PressLibrary(float x, float y);
    void OnWheel(winrt::Windows::UI::Core::PointerEventArgs const& e);
    void ExpireNotices(TimePoint now);
    void UpdateControllers(TimePoint now);
    /// The controller being mapped, or the picker's: what isn't the primary pad's own input.
    void ReadMapTarget(TimePoint now);
    void ReadPadPicker();
    void WorkPadPicker(unsigned directions, GamepadButtons edges);
    void WorkBindingEditor(BindingEditor& editor, unsigned directions, GamepadButtons edges);
    void ReadPad(TimePoint now);
    void ReadBattery(TimePoint now);

    // library_session_network.cpp: the portal, the updater and the storage worker.
    void ReportPortalFault(std::string_view what, PortalError error, const PortalAddress& address,
                           unsigned short port, int os_error);
    PortalAddress RefreshAddresses();
    bool StartPortal();
    void OpenUpdateGuide();
    void PollNetwork(TimePoint now);
    void StartStorageJob(StorageJob next, const PortalStatus& state);
    void HandleImportConfirm();
    void HandleNetworkScreen();
    void PollImport();
    void HandleUpdatesPage();
    void HandleUpdateNotice();

    // library_session_pages.cpp: tabs, Settings' pages, the controller panel and the shelves.
    void ApplySystemEffect(SystemPageEffect effect, TimePoint now);
    void HandlePicker();
    void SwitchTab(LibraryTab to);
    void HandlePressedTab(bool top_level);
    void HandleTabBumpers(bool top_level);
    bool HandleSystemPage(TimePoint now);
    void HandleGraphicsPage(TimePoint now);
    bool HandleQuit();
    void HandlePageShortcuts();
    void HandleConfigurationPage();
    void OpenHomeCategory();
    void ActivateFilesRow();
    void HandleControllerPanel();
    void HandleNavigation();
    void HandlePin(bool on_game);
    void SaveControllerChoices(TimePoint now);
    void Draw();

    // library_session_game_options.cpp: a game's options and the managers inside them.
    void Notify(TimePoint now, T text);
    void OpenGameOptions(bool on_game);
    void HandleGameOptions(TimePoint now);
    void ActivateGameOption(TimePoint now);
    void HandleContentPane();
    void HandleModsPane(TimePoint now);
    void HandleCheatsPane(TimePoint now);
    void HandleGameGraphicsPane(TimePoint now);
    void HandlePlainOption(TimePoint now);

    void* core_window;
    unsigned width;
    unsigned height;
    const std::filesystem::path& root;
    const std::function<void()>& seed;
    std::string_view auto_pick;
    std::string_view initial_notice;
    std::uint64_t* selected_program_id;
    /// What Run returns once a method says the library is done (nullopt: the app ends).
    std::optional<std::string> launch;

    winrt::Windows::UI::Core::CoreWindow window;
    std::unique_ptr<LibraryCanvas> canvas;
    // With output=4k the swapchain is larger than the window (3840x2160 for a 1920x1080 window);
    // resizes and pointer positions keep that ratio.
    const double oversize;
    // In App mode (Dev Home's app type) the console leaves about 1 GiB, in Game mode about 5:
    // no game fits the first, so the library only asks to change it.
    std::uint64_t app_memory_limit;
    const bool app_mode;
    LibraryScan scan;
    size_t selected = 0;
    bool rail = false; // The focus is in the played games' rail (library_layout.h).
    // The screen's tabs; Settings is the configuration panel, shown as a page.
    LibraryTab tab = LibraryTab::Home;
    Filter filter = Filter::All;
    std::vector<size_t> shelf;   // The Library tab's entries (LibraryLayout::Shelf).
    size_t shelf_focus = 0;      // In `shelf`; shelf.size() is the import tile.
    bool filter_focus = false;   // The Library tab's focus is on the filter bar.
    Favorites favorites = LoadFavorites();
    WORD shown_minute = 0xFFFF;  // The header's clock.
    ControllerBattery battery;   // The header's battery, read every few seconds.
    TimePoint battery_read_at{};
    bool dirty = true, loading = true, settings = false;
    ConfigurationPanel configuration;
    Updates::Updater updater;
    // Declared before the portal: its workers read this view until the portal is destroyed.
    PortalLibrarySource portal_library{root, DefaultLibraryCacheDirectory()};
    NetworkPortal network_portal{root,DefaultSystemImportBackend()};
    PortalFaultLog portal_faults{Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir)};
    PortalAddress portal_address;   // Where the running portal listens.
    TimePoint portal_check_at{};
    TimePoint network_poll_at{};
    // Revoked before the library-owned portal is destroyed. No background server survives play.
    winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending_revoker network_suspending;
    SystemPage system_page;
    // The selected game's options (Menu), and its cheats page while open.
    GameOptionsPage game_options;
    std::optional<std::size_t> requested_game_option;
    std::optional<std::size_t> requested_manager_tab;
    std::unique_ptr<Content::ContentSession> content_session;
    bool missing_content = false;
    std::optional<Cheats::CheatSession> cheat_session;
    Cheats::CheatsPage cheats_page;
    std::unique_ptr<Mods::ModSession> mod_session;
    Mods::ModsPage mods_page;
    std::wstring options_notice;
    GraphicsPage graphics_page;
    // The selected game's own graphics, for its Graphics pane; loaded for `game_graphics_id`.
    GameGraphics game_graphics;
    std::size_t game_graphics_row{};
    std::uint64_t game_graphics_id{};
    // Titles are read in the game language, so the library shows what the games will.
    Settings::Language title_language{};
    const LibraryCache cache{DefaultLibraryCacheDirectory()};
    bool return_to_configuration = false;
    unsigned picker_kind = 0; // 0 games, 1 keys, 2 firmware
    std::future<FileSetupStatus> setup_future;
    std::jthread setup_worker;
    FileImportProgress import_progress;
    std::future<std::string> import_future;
    std::jthread import_worker;
    StorageJob storage_job{};
    std::optional<GameFiles> removal_request; ///< Confirmed in the game's options.
    bool removed_copy{}; ///< That removal keeps another copy of the game.
    std::wstring after_scan; ///< Shown when the rescan that follows a storage job completes.
    /// Set for the rescan after a storage job that leaves keys and firmware alone: it keeps the
    /// titles already read. The value is the game whose content changed (0: none).
    std::optional<std::uint64_t> keep_titles;
    std::chrono::steady_clock::time_point auto_pick_at{};
    unsigned setting_row = 0;
    ControllerPanel panel;
    ControllerOptions options = LoadControllerOptions();
    TimePoint capture_until{};
    std::wstring notice; // Library scan/metadata messages only.
    TimePoint keyboard_notice_until{};
    TimePoint controller_notice_until{};
    TimePoint system_notice_until{};
    TimePoint options_notice_until{};
    // Set by input handlers when the name editor closes: true keeps the name, false discards it.
    std::optional<bool> finish_name_editor;
    bool play_order{}; // What the list is ordered by.
    bool device_changed = false;
    size_t retained_page = SIZE_MAX;
    bool metadata_dirty = true;
    size_t names_cursor = 0; // Next game whose title is looked up in the cache, after a scan.
    size_t fill_cursor = 0;  // Next game the background fill reads into the cache.
    unsigned actions = 0;
    std::optional<LibraryTab> pressed_tab; // A tab the pointer chose.
    winrt::Windows::Foundation::IAsyncOperation<StorageFolder> picker{nullptr};
    std::future<std::string> validation;
    std::jthread validation_worker;
    std::string selected_path;
    bool resize = false, window_closed = false;
    winrt::Windows::UI::Core::CoreWindow::SizeChanged_revoker size_changed;
    winrt::Windows::UI::Core::CoreWindow::Closed_revoker closed;
    winrt::Windows::UI::Core::CoreWindow::VisibilityChanged_revoker visible;
    winrt::Windows::UI::Core::CoreWindow::Activated_revoker activation;
    winrt::Windows::UI::Core::CoreWindow::CharacterReceived_revoker characters;
    winrt::Windows::UI::Core::CoreWindow::KeyDown_revoker keyboard;
    winrt::Windows::UI::Core::CoreWindow::PointerPressed_revoker pointer;
    winrt::Windows::UI::Core::CoreWindow::PointerWheelChanged_revoker wheel;
    std::future<LibraryScan> future;
    std::jthread scanner;
    bool metadata_background = false; // The running job is a names or fill batch.
    std::future<MetadataBatch> metadata;
    std::jthread metadata_worker;
    unsigned generation = 0;
    bool logging_ready = false;
    bool open_mii_editor = false;
    GamepadButtons previous{};
    StickNavigation stick_navigation;
    StickNavigation manager_dpad{std::chrono::milliseconds{280}, std::chrono::milliseconds{80}};
    TriggerTabs trigger_tabs;
    unsigned navigation_context = 0;
    std::optional<ControllerDevice> pad;
    // What the mapping editor edits, not always `pad`: that one also works it, and so does this.
    std::optional<ControllerDevice> map_target;
    GamepadButtons target_previous{};
    StickNavigation target_stick;
    ControllerCatalog<ControllerDevice> controllers;
    // 8080, or one of the next ports when another app holds it or the console refuses it.
    static constexpr unsigned short portal_port = 8080;
    static constexpr unsigned portal_spare_ports = 4;
};

} // namespace Jadyx
