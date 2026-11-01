// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"

namespace Jadyx {
using I18n::Tr;

// Saves and applies the System page; the interface switches language on the next frame.
void LibrarySession::ApplySystemEffect(SystemPageEffect effect, TimePoint now) {
    switch (effect) {
    case SystemPageEffect::Changed:
    case SystemPageEffect::NameDone: {
        const bool saved = SaveSystemSettings(system_page.settings);
        const auto resolved = ApplySystemSettings();
        system_page.notice = Tr(saved ? T::SettingsSaved : T::SystemSaveFailed);
        system_notice_until = saved ? now + std::chrono::seconds{3} : std::chrono::steady_clock::time_point::max();
        if (resolved.game_language != title_language) {
            title_language = resolved.game_language;
            ReloadMetadata();
        }
        if (system_page.settings.track_play_time != play_order && !loading)
            RefreshPlay(selected < scan.entries.size() ? scan.entries[selected].launch_path : std::string{});
        break;
    }
    case SystemPageEffect::OpenUpdates:
        system_page.updates_open = true; system_page.update_row = 0; break;
    case SystemPageEffect::ClearCache: {
        ReloadMetadata();
        canvas->InvalidateCovers();
        // The mods store's pictures, index and unfinished downloads go with it.
        std::error_code ec;
        std::filesystem::remove_all(Mods::Web::CacheFolder(), ec);
        system_page.notice = Tr(cache.Clear() ? T::LibraryCacheCleared : T::SystemSaveFailed);
        system_notice_until = now + std::chrono::seconds{3};
        break;
    }
    case SystemPageEffect::OpenMiiEditor:
        if (!HasUsableHeaderKey()) {
            system_page.notice = Tr(T::MissingKeys);
            system_notice_until = now + std::chrono::seconds{3};
        } else if (!network_portal.BlockForLocalOperation()) {
            system_page.notice = Tr(T::NetworkBusy);
            system_notice_until = now + std::chrono::seconds{3};
        } else {
            open_mii_editor = true;
        }
        break;
    case SystemPageEffect::None:
    case SystemPageEffect::EditName:
        break;
    }
    dirty = true;
}

void LibrarySession::HandlePicker() {
    if (picker) {
        actions = 0; // The system picker owns input until it returns.
        if (picker.Status() != winrt::Windows::Foundation::AsyncStatus::Started) {
            try {
                auto folder = picker.GetResults();
                if (folder && picker_kind == 0) StartScan(folder);
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
                        [folder, kind = picker_kind, old_metadata = std::move(metadata_worker), &progress = import_progress](std::stop_token stop) mutable {
                            if (old_metadata.joinable()) old_metadata.join();
                            winrt::init_apartment(winrt::apartment_type::multi_threaded);
                            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                            return ImportSystemFiles(folder, kind == 1 ? FileImport::Keys : FileImport::Firmware, progress, stop);
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
}

    // Switching tabs: the bumpers (Q/E) at the top level, or a press on the header.
void LibrarySession::SwitchTab(LibraryTab to) {
    if (to == tab) return;
    if (tab == LibraryTab::Settings) {
        system_page.open = graphics_page.open = false;
        configuration.open = configuration.files = false;
        settings = return_to_configuration = false;
        panel.expanded = panel.keyboard.open = panel.keyboard.capturing = false;
    }
    tab = to;
    if (to == LibraryTab::Settings) {
        configuration.open = true; configuration.files = false; configuration.selected = HomeSystem;
        configuration.notice.clear(); RefreshSetup();
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
}

void LibrarySession::HandlePressedTab(bool top_level) {
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
            if (changed) ForgetContent(*changed);
            SwitchTab(*pressed_tab);
        }
        if (top_level || *pressed_tab == LibraryTab::Settings || tab == LibraryTab::Settings) {
            if (!game_options.open && !configuration.busy && !picker && !validation.valid()) SwitchTab(*pressed_tab);
        }
        pressed_tab.reset();
    }
}

void LibrarySession::HandleTabBumpers(bool top_level) {
    const bool settings_page = tab == LibraryTab::Settings && (settings || system_page.open || graphics_page.open || configuration.files) &&
                               !panel.keyboard.open && !panel.expanded && !system_page.name_editor &&
                               !configuration.busy && !picker && !validation.valid() && !game_options.open &&
                               !missing_content;
    if ((top_level || settings_page) && (actions & (PreviousTab | NextTab))) {
        constexpr auto count = static_cast<unsigned>(LibraryTab::Count);
        const auto current = static_cast<unsigned>(tab);
        const auto to = actions & PreviousTab ? (current + count - 1) % count : (current + 1) % count;
        SwitchTab(static_cast<LibraryTab>(to));
        actions &= ~(PreviousTab | NextTab);
    }
}

bool LibrarySession::HandleSystemPage(TimePoint now) {
    if (system_page.open && configuration.open && !settings) {
        // The name editor takes its input in the handlers above; the page waits behind it.
        if (!system_page.name_editor && actions) {
            const auto send = [&](unsigned bit, SystemInput input) {
                if (actions & bit) ApplySystemEffect(HandleSystemInput(system_page, input), now);
            };
            send(Up, SystemInput::Up);
            send(Down, SystemInput::Down);
            const bool opens = system_page.selected == SystemPage::UserName ||
                               system_page.selected == SystemPage::ClearCache || system_page.selected == SystemPage::Updates ||
                               system_page.selected == SystemPage::MiiEditor;
            send(Left, opens ? SystemInput::Back : SystemInput::Left);
            send(Right, SystemInput::Right);
            send(Play, SystemInput::Confirm);
            send(Quit, SystemInput::Back);
            if (!system_page.open) configuration.selected = HomeSystem;
        }
        actions = 0;
        if (open_mii_editor) {
            network_portal.Stop();
            if (selected_program_id) *selected_program_id = 0;
            launch = std::string{MiiEditorLaunch};
            return true;
        }
    }
    return false;
}

void LibrarySession::HandleGraphicsPage(TimePoint now) {
    if (graphics_page.open && configuration.open && !settings) {
        // Saved for the next game; a game's own Graphics pane can override each row.
        const auto send = [&](unsigned bit, GraphicsInput input) {
            if (!(actions & bit) || !HandleGraphicsInput(graphics_page, input)) return;
            const bool saved = SaveGraphicsSettings(graphics_page.settings);
            system_page.notice = Tr(saved ? T::SettingsSaved : T::SystemSaveFailed);
            system_notice_until = saved ? now + std::chrono::seconds{3} : std::chrono::steady_clock::time_point::max();
        };
        send(Up, GraphicsInput::Up);
        send(Down, GraphicsInput::Down);
        send(Left, GraphicsInput::Left);
        send(Right, GraphicsInput::Right);
        send(Play, GraphicsInput::Confirm);
        send(Quit, GraphicsInput::Back);
        if (!graphics_page.open) configuration.selected = HomeGraphics;
        if (actions) dirty = true;
        actions = 0;
    }
}

bool LibrarySession::HandleQuit() {
    if (actions & Quit) {
        if (panel.keyboard.open) { panel.keyboard.open = panel.keyboard.capturing = false; dirty = true; actions &= ~Quit; }
        else if (panel.expanded) { panel.expanded = false; dirty = true; actions &= ~Quit; }
        else if (settings) { settings = false; configuration.open = return_to_configuration; return_to_configuration = false; dirty = true; actions &= ~Quit; }
        else if (configuration.open) {
            if (configuration.files) { configuration.files = false; configuration.selected = HomeFiles; }
            else configuration.open = false;
            dirty = true; actions &= ~Quit;
        }
        else if (tab == LibraryTab::Library) { SwitchTab(LibraryTab::Home); actions &= ~Quit; }
        else return true;
    }
    return false;
}

void LibrarySession::HandlePageShortcuts() {
    if (actions & Panel && !configuration.open) { settings = !settings; panel.keyboard.open = panel.keyboard.capturing = false; panel.expanded = false; dirty = true; }
    if (actions & AddFolder && !settings && !picker && !configuration.busy && !validation.valid() &&
        !configuration.open)
        SwitchTab(LibraryTab::Settings);
    // The import tile opens the file manager on the Settings tab.
    if (actions & Play && tab == LibraryTab::Library && !filter_focus && shelf_focus >= shelf.size() &&
        LibraryLayout::HasImportTile(filter) && !settings && !configuration.open && !picker &&
        !validation.valid()) {
        SwitchTab(LibraryTab::Settings);
        configuration.files = true;
        configuration.selected = 0;
        actions &= ~Play;
    }
    // The configuration closed itself (Back): its tab goes with it.
    if (tab == LibraryTab::Settings && !configuration.open && !settings) { tab = LibraryTab::Home; dirty = true; }
}

void LibrarySession::HandleConfigurationPage() {
    if (configuration.open && !settings && !picker && !configuration.busy) {
        const auto count = ConfigurationRowCount(configuration);
        if (actions & Up) configuration.selected = (configuration.selected + count - 1) % count;
        if (actions & Down) configuration.selected = (configuration.selected + 1) % count;
        if (actions & (Up | Down)) dirty = true;
        if (configuration.files && actions & Left) {
            configuration.files = false; configuration.selected = HomeFiles; dirty = true;
        } else if (actions & Play || (!configuration.files && actions & Right)) {
            if (!configuration.files) OpenHomeCategory();
            else ActivateFilesRow();
            dirty = true;
        }
        actions = 0;
    }
}

void LibrarySession::OpenHomeCategory() {
    switch (configuration.selected) {
    case HomeFiles: configuration.files = true; configuration.selected = 0; break;
    case HomeControllers: settings = true; return_to_configuration = true; setting_row = 0; break;
    case HomeGraphics:
        graphics_page.open = true;
        graphics_page.selected = 0;
        system_page.notice.clear();
        break;
    default:
        system_page.open = true;
        system_page.selected = SystemPage::UserName;
        system_page.notice.clear();
        // Windows' language or region may have changed since the library opened.
        system_page.locale = DetectSystemLocale();
        break;
    }
}

void LibrarySession::ActivateFilesRow() {
    if (loading || setup_future.valid()) configuration.notice = Tr(T::WaitForCheck);
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
            configuration.network_action = 1;
            configuration.notice.clear();
        } else {
            configuration.network_screen = StartPortal();
            configuration.network_action = 1;
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
            configuration.selected = 0; RefreshSetup(); StartScan();
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
}

void LibrarySession::HandleControllerPanel() {
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
}

void LibrarySession::HandleNavigation() {
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
void LibrarySession::HandlePin(bool on_game) {
    if (actions & Pin && on_game && !settings && !configuration.open && !game_options.open && !loading &&
        selected < scan.entries.size()) {
        auto& entry = scan.entries[selected];
        entry.pinned = !entry.pinned;
        if (entry.pinned) favorites.insert(entry.launch_path);
        else favorites.erase(entry.launch_path);
        if (!SaveFavorites(favorites)) notice = Tr(T::PinSaveFailed);
        const auto focused_shelf = shelf_focus;
        RefreshPlay(std::string{entry.launch_path});
        // On a filtered shelf the focus stays where it was, not where the game moved.
        if (tab == LibraryTab::Library) {
            shelf_focus = std::min(focused_shelf, shelf.empty() ? 0 : shelf.size() - 1);
            if (shelf_focus < shelf.size()) selected = shelf[shelf_focus];
        }
    }
}

void LibrarySession::SaveControllerChoices(TimePoint now) {
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
}

void LibrarySession::Draw() {
    const GameOptionsView options_view{
        game_options, game_options.open && !configuration.open && game_options.game < scan.entries.size() ? &scan.entries[game_options.game] : nullptr,
        cheat_session ? &*cheat_session : nullptr, cheats_page, options_notice, content_session.get(),
        mod_session.get(), &mods_page,
        game_options.open && game_options.game < scan.entries.size() &&
            FindDumpFiles(scan.entries, scan.entries[game_options.game].launch_path).kept,
        game_options.open && game_options.game < scan.entries.size() && game_graphics_id != 0 &&
                game_graphics_id == scan.entries[game_options.game].program_id ? &game_graphics : nullptr,
        game_graphics_row, &graphics_page.settings};
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
    canvas->Draw({.scan = scan,
                  .selected = selected,
                  .loading = loading,
                  .controller_panel = settings,
                  .controller_row = setting_row,
                  .options = options,
                  .notice = notice,
                  .panel = panel,
                  .configuration = configuration,
                  .system = system_page,
                  .graphics = graphics_page,
                  .game_options = options_view,
                  .missing_content = missing_content,
                  .rail = rail,
                  .tab = tab,
                  .filter = filter,
                  .shelf = &shelf,
                  .shelf_focus = shelf_focus,
                  .filter_focus = filter_focus,
                  .battery = battery,
                  .app_mode = app_mode,
                  .app_memory_mib = app_memory_limit >> 20});
    dirty = false;
}

} // namespace Jadyx
