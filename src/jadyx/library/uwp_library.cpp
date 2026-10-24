// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/uwp_library.h"
#include "jadyx/input/stick_navigation.h"
#include "jadyx/input/controller_catalog.h"
#include "jadyx/ui/text_entry_input.h"
#include "jadyx/library/favorites.h"
#include "jadyx/library/game_removal.h"
#include "jadyx/library/library_layout.h"
#include "jadyx/library/uwp_library_canvas.h"
#include "jadyx/storage/uwp_rom_storage.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/uwp_file_manager.h"
#include "jadyx/storage/nsp_import.h"
#include "jadyx/storage/portal_fault_log.h"

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
#include "jadyx/settings/system_page.h"
#include "jadyx/updates/update_ui.h"
#include "jadyx/input/uwp_input.h"
#include "jadyx/library/uwp_library_metadata.h"
#include "jadyx/settings/uwp_system_settings.h"
#include <fmt/format.h>
#include "common/logging.h"
#include "common/fs/path_util.h"
#include "core/file_sys/common_funcs.h"
#include "core/core.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace Jadyx {
namespace {
using winrt::Windows::Gaming::Input::Gamepad;
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::System::VirtualKey;
using I18n::Tr;
using T = I18n::Text;

/// Background batches stay short so a page change never waits long behind them: games opened
/// through the loaders per fill batch, cache records read per names batch.
constexpr size_t FillBatch = 6;
constexpr size_t NamesBatch = 64;

std::wstring Wide(std::string_view utf8) {
    return winrt::to_hstring(utf8).c_str();
}
} // namespace

std::optional<std::string> ShowGameLibrary(void* core_window, unsigned width, unsigned height,
                                         const std::filesystem::path& root,
                                         const std::function<void()>& seed,
                                         std::string_view auto_pick, std::string_view initial_notice) {
    using namespace winrt::Windows::UI::Core;
    auto window = CoreWindow::GetForCurrentThread();
    auto canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
    LibraryScan scan;
    size_t selected = 0;
    bool rail = false; // The focus is in the played games' rail (library_layout.h).
    // The screen's tabs; Settings is the configuration panel, shown as a page.
    using LibraryLayout::LibraryTab;
    using LibraryLayout::Filter;
    LibraryTab tab = LibraryTab::Home;
    Filter filter = Filter::All;
    std::vector<size_t> shelf;   // The Library tab's entries (LibraryLayout::Shelf).
    size_t shelf_focus = 0;      // In `shelf`; shelf.size() is the import tile.
    bool filter_focus = false;   // The Library tab's focus is on the filter bar.
    Favorites favorites = LoadFavorites();
    WORD shown_minute = 0xFFFF;  // The header's clock.
    ControllerBattery battery;   // The header's battery, read every few seconds.
    auto battery_read_at = std::chrono::steady_clock::time_point{};
    bool dirty = true, loading = true, settings = false;
    ConfigurationPanel configuration;
    Updates::Updater updater;
    // Declared before the portal: its workers read this view until the portal is destroyed.
    PortalLibrarySource portal_library{root, DefaultLibraryCacheDirectory()};
    NetworkPortal network_portal{root,DefaultSystemImportBackend()};
    network_portal.SetLogDirectory(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir));
    PortalFaultLog portal_faults{Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir)};
    PortalAddress portal_address;   // Where the running portal listens.
    auto portal_check_at = std::chrono::steady_clock::time_point{};
    network_portal.SetLibrary(portal_library.View());
    network_portal.SetUpdatePackage([&] { return updater.PrepareAssistedDownload(); });
    auto network_poll_at = std::chrono::steady_clock::time_point{};
    // Revoked before the library-owned portal is destroyed. No background server survives play.
    auto network_suspending = winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending(
        winrt::auto_revoke, [&](const auto&, const winrt::Windows::ApplicationModel::SuspendingEventArgs&) {
            updater.Pause();
            network_portal.Stop();
        });
    SystemPage system_page;
    system_page.updated = updater.CurrentPhase() == Updates::Phase::Confirmed;
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
    system_page.settings = LoadSystemSettings();
    system_page.locale = DetectSystemLocale();
    // Titles are read in the game language, so the library shows what the games will.
    Settings::Language title_language = ApplySystemSettings().game_language;
    const LibraryCache cache{DefaultLibraryCacheDirectory()};
    bool return_to_configuration = false;
    unsigned picker_kind = 0; // 0 games, 1 keys, 2 firmware
    std::future<FileSetupStatus> setup_future;
    std::jthread setup_worker;
    FileImportProgress import_progress;
    std::future<std::string> import_future;
    std::jthread import_worker;
    // What the storage worker runs: one at a time, never alongside a manager or the metadata reader.
    enum class StorageJob { Files, PortalGame, PortalSystem, PortalContent, PortalRemoval, Removal };
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
    panel.xbox = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
    ControllerOptions options = LoadControllerOptions();
    if (panel.xbox) options.controller_id.clear();
    panel.keyboard.bindings = LoadKeyboardBindings();
    auto capture_until = std::chrono::steady_clock::time_point{};
    std::wstring notice; // Library scan/metadata messages only.
    auto keyboard_notice_until = std::chrono::steady_clock::time_point{};
    auto controller_notice_until = std::chrono::steady_clock::time_point{};
    auto system_notice_until = std::chrono::steady_clock::time_point{};
    auto options_notice_until = std::chrono::steady_clock::time_point{};
    // Set by input handlers when the name editor closes: true keeps the name, false discards it.
    std::optional<bool> finish_name_editor;
    auto save_keyboard = [&] {
        const bool saved = SaveKeyboardBindings(panel.keyboard.bindings);
        panel.keyboard.notice = Tr(saved ? T::KeyboardSaved : T::KeyboardSaveFailed);
        keyboard_notice_until = saved ? std::chrono::steady_clock::now() + std::chrono::seconds{3} :
                                       std::chrono::steady_clock::time_point::max();
        dirty = true;
    };
    // The Pulse session each game keeps, as the tiles and the options show it.
    auto refresh_pulse = [&] {
        PulseSessions::Prune();
        for (auto& entry : scan.entries) {
            const auto kept = PulseSessions::Find(entry.launch_path);
            entry.pulse_saved = kept ? PulseSessions::FormatTime(kept->saved) : std::wstring{};
            entry.pulse_current = kept && kept->current;
        }
        dirty = true;
    };
    bool play_order = system_page.settings.track_play_time; // What the list is ordered by.
    auto begin_capture = [&] {
        panel.keyboard.notice.clear();
        panel.keyboard.capturing = true;
        capture_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
        dirty = true;
    };
    bool device_changed = false;
    size_t retained_page = SIZE_MAX;
    bool metadata_dirty = true;
    size_t names_cursor = 0; // Next game whose title is looked up in the cache, after a scan.
    size_t fill_cursor = 0;  // Next game the background fill reads into the cache.
    unsigned actions = 0;
    std::optional<LibraryTab> pressed_tab; // A tab the pointer chose.
    enum : unsigned { Up = 1, Down = 2, Play = 4, Quit = 8, Swap = 16, Zone = 32,
                      Refresh = 64, Panel = 128, Left = 256, Right = 512, AddFolder = 1024,
                      Options = 2048, PreviousTab = 4096, NextTab = 8192, Pin = 16384, PreviousManagerTab = 32768, NextManagerTab = 65536 };
    using winrt::Windows::Storage::StorageFolder;
    winrt::Windows::Foundation::IAsyncOperation<StorageFolder> picker{nullptr};
    std::future<std::string> validation;
    std::jthread validation_worker;
    std::string selected_path;
    bool resize = false, window_closed = false;
    auto size_changed = window.SizeChanged(winrt::auto_revoke, [&](auto&&, WindowSizeChangedEventArgs const& e) {
        const auto scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        width = std::max(1U, static_cast<unsigned>(e.Size().Width * scale));
        height = std::max(1U, static_cast<unsigned>(e.Size().Height * scale));
        resize = true;
    });
    auto closed = window.Closed(winrt::auto_revoke, [&](auto&&, CoreWindowEventArgs const&) { window_closed = true; });
    auto visible = window.VisibilityChanged(winrt::auto_revoke, [&](auto&&, VisibilityChangedEventArgs const&) { dirty = true; });
    auto activation = window.Activated(winrt::auto_revoke, [&](auto&&, WindowActivatedEventArgs const& e) {
        if (e.WindowActivationState() == CoreWindowActivationState::Deactivated) {
            panel.keyboard.capturing = false; dirty = true;
        }
    });
    // Typing on a real keyboard while the name editor is open; KeyDown handles the rest.
    auto characters = window.CharacterReceived(winrt::auto_revoke, [&](auto&&, CharacterReceivedEventArgs const& e) {
        if (!system_page.name_editor) return;
        system_page.name_editor->Type(static_cast<wchar_t>(e.KeyCode()));
        system_page.editor_notice.clear();
        dirty = true;
        e.Handled(true);
    });
    auto keyboard = window.KeyDown(winrt::auto_revoke, [&](auto&&, KeyEventArgs const& e) {
        if (e.KeyStatus().WasKeyDown && e.VirtualKey() != VirtualKey::Up &&
            e.VirtualKey() != VirtualKey::Down && e.VirtualKey() != VirtualKey::Left &&
            e.VirtualKey() != VirtualKey::Right) return;
        if (system_page.name_editor) {
            using Command = TextEntry::Command;
            std::optional<Command> command;
            switch (e.VirtualKey()) {
            case VirtualKey::Escape: command = Command::Cancel; break;
            case VirtualKey::Enter: command = Command::Done; break;
            case VirtualKey::Back: command = Command::Delete; break;
            case VirtualKey::Up: command = Command::Up; break;
            case VirtualKey::Down: command = Command::Down; break;
            case VirtualKey::Left: command = Command::Left; break;
            case VirtualKey::Right: command = Command::Right; break;
            default: break; // Characters arrive through CharacterReceived.
            }
            if (command) {
                const auto result = system_page.name_editor->Apply(*command);
                if (result != TextEntry::Result::None) finish_name_editor = result == TextEntry::Result::Done;
            }
            dirty = true;
            e.Handled(true);
            return;
        }
        auto& editor = panel.keyboard;
        if (editor.open) {
            const auto key = e.VirtualKey();
            if (editor.capturing) {
                if (key != VirtualKey::Escape) {
                    if (AssignKeyboardKey(editor.bindings, editor.selected, static_cast<unsigned>(key))) save_keyboard();
                    else {
                        editor.notice = Tr(T::KeyUnsupported);
                        keyboard_notice_until = std::chrono::steady_clock::time_point::max();
                    }
                }
                editor.capturing = false;
                dirty = true;
            } else if (key == VirtualKey::Escape) { editor.open = false; dirty = true; }
            else if (key == VirtualKey::Tab) {
                editor.page = (editor.page + 1) % 3; editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
            } else if (key == VirtualKey::Enter) begin_capture();
            else if (key == VirtualKey::Delete) {
                AssignKeyboardKey(editor.bindings, editor.selected, 0); save_keyboard();
            } else if (key == VirtualKey::Up || key == VirtualKey::Down ||
                       key == VirtualKey::Left || key == VirtualKey::Right) {
                const auto items = KeyboardPageActions(editor.page);
                const size_t count = items.size();
                const size_t index = static_cast<size_t>(std::find(items.begin(), items.end(), editor.selected) - items.begin());
                const size_t step = key == VirtualKey::Up || key == VirtualKey::Down ? 2 : 1;
                const bool reverse = key == VirtualKey::Up || key == VirtualKey::Left;
                editor.selected = items[(index + (reverse ? count - step : step)) % count];
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        switch (e.VirtualKey()) {
        case VirtualKey::Up: actions |= Up; break;
        case VirtualKey::Down: actions |= Down; break;
        case VirtualKey::Left: actions |= Left; break;
        case VirtualKey::Right: actions |= Right; break;
        case VirtualKey::F1: actions |= Panel; break;
        case VirtualKey::Escape: actions |= Quit; break;
        case VirtualKey::Enter: actions |= Play; break;
        case VirtualKey::X: actions |= Swap; break;
        case VirtualKey::R: actions |= Refresh; break;
        case VirtualKey::F: actions |= Pin; break;
        case VirtualKey::O: actions |= AddFolder; break;
        case VirtualKey::F2: actions |= Options; break;
        case VirtualKey::PageUp: actions |= PreviousManagerTab; break;
        case VirtualKey::PageDown: actions |= NextManagerTab; break;
        case VirtualKey::Q: actions |= PreviousTab; break;
        case VirtualKey::E: actions |= NextTab; break;
        default: break;
        }
        e.Handled(true);
    });
    auto pointer = window.PointerPressed(winrt::auto_revoke, [&](auto&&, PointerEventArgs const& e) {
        const auto p = e.CurrentPoint().Position();
        const auto dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        const auto scale = std::max(0.01f, std::min(width / 1280.0f, height / 720.0f));
        const float x = static_cast<float>((p.X * dpi - (width - 1280 * scale) / 2) / scale);
        const float y = static_cast<float>((p.Y * dpi - (height - 720 * scale) / 2) / scale);
        if (configuration.network_screen || configuration.confirm_import) {
            const auto hit = canvas->HitAt(x, y);
            if (hit.hit == LibraryHit::NetworkAction && hit.index < 3) {
                configuration.network_action = hit.index;
                actions |= Play;
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (system_page.name_editor) {
            using L = SystemPageLayout;
            if (const auto key = TextEntryPanelLayout::KeyAt(x - L::EditorX, y - L::EditorY)) {
                auto& editor = *system_page.name_editor;
                editor.Focus(key->first, key->second);
                if (editor.Press() == TextEntry::Result::Done) finish_name_editor = true;
                system_page.editor_notice.clear();
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (game_options.open && y < 94) {
            const auto target = canvas->HitAt(x, y);
            if (target.hit == LibraryHit::Tab) {
                pressed_tab = static_cast<LibraryTab>(target.index);
                dirty = true;
            } else if (target.hit == LibraryHit::Controller && !game_options.page && !content_session) {
                game_options = {};
                actions |= Panel;
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (game_options.open && canvas->HitAt(x, y).hit == LibraryHit::GameOptionSelect) {
            const auto target = canvas->HitAt(x, y);
            if (target.hit == LibraryHit::GameOptionSelect && target.index < GameOptions().size()) {
                requested_game_option = target.index;
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (game_options.open) {
            const auto target = canvas->HitAt(x, y);
            if (target.hit == LibraryHit::ManagerTab) {
                requested_manager_tab = target.index;
                game_options.focus = GameOptionsFocus::ManagerTabs;
                dirty = true;
            } else if (target.hit == LibraryHit::ManagerRow) {
                if (game_options.page == GameOption::Mods && mod_session) {
                    const auto count = mods_page.tab == Mods::ModsTab::Installed ?
                        mod_session->View().mods.size() : mod_session->View().files.size();
                    if (target.index < count) { mods_page.selected = target.index; actions |= Play; }
                } else if (game_options.page == GameOption::Cheats && cheat_session && target.index < cheat_session->Catalog().Cheats(cheats_page.category).size()) {
                    cheats_page.selected = target.index; actions |= Play;
                }
                game_options.focus = GameOptionsFocus::Content;
                dirty = true;
            } else if (target.hit == LibraryHit::ManagerCard && mod_session &&
                       target.index < mod_session->Store().cards.size()) {
                mods_page.card = target.index;
                game_options.focus = GameOptionsFocus::Content;
                actions |= Play; dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (system_page.updates_open) {
            const auto hit = canvas->HitAt(x, y);
            if (hit.hit == LibraryHit::UpdateAction) {
                if (hit.index == UpdateBackHit) actions |= Quit;
                else { system_page.update_row = hit.index; actions |= Play; }
                dirty = true;
            }
            e.Handled(true); return;
        }
        // The new-version notice: its prompts do what Y and X do while it shows.
        if (const auto hit = canvas->HitAt(x, y); hit.hit == LibraryHit::UpdateNotice) {
            actions |= hit.index == 0 ? Pin : Refresh;
            dirty = true;
            e.Handled(true); return;
        }
        // The header's tabs and controller work over every page that is not a dialog.
        if ((!settings || configuration.open) && !panel.keyboard.open && !panel.expanded && !game_options.open &&
            !missing_content && !configuration.busy) {
            const auto target = canvas->HitAt(x, y);
            if (target.hit == LibraryHit::Tab) {
                pressed_tab = static_cast<LibraryTab>(target.index);
                e.Handled(true);
                return;
            }
            if (target.hit == LibraryHit::Controller && !configuration.open) {
                actions |= Panel;
                e.Handled(true);
                return;
            }
        }
        if (configuration.open && !content_session && !panel.keyboard.open && !panel.expanded) {
            const auto target = canvas->HitAt(x, y);
            const bool in_page = system_page.open || configuration.files || settings;
            const size_t shown = system_page.open ? HomeSystem : configuration.files ? HomeFiles :
                                 settings ? HomeControllers : configuration.selected;
            auto category = target.hit == LibraryHit::Category ? std::optional<size_t>{target.index} :
                            target.hit == LibraryHit::Controller ? std::optional<size_t>{HomeControllers} :
                            std::nullopt;
            // A row of the category the list has focused opens it, like A would.
            if (target.hit == LibraryHit::SettingRow && !in_page) category = shown;
            if (configuration.busy || picker != nullptr) {
            } else if (category && !(in_page && *category == shown)) {
                // Another category: leave the open page and enter it.
                system_page.open = configuration.files = settings = return_to_configuration = false;
                panel.notice.clear();
                configuration.selected = *category;
                actions |= Play;
                dirty = true;
            } else if (target.hit == LibraryHit::SettingRow) {
                if (system_page.open) system_page.selected = target.index;
                else if (configuration.files) configuration.selected = target.index;
                else setting_row = static_cast<unsigned>(target.index);
                actions |= Play;
                dirty = true;
            }
            e.Handled(true);
            return;
        }
        if (panel.keyboard.open) {
            auto& editor = panel.keyboard;
            if (Contains({1120, 112, 48, 40}, x, y)) {
                editor.open = editor.capturing = false; dirty = true;
            } else if (!editor.capturing) {
                if (Contains({635, 203, 514, 36}, x, y)) {
                    editor.page = std::min(2U, static_cast<unsigned>((x - 635) / 174));
                    editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
                } else if (Contains({635, 603, 155, 36}, x, y)) {
                    AssignKeyboardKey(editor.bindings, editor.selected, 0); save_keyboard();
                } else if (Contains({806, 603, 170, 36}, x, y)) {
                    editor.bindings = DefaultKeyboardBindings; save_keyboard();
                } else {
                    for (size_t i = 0; i < KeyboardButtonCount; ++i) {
                        if (Contains(KeyboardDiagramTile(i), x, y)) {
                            editor.page = KeyboardPageFor(i); editor.selected = i; begin_capture(); break;
                        }
                    }
                    for (const size_t i : KeyboardPageActions(editor.page)) {
                        if (!editor.capturing && Contains(KeyboardTileFor(i), x, y)) {
                            editor.selected = i; begin_capture(); break;
                        }
                    }
                }
            }
            e.Handled(true); return;
        }
        if (settings) {
            if (panel.expanded) {
                const size_t first = ControllerChoiceFirst(panel);
                if (x >= 648 && x < 1178 && y >= ControllerChoiceTop) {
                    const size_t i = first + static_cast<size_t>((y - ControllerChoiceTop) / ControllerChoiceHeight);
                    if (i < panel.devices.size() + 2 && i < first + ControllerVisibleChoices) {
                        panel.choice = i;
                        actions |= Play;
                    } else actions |= Quit;
                } else actions |= Quit;
            } else if (!panel.xbox && Contains({648, 510, 530, 34}, x, y)) {
                panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
            } else if (x < 620 || x > 1206 || y < 133 || y > 620) actions |= Panel;
            else if (x >= 648 && x < 1178 && y >= ControllerRowTop &&
                     y < ControllerRowTop + (panel.xbox ? 3 : 4) * ControllerRowHeight) {
                setting_row = static_cast<unsigned>((y - ControllerRowTop) / ControllerRowHeight);
                actions |= Play;
            }
        } else if (const auto target = canvas->HitAt(x, y); !loading) {
            switch (target.hit) {
            case LibraryHit::Play: actions |= Play; break;
            case LibraryHit::Options: actions |= Options; break;
            case LibraryHit::Filter:
                filter = static_cast<Filter>(target.index);
                filter_focus = true;
                shelf = LibraryLayout::Shelf(scan.entries, filter);
                shelf_focus = 0;
                dirty = true;
                break;
            case LibraryHit::RailTile:
            case LibraryHit::Tile:
                if (target.index < scan.entries.size()) {
                    selected = target.index;
                    rail = target.hit == LibraryHit::RailTile;
                    dirty = true;
                }
                break;
            case LibraryHit::ShelfTile:
                if (target.index < shelf.size()) {
                    // A second press on the focused game plays it.
                    if (!filter_focus && shelf_focus == target.index) actions |= Play;
                    shelf_focus = target.index;
                    selected = shelf[target.index];
                    filter_focus = false;
                    dirty = true;
                }
                break;
            case LibraryHit::Import:
                shelf_focus = shelf.size();
                filter_focus = false;
                actions |= Play;
                dirty = true;
                break;
            default: break;
            }
        }
        e.Handled(true);
    });
    auto wheel = window.PointerWheelChanged(winrt::auto_revoke, [&](auto&&, PointerEventArgs const& e) {
        if (configuration.network_screen) {
            const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
            if (delta) { actions |= delta > 0 ? Left : Right; dirty = true; }
            e.Handled(true);
            return;
        }
        if (game_options.open && !configuration.open) {
            const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
            if (delta) { actions |= delta > 0 ? Up : Down; dirty = true; }
            e.Handled(true);
            return;
        }

        if (settings && panel.expanded && !panel.keyboard.open) {
            const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
            const size_t count = panel.devices.size() + 2;
            if (delta > 0 && panel.choice) --panel.choice;
            if (delta < 0 && panel.choice + 1 < count) ++panel.choice;
            dirty = true;
            e.Handled(true);
        }
    });
    std::future<LibraryScan> future;
    std::jthread scanner;
    struct MetadataBatch {
        std::vector<LibraryEntry> entries;
        unsigned generation{};
        bool icons{};      // The window around the cursor.
        bool stopped{};    // Interrupted: some entries were not read.
    };
    bool metadata_background = false; // The running job is a names or fill batch.
    std::future<MetadataBatch> metadata;
    std::jthread metadata_worker;
    unsigned generation = 0;
    bool logging_ready = false;
    auto refresh_setup = [&] {
        if (setup_future.valid()) return;
        std::packaged_task<FileSetupStatus()> task{[] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            return ReadFileSetupStatus();
        }};
        setup_future = task.get_future();
        setup_worker = std::jthread(std::move(task));
    };
    // Forget what the entries know and read it again (cache first): after the game language
    // changed or the cache was cleared. Results in flight belong to the old generation.
    auto reload_metadata = [&] {
        ++generation;
        metadata_worker.request_stop();
        for (auto& entry : scan.entries) {
            entry.metadata_loaded = entry.title_loaded = false;
            entry.display_version.clear();
            std::vector<unsigned char>{}.swap(entry.icon);
        }
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        names_cursor = fill_cursor = 0;
        dirty = true;
    };
    // A game's updates/DLC changed (0: any game's): only its entries read their metadata again.
    // Its cache record no longer matches the catalog, so the fill rereads it from the files.
    auto forget_content = [&](std::uint64_t application) {
        metadata = {}; ++generation;
        for (auto& entry : scan.entries) {
            if (application && FileSys::GetBaseTitleID(entry.program_id) != application) continue;
            entry.title_loaded = entry.metadata_loaded = false;
            entry.build_id = {}; entry.display_version.clear();
        }
        metadata_dirty = true; names_cursor = fill_cursor = 0;
    };
    // Each game's play time and last play, and the library's order (play_history.h). `keep` stays
    // selected where it lands; otherwise the first game (the last one played) is.
    auto refresh_play = [&](std::string_view keep) {
        const auto history = LoadPlayHistory();
        for (auto& entry : scan.entries) {
            const auto found = history.find(entry.launch_path);
            entry.play_seconds = found == history.end() ? 0 : found->second.seconds;
            entry.last_played = found == history.end() ? 0 : found->second.last_played;
            entry.pinned = favorites.contains(entry.launch_path);
        }
        play_order = system_page.settings.track_play_time;
        OrderLibrary(scan.entries, history, play_order);
        // Pinned games lead, in the order the history gives them.
        std::stable_partition(scan.entries.begin(), scan.entries.end(), [](const auto& e) { return e.pinned; });
        selected = 0;
        for (size_t i = 0; i < scan.entries.size(); ++i)
            if (!keep.empty() && scan.entries[i].launch_path == keep) selected = i;
        // The last game played opens in the rail, where the played games are.
        rail = selected < LibraryLayout::RailCount(scan.entries);
        shelf = LibraryLayout::Shelf(scan.entries, filter);
        shelf_focus = 0;
        for (size_t i = 0; i < shelf.size(); ++i)
            if (shelf[i] == selected) shelf_focus = i;
        // Covers and background reads follow positions: start them over for the new order.
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        names_cursor = fill_cursor = 0;
        dirty = true;
    };
    // The entries whose covers the current tab may show, nearest first, and a key that changes
    // when they do.
    struct Covers {
        std::vector<size_t> order;
        std::vector<bool> shown;
        size_t key{};
    };
    auto covers_for_tab = [&] {
        Covers result;
        result.shown.assign(scan.entries.size(), false);
        const auto add = [&](size_t index) {
            if (index < scan.entries.size() && !result.shown[index]) {
                result.shown[index] = true;
                result.order.push_back(index);
            }
        };
        if (tab == LibraryTab::Library) {
            using LibraryLayout::Columns;
            const size_t row = std::min(shelf_focus, shelf.empty() ? 0 : shelf.size() - 1) / Columns;
            const size_t first = (row > 0 ? row - 1 : 0) * Columns;
            const size_t end = std::min(shelf.size(), (row + 3) * Columns);
            // The focused row, the ones below, then the one above.
            for (size_t i = row * Columns; i < end; ++i) add(shelf[i]);
            for (size_t i = first; i < std::min(end, row * Columns); ++i) add(shelf[i]);
            result.key = (first + end * 65536) * 16 + 8 + static_cast<size_t>(filter);
        } else {
            const auto window = LibraryLayout::CoversAround({selected, rail}, scan.entries.size(),
                                                            LibraryLayout::RailCount(scan.entries));
            const size_t row = rail ? 0 : selected / LibraryLayout::Columns * LibraryLayout::Columns;
            for (size_t i = row; i < std::min(row + LibraryLayout::Columns, window.end); ++i) add(i);
            for (size_t i = row + LibraryLayout::Columns; i < window.end; ++i) add(i);
            for (size_t i = 0; i < window.rail; ++i) add(i);
            for (size_t i = window.first; i < row; ++i) add(i);
            // Rows 0 and 1 share a first entry: the key is both ends of the window.
            result.key = (window.first + window.end * 65536) * 16;
        }
        return result;
    };
    auto start_scan = [&](StorageFolder added = nullptr) {
        loading = dirty = true;
        ++generation;
        metadata_dirty = true;
        retained_page = SIZE_MAX;
        names_cursor = fill_cursor = 0;
        metadata_worker.request_stop();
        canvas->InvalidateCovers();
        std::packaged_task<LibraryScan(std::stop_token)> task{[&, added](std::stop_token stop) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            seed();
            // Recover interrupted publication before a metadata worker constructs NCA readers.
            ReadFileSetupStatus();
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            auto result = ScanGameLibrary(root, stop);
            // The filesystem's own text is English whatever the interface language: log it only.
            if (!result.error.empty()) {
                LOG_WARNING(Frontend, "Library scan: internal folder: {}", result.error);
                result.error = I18n::ToUtf8(Tr(T::LibraryScanFailed));
            }
            const std::wstring internal{Tr(T::SourceInternal)};
            for (auto& entry : result.entries) {
                const auto path = (root / entry.relative_path).u8string();
                entry.launch_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
                entry.source_name = internal;
                // When the file arrived: copies keep the source's write time but not its creation.
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (GetFileAttributesExW((root / entry.relative_path).c_str(), GetFileExInfoStandard, &data)) {
                    const auto ticks = (static_cast<std::uint64_t>(data.ftCreationTime.dwHighDateTime) << 32) |
                                       data.ftCreationTime.dwLowDateTime;
                    // 100 ns ticks since 1601 to Unix seconds.
                    if (ticks > 116444736000000000ULL)
                        entry.added = static_cast<std::int64_t>((ticks - 116444736000000000ULL) / 10000000ULL);
                }
            }
            try {
                if (added) RememberGameFolder(added);
                auto external = ScanExternalGameFolders(stop);
                if (!external.error.empty()) {
                    if (!result.error.empty()) result.error += " | ";
                    result.error += external.error;
                }
                result.limited |= external.limited;
                result.entries.insert(result.entries.end(),
                    std::make_move_iterator(external.entries.begin()), std::make_move_iterator(external.entries.end()));
            } catch (const winrt::hresult_error& e) {
                result.error = I18n::ToUtf8(I18n::Format(T::FolderAuthFailed, {e.message()}));
            } catch (const std::exception& e) {
                LOG_WARNING(Frontend, "Library scan: external folders: {}", e.what());
                result.error = I18n::ToUtf8(Tr(T::LibraryScanFailed));
            }
            std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
                return a.launch_path < b.launch_path;
            });
            return result;
        }};
        future = task.get_future();
        scanner = std::jthread(std::move(task));
    };
    // Saves and applies the System page; the interface switches language on the next frame.
    auto apply_system_effect = [&](SystemPageEffect effect, std::chrono::steady_clock::time_point now) {
        switch (effect) {
        case SystemPageEffect::Changed:
        case SystemPageEffect::NameDone: {
            const bool saved = SaveSystemSettings(system_page.settings);
            const auto resolved = ApplySystemSettings();
            system_page.notice = Tr(saved ? T::SettingsSaved : T::SystemSaveFailed);
            system_notice_until = saved ? now + std::chrono::seconds{3} : std::chrono::steady_clock::time_point::max();
            if (resolved.game_language != title_language) {
                title_language = resolved.game_language;
                reload_metadata();
            }
            if (system_page.settings.track_play_time != play_order && !loading)
                refresh_play(selected < scan.entries.size() ? scan.entries[selected].launch_path : std::string{});
            break;
        }
        case SystemPageEffect::OpenUpdates:
            system_page.updates_open = true; system_page.update_row = 0; break;
        case SystemPageEffect::ClearCache: {
            reload_metadata();
            canvas->InvalidateCovers();
            // The mods store's pictures, index and unfinished downloads go with it.
            std::error_code ec;
            std::filesystem::remove_all(Mods::Web::CacheFolder(), ec);
            system_page.notice = Tr(cache.Clear() ? T::LibraryCacheCleared : T::SystemSaveFailed);
            system_notice_until = now + std::chrono::seconds{3};
            break;
        }
        case SystemPageEffect::None:
        case SystemPageEffect::EditName:
            break;
        }
        dirty = true;
    };
    start_scan();
    GamepadButtons previous{};
    StickNavigation stick_navigation;
    StickNavigation manager_dpad{std::chrono::milliseconds{280}, std::chrono::milliseconds{80}};
    TriggerTabs trigger_tabs;
    unsigned navigation_context = 0;
    std::optional<ControllerDevice> pad;
    ControllerCatalog<ControllerDevice> controllers;
    // 8080, or one of the next ports when another app holds it or the console refuses it.
    constexpr unsigned short portal_port = 8080;
    constexpr unsigned portal_spare_ports = 4;
    // Every fault is kept in jx_portal with the adapters of the moment. The user reads what
    // happened, or that the network itself needs a look once it keeps failing.
    const auto portal_fault = [&](std::string_view what, PortalError error, const PortalAddress& address,
                                  unsigned short port, int os_error) {
        const auto failures = portal_faults.Record({.what = what, .error = error, .address = address, .port = port,
                                                    .spare_ports = portal_spare_ports, .os_error = os_error});
        const std::wstring ip{winrt::to_hstring(address.ipv4)};
        const auto first = std::to_wstring(port), last = std::to_wstring(port + portal_spare_ports);
        std::wstring message;
        if (failures >= PortalFaultLog::RepeatedFailures)
            message = I18n::Format(T::NetworkRepeatedFailures, {std::to_wstring(failures)});
        else if (what == "stopped" && error == PortalError::AddressLost)
            message = I18n::Format(T::NetworkPortalLost, {ip});
        else switch (error) {
            case PortalError::NoAddress: message = Tr(T::NetworkUnavailable); break;
            case PortalError::PortInUse: message = I18n::Format(T::NetworkPortInUse, {first, last}); break;
            case PortalError::PortBlocked: message = I18n::Format(T::NetworkPortBlocked, {first, last}); break;
            case PortalError::AddressLost:
                message = I18n::Format(address.link == PortalLink::WiFi ? T::NetworkWifiLost : T::NetworkAddressLost, {ip});
                break;
            case PortalError::Bind: message = I18n::Format(T::NetworkBindFailed, {first, std::to_wstring(os_error)}); break;
            default:
                message = I18n::Format(T::NetworkStartFailedCode, {winrt::to_hstring(PortalErrorName(error)).c_str()});
        }
        configuration.notice = message + L" " + std::wstring{Tr(T::NetworkReportSaved)};
    };
    // Reads the console's addresses again, keeping the chosen one when it is still there.
    // Returns the address that was chosen before, empty when there was none.
    const auto refresh_addresses = [&] {
        const auto chosen = configuration.network_addresses.empty() ? PortalAddress{} :
            configuration.network_addresses[configuration.network_selected];
        configuration.network_addresses = PortalAddresses();
        configuration.network_selected = 0;
        for (size_t i = 0; i < configuration.network_addresses.size(); ++i)
            if (configuration.network_addresses[i].ipv4 == chosen.ipv4) configuration.network_selected = i;
        return chosen;
    };
    // Starts the portal on the chosen address, or says why not. Never logs the pairing code.
    const auto start_portal = [&] {
        // The list was read when Settings opened; a Wi-Fi reconnection may have changed it since.
        // When the address went away while binding, the list is read once more.
        for (unsigned attempt = 0;; ++attempt) {
            const auto chosen = refresh_addresses();
            if (configuration.network_addresses.empty()) {
                portal_fault("start", PortalError::NoAddress, chosen, portal_port, 0);
                return false;
            }
            const auto address = configuration.network_addresses[configuration.network_selected];
            if (!chosen.ipv4.empty() && chosen.ipv4 != address.ipv4)
                LOG_INFO(Frontend, "Network portal: {} {} is gone, using {} {}", chosen.name, chosen.ipv4,
                         address.name, address.ipv4);
            const bool started = network_portal.Start(address.ipv4, portal_port, portal_spare_ports);
            const auto status = network_portal.Status();
            if (started) {
                portal_address = address;
                portal_faults.Recovered(address, status.port);
                configuration.notice = address.link_local ?
                    std::wstring{winrt::to_hstring(address.ipv4)} + L": " + std::wstring{Tr(T::NetworkLinkLocal)} :
                    std::wstring{};
                LOG_INFO(Frontend, "Network portal: listening on {} {} port {}", address.name, address.ipv4, status.port);
                return true;
            }
            if (status.error == PortalError::AddressLost && attempt == 0) continue;
            portal_fault("start", status.error, address, status.port ? status.port : portal_port, status.os_error);
            return false;
        }
    };
    // Without a native deployment (Xbox) the install is guided from a browser: its screen opens
    // over the update page with the portal already listening.
    const auto open_update_guide = [&] {
        updater.RecordAssistedRequest();
        if (!network_portal.Status().running) start_portal();
        else configuration.notice.clear();
        configuration.network = network_portal.Status();
        configuration.network_screen = configuration.network_update = true;
        configuration.network_action = 0;
        dirty = true;
    };
    while (true) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        const auto now = std::chrono::steady_clock::now();
        network_portal.SetUploadsAvailable(!configuration.busy && !picker && !validation.valid() &&
            !game_options.open && !content_session && !settings && !system_page.open &&
            updater.CurrentPhase() != Updates::Phase::Downloading && updater.CurrentPhase() != Updates::Phase::Verifying &&
            updater.CurrentPhase() != Updates::Phase::Preparing && updater.CurrentPhase() != Updates::Phase::Requested);
        if (now >= network_poll_at) {
            auto update = updater.Status();
            if (update.revision != system_page.update.revision) dirty = true;
            system_page.update = std::move(update);
            if (system_page.updates_open && system_page.update.phase == Updates::Phase::Ready) {
                const bool storage_idle = !configuration.busy && !picker && !validation.valid() &&
                    !content_session && !loading && !setup_future.valid() &&
                    network_portal.BlockForLocalOperation();
                updater.InstallIfReady(storage_idle);
                if (updater.CurrentPhase() == Updates::Phase::Assistance) open_update_guide();
            }
            network_poll_at = now + std::chrono::milliseconds{500};
            network_portal.SetUpdateOffered(system_page.update.phase == Updates::Phase::Assistance);
            auto state = network_portal.Status();
            // The listener died on its own, or the console left the network it listens on (the
            // Wi-Fi dropped, the cable came out, the router gave another address): say so.
            if (configuration.network.running && !state.running && state.error != PortalError::None) {
                portal_fault("stopped", state.error, portal_address, configuration.network.port, 0);
                dirty = true;
            } else if (state.running && now >= portal_check_at) {
                portal_check_at = now + std::chrono::seconds{3};
                if (!PortalAddressOwned(portal_address.ipv4)) {
                    network_portal.Stop();
                    state = network_portal.Status();
                    portal_fault("stopped", PortalError::AddressLost, portal_address, configuration.network.port, 0);
                    refresh_addresses();
                    dirty = true;
                }
            }
            if (state.running) portal_library.Publish(scan.entries, static_cast<std::uint32_t>(title_language));
            if (configuration.network.running != state.running || configuration.network.paired != state.paired ||
                configuration.network.code != state.code || configuration.network.code_expired != state.code_expired ||
                configuration.network.error != state.error ||
                configuration.network.transfer.received != state.transfer.received ||
                configuration.network.transfer.phase != state.transfer.phase ||
                configuration.network.transfer.id != state.transfer.id ||
                configuration.network.transfer.error != state.transfer.error ||
                configuration.network.content.received != state.content.received ||
                configuration.network.content.phase != state.content.phase ||
                configuration.network.content.error != state.content.error ||
                configuration.network.removal != state.removal ||
                configuration.network.system.phase != state.system.phase ||
                configuration.network.system.processed != state.system.processed ||
                configuration.network.system.file.received != state.system.file.received ||
                configuration.network.system.error != state.system.error) {
                configuration.network = state;
                dirty = true;
            }
            const bool system_pending=state.system.phase=="prepare_pending"||state.system.phase=="commit_pending";
            const std::optional<StorageJob> next_job = removal_request ? StorageJob::Removal :
                system_pending ? StorageJob::PortalSystem : state.removal.phase == "pending" ? StorageJob::PortalRemoval :
                state.transfer.phase == "ready" ? StorageJob::PortalGame :
                state.content.phase == "ready" ? std::optional{StorageJob::PortalContent} : std::nullopt;
            if (next_job && !configuration.busy && !loading &&
                !setup_future.valid() && !picker && !validation.valid() && !content_session && !game_options.open) {
                configuration.busy=true;
                storage_job=*next_job;
                import_progress.completed=0; import_progress.total=0;
                portal_library.Progress(0, 0);
                if (storage_job == StorageJob::Removal || storage_job == StorageJob::PortalRemoval) notice = Tr(T::GameRemoving);
                metadata_worker.request_stop();
                // An update/DLC from the portal goes through the content manager's import.
                const auto install=[&](const std::filesystem::path& path, const std::string& tag, std::stop_token stop) {
                    return portal_library.Install(path, tag, stop, [&](std::uint64_t done, std::uint64_t total) {
                        import_progress.completed=static_cast<unsigned>(done >> 20);
                        import_progress.total=static_cast<unsigned>(total >> 20);
                    });
                };
                std::packaged_task<std::string(std::stop_token)> task{
                    [&, job=storage_job, install, removal=removal_request.value_or(GameFiles{}),
                     old_metadata=std::move(metadata_worker)](std::stop_token stop) mutable {
                        std::stop_callback cancel{stop,[&] { network_portal.CancelTransfer(); }};
                        if (old_metadata.joinable()) old_metadata.join();
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                        switch (job) {
                        case StorageJob::Removal: return RemoveGame(root, removal);
                        case StorageJob::PortalSystem: return network_portal.RunSystemImport(stop);
                        case StorageJob::PortalRemoval:
                            return network_portal.RunRemoval([&](const PortalRemoval& r) { return portal_library.Remove(r); });
                        case StorageJob::PortalContent: return network_portal.FinalizeContent(install);
                        default: return network_portal.FinalizeGame(ValidateUploadedNsp);
                        }
                    }};
                // Keys and firmware stay as they are: the rescan keeps every other game's titles.
                keep_titles = storage_job == StorageJob::PortalContent ? std::optional{ContentTagApplication(state.content.tag)} :
                    storage_job == StorageJob::PortalRemoval ? std::optional{state.removal.application} :
                    storage_job == StorageJob::Removal ? std::optional{removal_request->application} :
                    storage_job == StorageJob::PortalGame ? std::optional<std::uint64_t>{0} : std::nullopt;
                removal_request.reset();
                import_future=task.get_future();
                import_worker=std::jthread(std::move(task));
            }
        }
        if (!panel.keyboard.notice.empty() && now >= keyboard_notice_until) {
            panel.keyboard.notice.clear(); dirty = true;
        }
        if (!panel.notice.empty() && now >= controller_notice_until) {
            panel.notice.clear(); dirty = true;
        }
        if (!system_page.notice.empty() && now >= system_notice_until) {
            system_page.notice.clear(); dirty = true;
        }
        if (!options_notice.empty() && now >= options_notice_until) {
            options_notice.clear(); dirty = true;
        }
        SYSTEMTIME clock{};
        GetLocalTime(&clock);
        if (clock.wMinute != shown_minute) { shown_minute = clock.wMinute; dirty = true; }
        if (panel.keyboard.capturing && now >= capture_until) {
            panel.keyboard.capturing = false; dirty = true;
        }
        const bool catalog_changed = controllers.Update(now, [] {
            RefreshProControllers();
            return EnumerateControllers();
        }, SameControllerList);
        if (catalog_changed) {
            panel.devices = controllers.Devices();
            panel.choice = std::min(panel.choice, panel.devices.size() + 1);
            // An open list must not apply a stale row after hotplug/reorder.
            if (panel.expanded) {
                panel.expanded = false;
                actions &= ~Play;
            }
            dirty = true;
        }
        // Selection follows settings immediately; discovery remains rate limited.
        const auto next = controllers.Select(options.controller_id);
        if (bool(pad) != bool(next) || (pad && next &&
            (pad->id != next->id || pad->pad != next->pad || pad->pro != next->pro))) {
            previous = {}; stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset();
            battery_read_at = {}; // Another controller: read its battery now.
        }
        if (catalog_changed || !pad || !next || pad->id != next->id)
            pad = next ? std::optional<ControllerDevice>{*next} : std::nullopt;
        const unsigned context = configuration.confirm_import ? 1001 : configuration.network_screen ? 1000 : game_options.open ? (7 + static_cast<unsigned>(game_options.selected) * 3 + static_cast<unsigned>(game_options.focus)) :
            system_page.name_editor ? 6 : system_page.open ? 5 :
            panel.keyboard.open ? 3 : panel.expanded ? 2 : settings ? 1 : configuration.open ? 4 : 0;
        if (context != navigation_context) {
            stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset(); navigation_context = context;
        }
        if (pad) {
            try {
                const auto reading = ReadController(*pad).gamepad;
                const auto buttons = reading.Buttons;
                // Track physical edges before mapping: changing the option while holding a
                // button must not synthesize a second press of its new logical counterpart.
                const auto edges = MapFaceButtons(buttons & ~previous,
                    SwapControllerFaceButtons(*pad, options.swap_face_buttons));
                previous = buttons;
                auto hit = [&](GamepadButtons b) { return (edges & b) == b; };
                // The d-pad takes precedence; do not combine opposite directions.
                constexpr auto dpad = GamepadButtons::DPadUp | GamepadButtons::DPadDown |
                                      GamepadButtons::DPadLeft | GamepadButtons::DPadRight;
                unsigned directions = 0;
                if (game_options.open) {
                    const double dx = ((buttons & GamepadButtons::DPadRight) != GamepadButtons::None ? 1.0 : 0.0) -
                                      ((buttons & GamepadButtons::DPadLeft) != GamepadButtons::None ? 1.0 : 0.0);
                    const double dy = ((buttons & GamepadButtons::DPadUp) != GamepadButtons::None ? 1.0 : 0.0) -
                                      ((buttons & GamepadButtons::DPadDown) != GamepadButtons::None ? 1.0 : 0.0);
                    switch (manager_dpad.Update((buttons & dpad) != GamepadButtons::None ? dx : reading.LeftThumbstickX,
                                                (buttons & dpad) != GamepadButtons::None ? dy : reading.LeftThumbstickY, now)) {
                    case StickDirection::Up: directions |= Up; break;
                    case StickDirection::Down: directions |= Down; break;
                    case StickDirection::Left: directions |= Left; break;
                    case StickDirection::Right: directions |= Right; break;
                    case StickDirection::None: break;
                    }
                    const auto triggers = trigger_tabs.Update(reading.LeftTrigger, reading.RightTrigger);
                    if (triggers == 1) actions |= PreviousManagerTab;
                    if (triggers == 2) actions |= NextManagerTab;
                } else {
                    if ((buttons & dpad) != GamepadButtons::None) stick_navigation.Reset();
                    else {
                        switch (stick_navigation.Update(reading.LeftThumbstickX, reading.LeftThumbstickY, now)) {
                        case StickDirection::Up: directions |= Up; break;
                        case StickDirection::Down: directions |= Down; break;
                        case StickDirection::Left: directions |= Left; break;
                        case StickDirection::Right: directions |= Right; break;
                        case StickDirection::None: break;
                        }
                    }
                    if (hit(GamepadButtons::DPadUp)) directions |= Up;
                    if (hit(GamepadButtons::DPadDown)) directions |= Down;
                    if (hit(GamepadButtons::DPadLeft)) directions |= Left;
                    if (hit(GamepadButtons::DPadRight)) directions |= Right;
                }
                if (system_page.name_editor) {
                    // Like the console's keyboard: A types, X deletes, Y adds a space, Menu (or RB)
                    // is done, View (or LB) is Shift and B leaves without saving. The bumpers are
                    // what a game's keyboard uses, where + and - belong to the game menu.
                    auto& editor = *system_page.name_editor;
                    const std::pair<bool, MenuAction> presses[] = {
                        {(directions & Up) != 0, MenuAction::Up},
                        {(directions & Down) != 0, MenuAction::Down},
                        {(directions & Left) != 0, MenuAction::Left},
                        {(directions & Right) != 0, MenuAction::Right},
                        {hit(GamepadButtons::A), MenuAction::Confirm},
                        {hit(GamepadButtons::X), MenuAction::Alternate},
                        {hit(GamepadButtons::Y), MenuAction::Refresh},
                        {hit(GamepadButtons::View) || hit(GamepadButtons::LeftShoulder), MenuAction::PreviousTab},
                        {hit(GamepadButtons::Menu) || hit(GamepadButtons::RightShoulder), MenuAction::NextTab},
                        {hit(GamepadButtons::B), MenuAction::Back},
                    };
                    for (const auto& [pressed, action] : presses) {
                        if (!pressed || finish_name_editor) continue;
                        const auto result = editor.Apply(*TextEntryCommand(action));
                        if (result != TextEntry::Result::None) finish_name_editor = result == TextEntry::Result::Done;
                    }
                    if (directions || edges != GamepadButtons::None) {
                        system_page.editor_notice.clear();
                        dirty = true;
                    }
                } else {
                    // Back must remain available while editing/capturing keyboard input.
                    // Track all edges, but suppress other pad actions inside that modal.
                    if (hit(GamepadButtons::B)) actions |= Quit;
                    if (!panel.keyboard.open) {
                        actions |= directions;
                        if (hit(GamepadButtons::A)) actions |= Play;
                        // On the library X rescans and Y pins; the pages keep their own X and Y.
                        const bool library = !settings && !configuration.open && !game_options.open &&
                                             !missing_content;
                        if (hit(GamepadButtons::X)) {
                            if (system_page.updates_open) actions |= Quit;
                            else if (settings || content_session || missing_content) actions |= Swap;
                            else if (library) actions |= Refresh;
                            else if (!configuration.open) actions |= AddFolder;
                        }
                        if (hit(GamepadButtons::Y)) actions |= library ? Pin : Refresh;
                        if (hit(GamepadButtons::Menu)) actions |= Options;
                        if (hit(GamepadButtons::View)) actions |= Panel;
                        if (hit(GamepadButtons::LeftShoulder)) actions |= PreviousTab;
                        if (hit(GamepadButtons::RightShoulder)) actions |= NextTab;
                    }
                }
            } catch (...) {
                controllers.ReadFailed();
                pad.reset(); previous = {}; stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset();
            }
        }
        // The battery changes slowly and its report is a driver call: a few times a minute.
        if (now >= battery_read_at) {
            battery_read_at = now + std::chrono::seconds{20};
            ControllerBattery read;
            try {
                if (pad && pad->pad) {
                    const auto report = pad->pad.TryGetBatteryReport();
                    using winrt::Windows::System::Power::BatteryStatus;
                    if (report && report.Status() != BatteryStatus::NotPresent) {
                        const auto remaining = report.RemainingCapacityInMilliwattHours();
                        const auto full = report.FullChargeCapacityInMilliwattHours();
                        if (remaining && full && full.Value() > 0) {
                            read.known = true;
                            read.level = std::clamp(static_cast<float>(remaining.Value()) / full.Value(), 0.0f, 1.0f);
                            read.charging = report.Status() == BatteryStatus::Charging;
                        }
                    }
                }
            } catch (...) {}
            if (!(read == battery)) { battery = read; dirty = true; }
        }
        if (window_closed || QuitRequested()) return std::nullopt;
        if (actions && system_page.updated) { system_page.updated = false; dirty = true; }
        if(import_progress.confirmation.load()&&!configuration.confirm_import){
            std::lock_guard lock{import_progress.review_mutex};configuration.review_text=import_progress.review_text;
            configuration.confirm_import=true;configuration.network_action=0;dirty=true;
        }
        if(configuration.confirm_import){
            if(actions&(Left|Right|Up|Down))configuration.network_action^=1;
            if(actions&Quit || actions&Play){
                import_progress.decision=(actions&Quit)||configuration.network_action==0?2:1;
                import_progress.confirmation=false;
                import_progress.decision.notify_all();configuration.confirm_import=false;dirty=true;
            }
            dirty|=actions!=0;actions=0;pressed_tab.reset();
        }
        if (configuration.network_screen) {
            if (actions & (Left | Up)) configuration.network_action = (configuration.network_action + 2) % 3;
            if (actions & (Right | Down)) configuration.network_action = (configuration.network_action + 1) % 3;
            if (actions & Quit || (actions & Play && configuration.network_action == 0))
                configuration.network_screen = false;
            else if (actions & Refresh || (actions & Play && configuration.network_action == 1)) {
                // The update guide stays open when its portal failed to start: this retries it.
                if (configuration.network_update && !network_portal.Status().running) start_portal();
                else configuration.notice = network_portal.RenewCode() ? std::wstring{} : std::wstring{Tr(T::NetworkStartFailed)};
                configuration.network = network_portal.Status();
            } else if (actions & Play && configuration.network_action == 2) {
                network_portal.Stop();
                configuration.network = network_portal.Status();
                configuration.network_screen = false;
                if (!configuration.network_update) configuration.selected = FilesNetwork;
                configuration.notice.clear();
            }
            if (!configuration.network.running && !configuration.network_update) configuration.network_screen = false;
            if (!configuration.network_screen) configuration.network_update = false;
            dirty |= actions != 0;
            actions = 0;
            pressed_tab.reset();
        }
        if (finish_name_editor) {
            apply_system_effect(FinishNameEditor(system_page, *finish_name_editor), now);
            finish_name_editor.reset();
            dirty = true;
        }
        if (picker) {
            actions = 0; // The system picker owns input until it returns.
            if (picker.Status() != winrt::Windows::Foundation::AsyncStatus::Started) {
                try {
                    auto folder = picker.GetResults();
                    if (folder && picker_kind == 0) start_scan(folder);
                    else if (folder && picker_kind == 3 && content_session) content_session->AuthorizeFolder(folder);
                    else if (folder && picker_kind == 4 && mod_session) mod_session->AddFolder(folder);
                    else if (picker_kind >= 3) {} // A page's folder whose page closed meanwhile.
                    else if (folder) {
                        configuration.busy = true;
                        configuration.notice.clear();
                        import_progress.completed = 0;
                        import_progress.total = 0;
                        metadata_worker.request_stop();
                        std::packaged_task<std::string(std::stop_token)> task{
                            [folder, kind = picker_kind, old_metadata = std::move(metadata_worker), &import_progress](std::stop_token stop) mutable {
                                if (old_metadata.joinable()) old_metadata.join();
                                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                                return ImportSystemFiles(folder, kind == 1 ? FileImport::Keys : FileImport::Firmware, import_progress, stop);
                            }};
                        storage_job = StorageJob::Files;
                        import_future = task.get_future();
                        import_worker = std::jthread(std::move(task));
                    } else { configuration.notice = Tr(T::SelectionCancelled); dirty = true; }
                } catch (const winrt::hresult_error& e) {
                    configuration.notice = I18n::Format(T::FolderPickFailed, {e.message()}); dirty = true;
                }
                picker = nullptr;
                previous = {};
                stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset();
            }
        }
        if (missing_content) {
            if (actions & Quit) { missing_content = false; actions = 0; }
            else if (actions & Swap) { Content::AllowMissingForNextLaunch(selected_path); return selected_path; }
            else if (actions & Play) missing_content = false;
            else actions = 0;
            dirty = true;
        }
        if (validation.valid()) {
            actions = 0;
            if (validation.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                try {
                    const auto error = validation.get();
                    if (error.empty()) return selected_path;
                    if (error == "content-missing") missing_content = true;
                    else notice = Wide(error);
                } catch (const std::exception& e) {
                    LOG_WARNING(Frontend, "Game check failed: {}", e.what());
                    notice = Tr(T::GameLoadFailed);
                }
                dirty = true;
            }
        }
        if (setup_future.valid() && setup_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try { configuration.status = setup_future.get(); }
            catch (...) { configuration.notice = Tr(T::SetupCheckFailed); }
            configuration.selected = std::min(configuration.selected, ConfigurationRowCount(configuration) - 1);
            dirty = true;
        }
        if (configuration.busy) {
            if (actions & Quit) { import_worker.request_stop(); configuration.notice = Tr(T::CancellingImport); }
            actions = 0;
            const auto done = import_progress.completed.load(std::memory_order_relaxed);
            const auto total = import_progress.total.load(std::memory_order_relaxed);
            if (done != configuration.completed || total != configuration.total) {
                configuration.completed = done; configuration.total = total; dirty = true;
            }
            if (import_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
                const bool removal = storage_job == StorageJob::Removal || storage_job == StorageJob::PortalRemoval;
                try {
                    const auto error = import_future.get();
                    if (removal) {
                        const bool content = storage_job == StorageJob::PortalRemoval &&
                            !network_portal.Status().removal.content.empty();
                        after_scan = Tr(!error.empty() ? (error == "external" ? T::GameRemoveExternal : T::GameRemoveFailed) :
                                        content ? T::ContentRemoved :
                                        storage_job == StorageJob::Removal && removed_copy ? T::GameCopyRemoved : T::GameRemoved);
                    } else {
                        configuration.notice = error.empty() ? std::wstring{Tr(storage_job==StorageJob::PortalSystem&&network_portal.Status().system.phase!="complete"?T::SystemReviewReady:T::ImportDone)} :
                            storage_job != StorageJob::Files ? std::wstring{Tr(T::ImportFailedKept)} : Wide(error);
                    }
                } catch (...) {
                    if (removal) after_scan = Tr(T::GameRemoveFailed);
                    else configuration.notice = Tr(T::ImportFailedKept);
                }
                configuration.busy = false;
                portal_library.Progress(0, 0);
                configuration.confirm_import=false;
                // The old metadata worker has joined; discard results before rescanning.
                metadata = {};
                refresh_setup();
                start_scan();
                dirty = true;
            }
        }
        // Switching tabs: the bumpers (Q/E) at the top level, or a press on the header.
        const auto switch_tab = [&](LibraryTab to) {
            if (to == tab) return;
            if (tab == LibraryTab::Settings) {
                system_page.open = false;
                configuration.open = configuration.files = false;
                settings = return_to_configuration = false;
                panel.expanded = panel.keyboard.open = panel.keyboard.capturing = false;
            }
            tab = to;
            if (to == LibraryTab::Settings) {
                configuration.open = true; configuration.files = false; configuration.selected = HomeSystem;
                configuration.notice.clear(); refresh_setup();
                if (!network_portal.Status().running) {
                    configuration.network_addresses = PortalAddresses();
                    configuration.network_selected = 0;
                }
            }
            if (to == LibraryTab::Library) {
                // The shelf opens on the game Home had focused.
                shelf = LibraryLayout::Shelf(scan.entries, filter);
                shelf_focus = 0;
                for (size_t i = 0; i < shelf.size(); ++i)
                    if (shelf[i] == selected) shelf_focus = i;
            }
            filter_focus = false;
            dirty = true;
        };
        // Windows can replace us after accepting a deployment. Do not launch a game or
        // begin another storage operation while the old process is waiting to close.
        if (updater.CurrentPhase() == Updates::Phase::Preparing || updater.CurrentPhase() == Updates::Phase::Requested) {
            actions = 0;
            pressed_tab.reset();
        }
        if (system_page.updates_open) {
            const auto& snapshot = system_page.update;
            const auto items = Updates::Actions(snapshot);
            // B, and X like everywhere a page closes, leave; Y does nothing here.
            const bool leave = actions & (Quit | Swap | Refresh | AddFolder);
            if (system_page.update_detail) {
                if (actions & Up && system_page.update_scroll) --system_page.update_scroll;
                if (actions & Down) ++system_page.update_scroll;
                if (leave || actions & Play) { system_page.update_detail=0; system_page.update_scroll=0; }
                if (actions) dirty=true;
                actions=0;
            }
            if (!items.empty()) {
                system_page.update_row = std::min(system_page.update_row, items.size()-1);
                if (actions & Up) system_page.update_row = (system_page.update_row + items.size()-1)%items.size();
                if (actions & Down) system_page.update_row = (system_page.update_row+1)%items.size();
            }
            if (actions && leave) system_page.updates_open = false;
            else if (actions & Play && !items.empty()) {
                using A = Updates::Action;
                const auto action = items[system_page.update_row];
                // Reserve local storage admission atomically against incoming browser uploads.
                const bool mutation = action == A::Download || action == A::Install || action == A::Cancel;
                if (mutation && (configuration.busy || picker || validation.valid() || content_session || !network_portal.BlockForLocalOperation())) {
                    system_page.update.error = Updates::Error::Busy;
                } else switch (action) {
                case A::Notes: system_page.update_detail=1; system_page.update_scroll=0; break;
                case A::Check: updater.Check(true); break;
                case A::Channel: updater.SetChannel(snapshot.channel == Updates::Channel::Stable ? Updates::Channel::Preview : Updates::Channel::Stable); break;
                case A::Download: updater.Download(true); break;
                case A::Pause: updater.Pause(); break;
                case A::Cancel: updater.Cancel(); break;
                case A::Install:
                    if (snapshot.phase == Updates::Phase::Ready) updater.Install();
                    if (updater.CurrentPhase() == Updates::Phase::Assistance) open_update_guide();
                    break;
                case A::Later: updater.Dismiss(false); system_page.updates_open = false; break;
                case A::Skip: updater.Dismiss(true); system_page.updates_open = false; break;
                }
                // Notes and Channel keep their row; the others change which rows there are.
                if (action != A::Notes && action != A::Channel) system_page.update_row = 0;
            }
            if (actions) dirty = true;
            actions = 0; pressed_tab.reset();
        }
        // While the new-version notice shows on Home or the Library, Y opens the update page and
        // X hides the notice until tomorrow; neither pins nor rescans then.
        if (system_page.update.notice && system_page.update.release && !system_page.updates_open && !settings &&
            !configuration.open && !configuration.network_screen && !game_options.open && !missing_content &&
            !panel.keyboard.open && tab != LibraryTab::Settings && actions & (Pin | Refresh)) {
            if (actions & Pin) { system_page.updates_open = true; system_page.update_row = 0; }
            else { updater.Dismiss(false); system_page.update.notice = false; }
            actions &= ~(Pin | Refresh);
            dirty = true;
        }
        // Leaving the update page to use another feature pauses network work before dispatching it.
        if (!system_page.updates_open && actions &&
            (updater.CurrentPhase() == Updates::Phase::Downloading || updater.CurrentPhase() == Updates::Phase::Verifying)) updater.Pause();
        const bool top_level = !settings && !system_page.open && !configuration.files && !configuration.busy &&
                               !picker && !validation.valid() && !game_options.open && !missing_content;
        if (pressed_tab) {
            // A primary tab pops idle management pages. Never interrupt an import/install.
            if (game_options.open && (!content_session || !content_session->Busy()) &&
                (!mod_session || (!mod_session->Busy() && mod_session->Downloads().empty()))) {
                const auto changed = content_session && content_session->Changed() ?
                    std::optional{content_session->Application()} : std::nullopt;
                content_session.reset();
                mod_session.reset();
                cheat_session.reset();
                game_options = {};
                if (changed) forget_content(*changed);
                switch_tab(*pressed_tab);
            }
            if (top_level || *pressed_tab == LibraryTab::Settings || tab == LibraryTab::Settings) {
                if (!game_options.open && !configuration.busy && !picker && !validation.valid()) switch_tab(*pressed_tab);
            }
            pressed_tab.reset();
        }
        const bool settings_page = tab == LibraryTab::Settings && (settings || system_page.open || configuration.files) &&
                                   !panel.keyboard.open && !panel.expanded && !system_page.name_editor &&
                                   !configuration.busy && !picker && !validation.valid() && !game_options.open &&
                                   !missing_content;
        if ((top_level || settings_page) && (actions & (PreviousTab | NextTab))) {
            constexpr auto count = static_cast<unsigned>(LibraryTab::Count);
            const auto current = static_cast<unsigned>(tab);
            const auto to = actions & PreviousTab ? (current + count - 1) % count : (current + 1) % count;
            switch_tab(static_cast<LibraryTab>(to));
            actions &= ~(PreviousTab | NextTab);
        }
        if (system_page.open && configuration.open && !settings) {
            // The name editor takes its input in the handlers above; the page waits behind it.
            if (!system_page.name_editor && actions) {
                const auto send = [&](unsigned bit, SystemInput input) {
                    if (actions & bit) apply_system_effect(HandleSystemInput(system_page, input), now);
                };
                send(Up, SystemInput::Up);
                send(Down, SystemInput::Down);
                const bool opens = system_page.selected == SystemPage::UserName ||
                                   system_page.selected == SystemPage::ClearCache || system_page.selected == SystemPage::Updates;
                send(Left, opens ? SystemInput::Back : SystemInput::Left);
                send(Right, SystemInput::Right);
                send(Play, SystemInput::Confirm);
                send(Quit, SystemInput::Back);
                if (!system_page.open) configuration.selected = HomeSystem;
            }
            actions = 0;
        }
        const bool on_game = tab == LibraryTab::Home ||
                             (tab == LibraryTab::Library && !filter_focus && shelf_focus < shelf.size());
        if (actions & Options && on_game && !game_options.open && !settings && !configuration.open &&
            !panel.keyboard.open && !loading && !picker && !validation.valid() && !scan.entries.empty()) {
            if (!network_portal.BlockForLocalOperation()) {
                notice=Tr(T::NetworkBusy); actions=0; dirty=true;
            } else {
            game_options = {true, selected};
            requested_game_option = 0;
            options_notice.clear();
            actions = 0;
            dirty = true;
            }
        }
        if (game_options.open) {
            // A rescan may have replaced the list under the page.
            if (!content_session && game_options.game >= scan.entries.size()) {
                game_options = {};
                cheat_session.reset();
                mod_session.reset();
                actions = 0; dirty = true;
                continue;
            }
            const bool mods_active = game_options.page == GameOption::Mods && mod_session;
            const bool cheats_active = game_options.page == GameOption::Cheats && cheat_session;
            const bool can_switch = !configuration.open && !picker && (!content_session || !content_session->Busy()) &&
                (!mods_active || (!mod_session->Busy() && mod_session->Downloads().empty()));
            const bool has_manager_tabs = mods_active ||
                (cheats_active && Cheats::CheatTabCount(cheat_session->Catalog()) > 0);
            const bool manager_busy = (mods_active && mod_session->Busy()) ||
                (content_session && content_session->Busy());
            const bool manager_modal = mods_active && mods_page.screen != Mods::ModsScreen::List;
            if (actions & (PreviousTab | NextTab)) {
                if (can_switch && !manager_modal) {
                    requested_game_option = actions & PreviousTab ?
                        (game_options.selected + GameOptions().size() - 1) % GameOptions().size() :
                        (game_options.selected + 1) % GameOptions().size();
                    dirty = true;
                }
                actions &= ~(PreviousTab | NextTab);
            }
            if ((actions & (PreviousManagerTab | NextManagerTab)) && !manager_busy && !manager_modal && has_manager_tabs) {
                if (game_options.focus == GameOptionsFocus::Sections)
                    game_options.focus = GameOptionsFocus::ManagerTabs;
                dirty = true;
            }
            if (game_options.focus == GameOptionsFocus::Sections) {
                if (!can_switch) actions &= Quit;
                else {
                    if (actions) dirty = true;
                    if (actions & (Left | PreviousTab)) requested_game_option =
                        (game_options.selected + GameOptions().size() - 1) % GameOptions().size();
                    if (actions & (Right | NextTab)) requested_game_option =
                        (game_options.selected + 1) % GameOptions().size();
                    if (actions & (Down | Play)) EnterGameOptionsPane(game_options, has_manager_tabs);
                    actions &= ~(Left | Right | PreviousTab | NextTab | Up | Down | Play);
                }
            } else if (!configuration.open && !manager_busy) {
                // Only a pointer focuses the manager's tabs: any way down or out goes back to the
                // pane. In the pane, B at the manager's root closes the options.
                if (game_options.focus == GameOptionsFocus::ManagerTabs) {
                    if (actions & (Up | Down | Play | Quit)) {
                        game_options.focus = GameOptionsFocus::Content;
                        actions &= ~(Up | Down | Play | Quit); dirty = true;
                    }
                    actions &= Left | Right | PreviousManagerTab | NextManagerTab;
                }
            }
            if (requested_game_option == game_options.selected && !game_options.activate && can_switch) {
                game_options.focus = GameOptionsFocus::Content;
                game_options.confirm_discard = false;
                requested_game_option.reset(); options_notice.clear(); dirty = true;
            }
            if (requested_game_option && can_switch) {
                const auto changed = content_session && content_session->Changed() ?
                    std::optional{content_session->Application()} : std::nullopt;
                content_session.reset(); requested_manager_tab.reset();
                SelectGameOption(game_options, *requested_game_option);
                requested_game_option.reset(); options_notice.clear();
                if (changed) {
                    mod_session.reset(); cheat_session.reset();
                    forget_content(*changed);
                }
                dirty = true;
            } else if (requested_game_option) {
                requested_game_option.reset(); // Busy jobs retain their page and selection.
            }
            if (cheat_session && cheat_session->Poll()) {
                std::size_t entries = 0;
                for (const auto category : {Cheats::CheatCategory::Gameplay, Cheats::CheatCategory::Graphics, Cheats::CheatCategory::Mine})
                    entries += cheat_session->Catalog().Cheats(category).size();
                LOG_INFO(Frontend, "Cheats manager: title={:016X} build={} entries={} source={}",
                         cheat_session->Game().title_id, Cheats::HexId(cheat_session->Game().build_id),
                         entries, static_cast<unsigned>(cheat_session->Source()));
                Common::Log::Stop();
                Cheats::ClampCheatsPage(cheats_page, cheat_session->Catalog(), Cheats::CheatsLayout::VisibleRows);
                dirty = true;
            }
            const auto notify = [&](T text) {
                options_notice = Tr(text);
                options_notice_until = now + std::chrono::seconds{4};
            };
            if (game_options.activate && !configuration.open &&
                scan.entries[game_options.game].title_loaded) {
                game_options.activate = false;
                game_options.page = GameOptionHasManager(GameOptions()[game_options.selected]) ?
                    std::optional{GameOptions()[game_options.selected]} : std::nullopt;
                if (game_options.page == GameOption::Updates || game_options.page == GameOption::Dlc) {
                    const auto& game = scan.entries[game_options.game];
                    if (!game.title_loaded || !game.program_id) {
                        notify(T::CheatsUnavailable); game_options.page.reset();
                    } else {
                        std::vector<std::uint64_t> bases;
                        for (const auto& e : scan.entries) if (e.program_id) bases.push_back(FileSys::GetBaseTitleID(e.program_id));
                        metadata_worker.request_stop();
                        content_session = std::make_unique<Content::ContentSession>(game.program_id, game.launch_path,
                            *game_options.page == GameOption::Updates ? Content::Kind::Update : Content::Kind::Dlc,
                            std::move(bases), std::move(metadata_worker));
                        metadata = {};
                    }
                }
                if (game_options.page == GameOption::Mods && !mod_session) {
                    // Mods are kept per title id; the build id (of the version that boots, update
                    // included) tells which mods' patches fit it.
                    const auto& game = scan.entries[game_options.game];
                    if (game.title_loaded && game.program_id == 0) {
                        notify(T::ModsHomebrew);
                        game_options.page.reset();
                    } else if (!game.title_loaded) {
                        notify(T::CheatsUnavailable);
                        game_options.page.reset();
                    } else {
                        std::string build_id;
                        if (std::any_of(game.build_id.begin(), game.build_id.end(), [](std::uint8_t b) { return b != 0; }))
                            for (const auto b : game.build_id) build_id += fmt::format("{:02X}", b);
                        // The store and the installs log what went wrong; the log opens with the page.
                        if (!logging_ready) { Common::Log::Initialize(); logging_ready = true; }
                        mod_session = std::make_unique<Mods::ModSession>(game.program_id, std::move(build_id),
                                                                         game.launch_path, I18n::ToUtf8(game.name),
                                                                         system_page.settings.show_adult_mods);
                        mods_page = {};
                        options_notice.clear();
                    }
                }
                if (game_options.page == GameOption::Cheats && !cheat_session) {
                    // Cheat files are named after the game's title and main build ids, which the
                    // library reads with the title (uwp_library_metadata.cpp).
                    const auto& game = scan.entries[game_options.game];
                    const bool known = std::any_of(game.build_id.begin(), game.build_id.end(),
                                                   [](std::uint8_t b) { return b != 0; });
                    if (game.title_loaded && game.program_id == 0) {
                        notify(T::CheatsHomebrew);
                        game_options.page.reset();
                    } else if (!game.title_loaded || !known) {
                        notify(T::CheatsUnavailable);
                        game_options.page.reset();
                    } else {
                        Cheats::CheatGame cheat_game{game.program_id};
                        std::copy(game.build_id.begin(), game.build_id.end(), cheat_game.build_id.begin());
                        cheat_session.emplace(cheat_game);
                        cheats_page = {};
                        options_notice.clear();
                    }
                }
            }
            if (content_session) {
                dirty |= content_session->Poll();
                if (content_session->Busy()) dirty = true;
                const auto send = [&](unsigned bits, Content::Input input) {
                    if ((actions & bits) && content_session && content_session->Handle(input)) {
                        const bool changed = content_session->Changed();
                        const auto application = content_session->Application();
                        content_session.reset(); game_options = {};
                        if (changed) forget_content(application);
                    }
                };
                send(Up, Content::Input::Up); send(Down, Content::Input::Down);
                send(Play, Content::Input::Confirm); send(Swap | AddFolder, Content::Input::Import);
                send(PreviousManagerTab | NextManagerTab, Content::Input::Storage); send(Refresh, Content::Input::Refresh);
                send(Quit, Content::Input::Back);
                if (content_session && content_session->TakeAuthorizeRequest() && !picker) {
                    try {
                        winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                        folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                        folder_picker.FileTypeFilter().Append(L"*"); picker_kind = 3;
                        picker = folder_picker.PickSingleFolderAsync();
                    } catch (...) { options_notice = Tr(T::PickerOpenFailed); }
                }
                dirty |= actions != 0;
            } else if (game_options.page == GameOption::Mods && mod_session) {
                using Kind = Mods::ModsCommand::Kind;
                using Mods::ModJob;
                const ModJob finished = mod_session->Poll();
                if (finished != ModJob::None) {
                    const auto& result = mod_session->View();
                    const bool online = result.origin.kind == Mods::SourceKind::GameBanana ||
                                        result.origin.kind == Mods::SourceKind::Cnx;
                    if (result.error != Mods::ModError::None) notify(Mods::ErrorText(result.error));
                    if (finished == ModJob::Reading && result.error == Mods::ModError::None)
                        Mods::ShowVariants(mods_page, result.candidates.size());
                    // A download with several mods in it: the player picks, wherever they are.
                    if (finished == ModJob::Installing && result.error == Mods::ModError::None &&
                        result.installed == 0 && !result.candidates.empty())
                        Mods::ShowVariants(mods_page, result.candidates.size());
                    if (finished == ModJob::Installing && result.installed > 0) {
                        // From a folder, back to the list where the new mods sit on top; from the
                        // store, the player stays where they browse.
                        if (!online) Mods::ShowInstalled(mods_page);
                        if (result.error == Mods::ModError::None) {
                            options_notice = result.updated ? std::wstring{Tr(T::ModsUpdated)}
                                                            : I18n::Plural(T::ModsInstalledOne, T::ModsInstalledOther,
                                                                           result.installed);
                            options_notice_until = now + std::chrono::seconds{4};
                        }
                    }
                    if (finished == ModJob::Browsing) mods_page.selected = mods_page.first = 0;
                    if (mods_page.tab != Mods::ModsTab::Explore)
                        Mods::ClampModsList(mods_page, mods_page.tab == Mods::ModsTab::Installed ? result.mods.size()
                                                                                                : result.files.size());
                    dirty = true;
                }
                if (mod_session->Tick()) dirty = true;
                const auto failed = mod_session->TakeNotice();
                if (failed) notify(Mods::ErrorText(*failed));
                // The log keeps lines in memory until an error: a job's end or a failure writes
                // them out, so the file pulled from the console tells what happened.
                if (finished != ModJob::None || failed) Common::Log::Stop();
                if (mod_session->Busy()) dirty = true; // The progress moves.
                const std::wstring mods_game = scan.entries[game_options.game].name;
                const auto send = [&](unsigned bits, Mods::ModsInput input) {
                    if ((bits && !(actions & bits)) || !mod_session) return;
                    const auto counts = Mods::CountsFor(*mod_session, mods_page, mods_game);
                    const auto command = Mods::HandleModsInput(mods_page, counts, input);
                    switch (command.kind) {
                    case Kind::Toggle:
                        if (!mod_session->Toggle(command.index)) notify(T::ModsSaveFailed);
                        break;
                    case Kind::Move:
                        if (!mod_session->Move(command.index, command.to)) notify(T::ModsSaveFailed);
                        break;
                    case Kind::Remove: mod_session->Remove(command.index); break;
                    case Kind::Open: options_notice.clear(); mod_session->Open(command.index); break;
                    case Kind::Up: mod_session->Up(); break;
                    case Kind::Install:
                        mod_session->Install(mods_page.chosen);
                        mods_page.screen = Mods::ModsScreen::List;
                        break;
                    case Kind::Dismiss: mod_session->Dismiss(); break;
                    case Kind::Cancel: mod_session->Cancel(); break;
                    case Kind::Store: mod_session->OpenStore(); break;
                    case Kind::Retry: mod_session->RetryStore(); break;
                    case Kind::Want: mod_session->Want(command.index); break;
                    case Kind::Detail: mod_session->OpenDetail(command.index); break;
                    case Kind::Update: mod_session->OpenUpdate(command.index); break;
                    case Kind::CloseDetail: mod_session->CloseDetail(); break;
                    case Kind::Download:
                        if (mod_session->Download(command.index)) {
                            const auto& detail = mod_session->Store().detail;
                            if (detail && command.index < detail->files.size()) {
                                options_notice = I18n::Format(T::ModsDownloadQueued,
                                                              {Wide(detail->files[command.index].name)});
                                options_notice_until = now + std::chrono::seconds{4};
                            }
                        } else {
                            notify(T::ModsDownloadRefused);
                        }
                        break;
                    case Kind::Sort: mod_session->CycleSort(); break;
                    case Kind::Search: mod_session->Search(I18n::ToUtf8(command.text)); break;
                    case Kind::Source: mod_session->SwitchSource(); break;
                    case Kind::FindGame: mod_session->FindGame(I18n::ToUtf8(command.text)); break;
                    case Kind::PickGame: mod_session->PickGame(command.index); break;
                    case Kind::CancelDownloads: mod_session->CancelDownloads(); break;
                    case Kind::Categories: mod_session->OpenCategories(); break;
                    case Kind::EnterCategory: mod_session->EnterCategory(command.index); break;
                    case Kind::LeaveCategory: mod_session->LeaveCategory(); break;
                    case Kind::PickCategory: mod_session->PickCategory(command.index); break;
                    case Kind::Close:
                        mod_session.reset();
                        game_options = {};
                        break;
                    case Kind::None:
                    case Kind::Moved:
                        break;
                    }
                };
                if (requested_manager_tab) {
                    if (*requested_manager_tab < Mods::ModsTabCount && mods_page.screen == Mods::ModsScreen::List) {
                        const auto steps = (*requested_manager_tab + Mods::ModsTabCount -
                            static_cast<std::size_t>(mods_page.tab)) % Mods::ModsTabCount;
                        for (std::size_t i = 0; i < steps; ++i) send(0, Mods::ModsInput::NextTab);
                    }
                    requested_manager_tab.reset();
                }
                send(Up, Mods::ModsInput::Up);
                send(Down, Mods::ModsInput::Down);
                const bool sub_tabs = game_options.focus == GameOptionsFocus::ManagerTabs;
                send(Left, sub_tabs ? Mods::ModsInput::PreviousTab : Mods::ModsInput::Left);
                send(Right, sub_tabs ? Mods::ModsInput::NextTab : Mods::ModsInput::Right);
                send(PreviousManagerTab, Mods::ModsInput::PreviousTab);
                send(NextManagerTab, Mods::ModsInput::NextTab);
                send(Play, Mods::ModsInput::Confirm);
                send(Swap | AddFolder, Mods::ModsInput::Alternate);
                send(Refresh, Mods::ModsInput::Menu);
                send(Options, Mods::ModsInput::Start);
                send(Quit, Mods::ModsInput::Back);
                if (mod_session && mod_session->TakeFolderRequest() && !picker) {
                    try {
                        winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                        folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                        folder_picker.FileTypeFilter().Append(L"*");
                        picker_kind = 4;
                        picker = folder_picker.PickSingleFolderAsync();
                    } catch (...) { options_notice = Tr(T::PickerOpenFailed); }
                }
                dirty |= actions != 0;
            } else if (game_options.page == GameOption::Cheats && cheat_session && (actions || requested_manager_tab)) {
                using Cheats::CheatsEffect;
                const auto send = [&](unsigned bits, MenuAction action) {
                    if (bits && !(actions & bits)) return;
                    const auto input = Cheats::CheatsInputForAction(action);
                    if (!input) return;
                    switch (Cheats::HandleCheatsInput(cheats_page, *cheat_session, *input,
                                                      Cheats::CheatsLayout::VisibleRows)) {
                    case CheatsEffect::Changed:
                        if (!cheat_session->Saved()) notify(T::CheatsSaveFailed);
                        else options_notice.clear();
                        break;
                    case CheatsEffect::OverBudget: notify(T::CheatsOverBudget); break;
                    case CheatsEffect::Refresh: cheat_session->Retry(); break;
                    case CheatsEffect::Back:
                        cheat_session.reset();
                        game_options = {};
                        break;
                    case CheatsEffect::None:
                    case CheatsEffect::Moved:
                        break;
                    }
                };
                if (requested_manager_tab) {
                    std::array<Cheats::CheatCategory, Cheats::CategoryCount> storage;
                    const auto tabs = Cheats::CheatTabs(cheat_session->Catalog(), storage);
                    if (*requested_manager_tab < tabs.size()) {
                        for (std::size_t i = 0; i < tabs.size() && cheats_page.category != tabs[*requested_manager_tab]; ++i)
                            send(0, MenuAction::NextTab);
                    }
                    requested_manager_tab.reset();
                }
                send(Up, MenuAction::Up);
                send(Down, MenuAction::Down);
                send(Left | PreviousManagerTab, MenuAction::PreviousTab);
                send(Right | NextManagerTab, MenuAction::NextTab);
                send(Play, MenuAction::Confirm);
                send(Swap | AddFolder, MenuAction::Alternate);
                send(Refresh, MenuAction::Refresh);
                if (cheat_session) send(Quit, MenuAction::Back);
                dirty = true;
            } else if (actions) {
                const auto send = [&](unsigned bit, GameOptionsInput input) {
                    if (actions & bit) HandleGameOptionsInput(game_options, input);
                };

                const auto option = GameOptions()[game_options.selected];
                if (!GameOptionHasManager(option)) send(Play, GameOptionsInput::Confirm);
                send(Quit, GameOptionsInput::Back);
                if (option == GameOption::Remove && (game_options.confirm_discard || game_options.discard)) {
                    const auto& game = scan.entries[game_options.game];
                    auto files = FindDumpFiles(scan.entries, game.launch_path);
                    if (!game.program_id || files.external) {
                        game_options.confirm_discard = game_options.discard = false;
                        notify(files.external ? T::GameRemoveExternal : T::GameRemoveFailed);
                    } else if (std::exchange(game_options.discard, false)) {
                        // The storage worker deletes it once the options have closed.
                        removed_copy = files.kept;
                        removal_request = std::move(files);
                        game_options.open = false;
                        options_notice.clear();
                    } else {
                        options_notice = Tr(files.kept ? T::GameRemoveCopyConfirm : T::GameRemoveConfirm);
                        options_notice_until = std::chrono::steady_clock::time_point::max();
                    }
                } else if (game_options.confirm_discard || game_options.discard) {
                    auto& game = scan.entries[game_options.game];
                    if (game.pulse_saved.empty()) {
                        game_options.confirm_discard = game_options.discard = false;
                        notify(T::PulseNone);
                    } else if (std::exchange(game_options.discard, false)) {
                        notify(PulseSessions::Discard(game.launch_path) ? T::PulseDiscarded : T::PulseDiscardFailed);
                        refresh_pulse();
                    } else {
                        options_notice = Tr(T::PulseDiscardConfirm);
                        options_notice_until = std::chrono::steady_clock::time_point::max();
                    }
                } else if (options_notice == Tr(T::PulseDiscardConfirm) || options_notice == Tr(T::GameRemoveConfirm) ||
                           options_notice == Tr(T::GameRemoveCopyConfirm)) {
                    options_notice.clear();
                }
                dirty = true;
            }
            actions = 0;
        }
        if (!game_options.open && !content_session) {
            mod_session.reset(); cheat_session.reset(); requested_manager_tab.reset();
        }
        if (actions & Quit) {
            if (panel.keyboard.open) { panel.keyboard.open = panel.keyboard.capturing = false; dirty = true; actions &= ~Quit; }
            else if (panel.expanded) { panel.expanded = false; dirty = true; actions &= ~Quit; }
            else if (settings) { settings = false; configuration.open = return_to_configuration; return_to_configuration = false; dirty = true; actions &= ~Quit; }
            else if (configuration.open) {
                if (configuration.files) { configuration.files = false; configuration.selected = HomeFiles; }
                else configuration.open = false;
                dirty = true; actions &= ~Quit;
            }
            else if (tab == LibraryTab::Library) { switch_tab(LibraryTab::Home); actions &= ~Quit; }
            else return std::nullopt;
        }
        if (actions & Panel && !configuration.open) { settings = !settings; panel.keyboard.open = panel.keyboard.capturing = false; panel.expanded = false; dirty = true; }
        if (actions & AddFolder && !settings && !picker && !configuration.busy && !validation.valid() &&
            !configuration.open)
            switch_tab(LibraryTab::Settings);
        // The import tile opens the file manager on the Settings tab.
        if (actions & Play && tab == LibraryTab::Library && !filter_focus && shelf_focus >= shelf.size() &&
            LibraryLayout::HasImportTile(filter) && !settings && !configuration.open && !picker &&
            !validation.valid()) {
            switch_tab(LibraryTab::Settings);
            configuration.files = true;
            configuration.selected = 0;
            actions &= ~Play;
        }
        // The configuration closed itself (Back): its tab goes with it.
        if (tab == LibraryTab::Settings && !configuration.open && !settings) { tab = LibraryTab::Home; dirty = true; }
        if (configuration.open && !settings && !picker && !configuration.busy) {
            const auto count = ConfigurationRowCount(configuration);
            if (actions & Up) configuration.selected = (configuration.selected + count - 1) % count;
            if (actions & Down) configuration.selected = (configuration.selected + 1) % count;
            if (actions & (Up | Down)) dirty = true;
            if (configuration.files && actions & Left) {
                configuration.files = false; configuration.selected = HomeFiles; dirty = true;
            } else if (actions & Play || (!configuration.files && actions & Right)) {
                if (!configuration.files) {
                    switch (configuration.selected) {
                    case HomeFiles: configuration.files = true; configuration.selected = 0; break;
                    case HomeControllers: settings = true; return_to_configuration = true; setting_row = 0; break;
                    default:
                        system_page.open = true;
                        system_page.selected = SystemPage::UserName;
                        system_page.notice.clear();
                        // Windows' language or region may have changed since the library opened.
                        system_page.locale = DetectSystemLocale();
                        break;
                    }
                } else if (loading || setup_future.valid()) configuration.notice = Tr(T::WaitForCheck);
                else if (configuration.selected < FilesNetwork && !network_portal.BlockForLocalOperation()) {
                    configuration.notice=Tr(T::NetworkBusy);
                }
                else if (configuration.selected == 3 || configuration.selected == 4) {
                    std::vector<std::uint64_t> bases;
                    for (const auto& e : scan.entries) if (e.program_id) bases.push_back(FileSys::GetBaseTitleID(e.program_id));
                    metadata_worker.request_stop();
                    content_session = std::make_unique<Content::ContentSession>(0, "",
                        configuration.selected == 3 ? Content::Kind::Update : Content::Kind::Dlc,
                        std::move(bases), std::move(metadata_worker));
                    metadata = {}; game_options.open = true; game_options.game = 0;
                    game_options.activate = false; game_options.focus = GameOptionsFocus::Content;
                }
                else if (configuration.selected == FilesNetwork) {
                    if (network_portal.Status().running) {
                        configuration.network_screen = true;
                        configuration.network_action = 0;
                        configuration.notice.clear();
                    } else {
                        configuration.network_screen = start_portal();
                        configuration.network_action = 0;
                    }
                    configuration.network = network_portal.Status();
                }
                else if (configuration.selected == FilesNetworkAddress) {
                    network_portal.Stop();
                    const auto old = configuration.network_addresses.empty() ? std::string{} :
                        configuration.network_addresses[configuration.network_selected].ipv4;
                    configuration.network_addresses = PortalAddresses();
                    configuration.network_selected = 0;
                    for (size_t i = 0; i < configuration.network_addresses.size(); ++i)
                        if (configuration.network_addresses[i].ipv4 == old)
                            configuration.network_selected = (i + 1) % configuration.network_addresses.size();
                    configuration.network = network_portal.Status();
                    configuration.notice.clear();
                }
                else if (configuration.selected == FilesNetworkCode) {
                    if (!network_portal.RenewCode()) configuration.notice = Tr(T::NetworkStopped);
                    else configuration.notice.clear();
                    configuration.network = network_portal.Status();
                    configuration.network_screen = configuration.network.running;
                    configuration.network_action = 1;
                }
                else if (configuration.selected >= FilesSources) {
                    try {
                        ForgetGameFolder(configuration.status.sources[configuration.selected - FilesSources].token);
                        configuration.notice = Tr(T::FolderRemoved);
                        configuration.selected = 0; refresh_setup(); start_scan();
                    } catch (...) { configuration.notice = Tr(T::FolderRemoveFailed); }
                } else {
                    picker_kind = static_cast<unsigned>(configuration.selected);
                    try {
                        winrt::Windows::Storage::Pickers::FolderPicker folder_picker;
                        folder_picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                        folder_picker.FileTypeFilter().Append(L"*");
                        picker = folder_picker.PickSingleFolderAsync();
                        configuration.notice = Tr(picker_kind == 0 ? T::PickGamesFolder :
                                                  picker_kind == 1 ? T::PickKeysFolder : T::PickFirmwareFolder);
                    } catch (...) { configuration.notice = Tr(T::PickerOpenFailed); }
                }
                dirty = true;
            }
            actions = 0;
        }
        if (!settings) panel.notice.clear();
        if (!panel.keyboard.open) panel.keyboard.notice.clear();
        if (loading && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            // A rescan keeps the game the player was on; the first scan starts at the last one played.
            const auto kept = selected < scan.entries.size() ? scan.entries[selected].launch_path : std::string{};
            try {
                auto rescanned = future.get();
                if (keep_titles) KeepKnownTitles(scan.entries, rescanned.entries);
                scan = std::move(rescanned);
                if (keep_titles && *keep_titles) forget_content(*keep_titles);
            }
            catch (const std::exception& e) {
                LOG_WARNING(Frontend, "Library scan failed: {}", e.what());
                scan.error = I18n::ToUtf8(Tr(T::LibraryScanFailed));
            } catch (const winrt::hresult_error& e) {
                LOG_WARNING(Frontend, "Library scan failed: {:08X} {}", static_cast<std::uint32_t>(e.code()),
                            winrt::to_string(e.message()));
                scan.error = I18n::ToUtf8(Tr(T::LibraryScanFailed));
            }
            loading = false;
            keep_titles.reset();
            refresh_play(kept);
            refresh_pulse();
            if (!configuration.busy) refresh_setup();
            notice = Wide(scan.error);
            if (notice.empty() && !initial_notice.empty()) notice = Wide(initial_notice);
            initial_notice = {};
            if (notice.empty()) notice = std::move(after_scan);
            after_scan.clear();
            if (scan.limited) notice = Tr(T::ScanLimit);
            dirty = true;
        }
        if (!auto_pick.empty() && !configuration.open && !loading && !picker && !validation.valid()) {
            if (auto_pick_at == std::chrono::steady_clock::time_point{})
                auto_pick_at = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            if (std::chrono::steady_clock::now() >= auto_pick_at) {
                for (size_t i = 0; i < scan.entries.size(); ++i) {
                    const auto path = scan.entries[i].relative_path.u8string();
                    if (scan.entries[i].launch_path == auto_pick ||
                        (!IsStoragePath(scan.entries[i].launch_path) &&
                         std::string_view{reinterpret_cast<const char*>(path.data()), path.size()} == auto_pick)) {
                        selected = i; settings = false; actions |= Play;
                    }
                }
            }
        }
        if (actions & Play && on_game && !configuration.open && !settings && !panel.keyboard.open && !loading &&
            !scan.entries.empty() && !network_portal.BlockForLocalOperation()) {
            notice=Tr(T::NetworkBusy); actions &= ~Play; dirty=true;
        }
        if (actions & Play && on_game && !configuration.open && !settings && !panel.keyboard.open && !loading &&
            !scan.entries.empty()) {
            network_portal.Stop();
            selected_path = scan.entries[selected].launch_path;
            notice = Tr(T::CheckingGame);
            dirty = true;
            metadata_worker.request_stop();
            metadata = {};
            std::packaged_task<std::string()> task{[path = selected_path, old_metadata = std::move(metadata_worker)]() mutable {
                if (old_metadata.joinable()) old_metadata.join();
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                if (!HasUsableHeaderKey() && !path.ends_with(".nro"))
                    return I18n::ToUtf8(Tr(T::MissingKeys));
                if (IsStoragePath(path)) {
                    if (const auto error = CheckExternalGame(path); !error.empty()) return error;
                }
                if (path.ends_with(".nro")) return std::string{};
                Core::System system;
                system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
                system.SetFilesystem(MakeUwpFilesystem());
                system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
                FileSys::ManualContentProvider provider;
                const auto resolved = Content::ResolveLaunchContent(*system.GetFilesystem(), path, provider, false, {}, Content::ReportedSystemVersion(system));
                if (resolved.error == Content::Error::Unavailable) return std::string{"content-missing"};
                return resolved.error == Content::Error::None ? std::string{} : I18n::ToUtf8(Tr(Content::ErrorText(resolved.error)));
            }};
            validation = task.get_future();
            validation_worker = std::jthread(std::move(task));
            actions = 0;
        }
        if (settings) {
            if (panel.keyboard.open) { actions &= ~(Swap | Zone); }
            else if (panel.expanded) {
                const size_t count = panel.devices.size() + 2;
                if (actions & Up) panel.choice = (panel.choice + count - 1) % count;
                if (actions & Down) panel.choice = (panel.choice + 1) % count;
                if (actions & (Up | Down)) dirty = true;
                if (actions & Play) {
                    if (panel.choice < 2 || (bool(panel.devices[panel.choice - 2]) &&
                        !panel.devices[panel.choice - 2].id.starts_with(L"session:"))) {
                        options.controller_id = panel.choice == 0 ? L"" : panel.choice == 1 ?
                            std::wstring{KeyboardControllerId} : panel.devices[panel.choice - 2].id;
                        device_changed = true;
                        panel.expanded = false;
                    }
                    dirty = true;
                }
                actions &= ~(Swap | Zone);
            } else {
                // Rows: [device (PC)], controller type, A/B/X/Y, deadzone, [keyboard (PC)].
                const unsigned offset = panel.xbox ? 0 : 1;
                const unsigned rows = panel.xbox ? 3 : 5;
                if (actions & Up) setting_row = (setting_row + rows - 1) % rows;
                if (actions & Down) setting_row = (setting_row + 1) % rows;
                if (actions & (Up | Down)) dirty = true;
                const bool opens = !panel.xbox && (setting_row == 0 || setting_row == 4);
                if (opens && actions & Left && !(actions & (Play | Right))) {
                    if (configuration.open) { settings = return_to_configuration = false; dirty = true; }
                } else if (actions & (Play | Left | Right)) {
                    if (!panel.xbox && setting_row == 4) {
                        panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
                    } else if (!panel.xbox && setting_row == 0) {
                        panel.expanded = true;
                        panel.choice = options.controller_id == KeyboardControllerId ? 1 : 0;
                        for (size_t i = 0; i < panel.devices.size(); ++i)
                            if (panel.devices[i].id == options.controller_id) panel.choice = i + 2;
                        dirty = true;
                    } else if (setting_row == offset) {
                        options.style = StepConsoleControllerStyle(options.style, (actions & Left) != 0);
                        device_changed = true; dirty = true;
                    } else if (setting_row == offset + 1) {
                        actions |= Swap;
                    } else if (actions & Left) {
                        // Back along the 8 / 12 / 18 % segments; A and Right go forward.
                        options.deadzone = options.deadzone < 0.10f ? 0.18f : options.deadzone < 0.15f ? 0.08f : 0.12f;
                        device_changed = true; dirty = true;
                    } else actions |= Zone;
                }
            }
        } else {
            actions &= ~(Swap | Zone);
            using LibraryLayout::Move;
            const auto move = [&](Move direction) {
                const auto moved = LibraryLayout::Moved({selected, rail}, direction, scan.entries.size(),
                                                        LibraryLayout::RailCount(scan.entries));
                if (moved.selected == selected && moved.rail == rail) return;
                selected = moved.selected;
                rail = moved.rail;
                dirty = true;
            };
            if (tab == LibraryTab::Home) {
                if (actions & Left) move(Move::Left);
                if (actions & Right) move(Move::Right);
                if (actions & Up) move(Move::Up);
                if (actions & Down) move(Move::Down);
            } else if (tab == LibraryTab::Library && !loading) {
                const size_t count = shelf.size() + (LibraryLayout::HasImportTile(filter) ? 1 : 0);
                if (filter_focus) {
                    if (actions & (Left | Right)) {
                        constexpr auto filters = static_cast<unsigned>(LibraryLayout::FilterCount);
                        const auto current = static_cast<unsigned>(filter);
                        filter = static_cast<Filter>(actions & Left ? (current + filters - 1) % filters
                                                                    : (current + 1) % filters);
                        shelf = LibraryLayout::Shelf(scan.entries, filter);
                        shelf_focus = 0;
                        dirty = true;
                    }
                    if (actions & Down && count) { filter_focus = false; dirty = true; }
                } else {
                    const auto shelf_move = [&](Move direction) {
                        const auto moved = LibraryLayout::ShelfMoved(shelf_focus, direction, count);
                        if (moved != shelf_focus) { shelf_focus = moved; dirty = true; }
                    };
                    if (actions & Up && shelf_focus < LibraryLayout::Columns) { filter_focus = true; dirty = true; }
                    else if (actions & Up) shelf_move(Move::Up);
                    if (actions & Down) shelf_move(Move::Down);
                    if (actions & Left) shelf_move(Move::Left);
                    if (actions & Right) shelf_move(Move::Right);
                }
                if (shelf_focus < shelf.size()) selected = shelf[shelf_focus];
            }
        }
        // Pinning the focused game: first in Home's rail and under the Pinned filter.
        if (actions & Pin && on_game && !settings && !configuration.open && !game_options.open && !loading &&
            selected < scan.entries.size()) {
            auto& entry = scan.entries[selected];
            entry.pinned = !entry.pinned;
            if (entry.pinned) favorites.insert(entry.launch_path);
            else favorites.erase(entry.launch_path);
            if (!SaveFavorites(favorites)) notice = Tr(T::PinSaveFailed);
            const auto focused_shelf = shelf_focus;
            refresh_play(std::string{entry.launch_path});
            // On a filtered shelf the focus stays where it was, not where the game moved.
            if (tab == LibraryTab::Library) {
                shelf_focus = std::min(focused_shelf, shelf.empty() ? 0 : shelf.size() - 1);
                if (shelf_focus < shelf.size()) selected = shelf[shelf_focus];
            }
        }
        if (actions & Swap) { options.swap_face_buttons = !options.swap_face_buttons; dirty = true; }
        if (actions & Zone) {
            options.deadzone = options.deadzone < 0.10f ? 0.12f : options.deadzone < 0.15f ? 0.18f : 0.08f;
            dirty = true;
        }
        if (device_changed || actions & (Swap | Zone)) {
            device_changed = false;
            const bool saved = SaveControllerOptions(options);
            panel.notice = Tr(saved ? T::SettingsSaved : T::ControllerSaveFailed);
            controller_notice_until = saved ? now + std::chrono::seconds{3} :
                                             std::chrono::steady_clock::time_point::max();
        }
        if (actions & Refresh && !loading && !picker && !validation.valid()) start_scan();
        if (!configuration.busy && metadata.valid() && metadata.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try {
                auto batch = metadata.get();
                if (batch.generation == generation) {
                    // Merge field by field: a names-only batch must not drop icons already shown.
                    const auto covers = covers_for_tab();
                    for (auto& entry : batch.entries) {
                        auto found = std::find_if(scan.entries.begin(), scan.entries.end(), [&](const auto& e) {
                            return e.launch_path == entry.launch_path;
                        });
                        if (found == scan.entries.end()) continue;
                        const auto index = static_cast<size_t>(found - scan.entries.begin());
                        // An interrupted background batch is resumed from its first unread game.
                        if (batch.stopped && !batch.icons && !entry.title_loaded) {
                            names_cursor = std::min(names_cursor, index);
                            fill_cursor = std::min(fill_cursor, index);
                        }
                        found->name = std::move(entry.name);
                        found->developer = std::move(entry.developer);
                        if (entry.title_loaded) found->display_version = std::move(entry.display_version);
                        if (entry.title_loaded) {
                            found->program_id = entry.program_id;
                            found->build_id = entry.build_id;
                        }
                        found->title_loaded |= entry.title_loaded;
                        // The player may have paged on meanwhile: keep icons only inside the window.
                        if (batch.icons && covers.shown[index])
                            found->icon = std::move(entry.icon);
                    }
                    dirty = true;
                }
            } catch (const std::exception&) { notice = Tr(T::MetadataFailed); }
        }
        // Metadata, one worker at a time and in priority order: the covers around the cursor, then
        // every title the cache already has, then the rest of the library into the cache.
        const auto covers = covers_for_tab();
        const size_t page = covers.key;
        // The player moved to other rows: background work yields to its covers. Behind the file
        // manager no cover is shown, but titles still fill in (the portal lists them).
        const bool covers_due = !configuration.open && (metadata_dirty || retained_page != page);
        if (metadata.valid() && metadata_background && covers_due)
            metadata_worker.request_stop();
        if (!configuration.busy && !setup_future.valid() && !content_session && !validation.valid() && !missing_content && !loading && !metadata.valid() && !scan.entries.empty()) {
            MetadataOptions request{title_language};
            std::vector<LibraryEntry> requested;
            if (covers_due) {
                metadata_dirty = false;
                retained_page = page;
                for (size_t i = 0; i < scan.entries.size(); ++i) {
                    auto& entry = scan.entries[i];
                    if (!covers.shown[i] && entry.metadata_loaded) {
                        std::vector<unsigned char>{}.swap(entry.icon);
                        entry.metadata_loaded = false;
                    }
                }
                // The focused row first, then the next ones (where the player usually goes), the
                // rail, and the row above.
                for (const size_t i : covers.order) {
                    auto& entry = scan.entries[i];
                    if (entry.metadata_loaded) continue;
                    entry.metadata_loaded = true;
                    requested.push_back(entry);
                }
                request.icons = true;
            } else {
                // Titles without icons: what the grid and the cache need, never a cover's bytes.
                const auto take = [&](size_t& cursor, size_t batch) {
                    while (cursor < scan.entries.size() && requested.size() < batch) {
                        const auto& entry = scan.entries[cursor++];
                        if (!entry.title_loaded)
                            requested.push_back({entry.relative_path, entry.name, {}, {}, false, false,
                                                 entry.launch_path, entry.source_name});
                    }
                };
                if (names_cursor < scan.entries.size()) {
                    take(names_cursor, NamesBatch);
                    request.cache_only = true;
                } else {
                    take(fill_cursor, FillBatch);
                }
            }
            if (!requested.empty()) {
                if (!logging_ready) { Common::Log::Initialize(); logging_ready = true; }
                std::packaged_task<MetadataBatch(std::stop_token)> task{
                    [root, &cache, requested = std::move(requested), generation, request](std::stop_token stop) mutable {
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                        ReadLibraryMetadata(root, requested, cache, request, stop);
                        return MetadataBatch{std::move(requested), generation, request.icons,
                                             stop.stop_requested()};
                    }};
                metadata_background = !request.icons;
                metadata = task.get_future();
                metadata_worker = std::jthread(std::move(task));
            }
        }
        actions = 0;
        if (resize) {
            canvas.reset();
            canvas = std::make_unique<LibraryCanvas>(core_window, width, height);
            resize = false;
            dirty = true;
        }
        const bool draw = dirty || canvas->Animating();
        if (draw) {
            const GameOptionsView options_view{
                game_options, game_options.open && !configuration.open && game_options.game < scan.entries.size() ? &scan.entries[game_options.game] : nullptr,
                cheat_session ? &*cheat_session : nullptr, cheats_page, options_notice, content_session.get(),
                mod_session.get(), &mods_page,
                game_options.open && game_options.game < scan.entries.size() &&
                    FindDumpFiles(scan.entries, scan.entries[game_options.game].launch_path).kept};
            // Titles, Pulse sessions and plays arrive after the shelf was made: keep it current,
            // the focus on the same game.
            if (tab == LibraryTab::Library && !loading) {
                const auto focused = shelf_focus < shelf.size() ? shelf[shelf_focus] : SIZE_MAX;
                shelf = LibraryLayout::Shelf(scan.entries, filter);
                const size_t count = shelf.size() + (LibraryLayout::HasImportTile(filter) ? 1 : 0);
                shelf_focus = std::min(shelf_focus, count ? count - 1 : 0);
                for (size_t i = 0; i < shelf.size(); ++i)
                    if (shelf[i] == focused) shelf_focus = i;
                if (shelf_focus < shelf.size()) selected = shelf[shelf_focus];
            }
            canvas->Draw({scan, selected, loading, settings, setting_row, options, notice, panel,
                          configuration, system_page, options_view, missing_content, rail, tab, filter,
                          &shelf, shelf_focus, filter_focus, battery});
            dirty = false;
        }
        // A drawn frame already waited for the display in Present(1); sleeping after it too made
        // every press answer a frame late. Idle, poll the pad every half frame.
        if (!draw) std::this_thread::sleep_for(std::chrono::milliseconds{8});
    }
}
} // namespace Jadyx
