// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"
#include "jadyx/library/controller_sections.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/input/dsu_client.h"
#include "jadyx/input/dsu_text.h"
#include "core/file_sys/common_funcs.h"
#include "jadyx/network/nextendo/service.h"
#include "jadyx/network/room_ping.h"

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
    if (tab == LibraryTab::Online) {
        online_page.open = false;
        if (system_page.editor_purpose != EditorPurpose::UserName) system_page.name_editor.reset();
        online_page.pending_room.reset();
        system_page.notice.clear();
    }
    if (tab == LibraryTab::Settings) {
        system_page.open = graphics_page.open = false;
        configuration.open = configuration.files = false;
        settings = return_to_configuration = false;
        panel.expanded = false;
        panel.CloseBindingEditors();
    }
    tab = to;
    if (to == LibraryTab::Online) {
        online_page.open = true;
        system_page.notice.clear();
        PollOnline();
        // While this console hosts a room, the tab opens on it.
        if (OnlineHosting(online_page.status)) online_page.host = true;
        // While play goes through Nextendo, the tab opens on Nextendo instead of the rooms.
        else if (Network::Nextendo::ServicePreferences().use_nextendo && !online_page.nextendo)
            ShowOnlineNetwork(true);
        // The public list downloads when the tab first opens, and again after a failure; the
        // refresh button downloads it whenever asked.
        if (online_page.lobby.state == Online::LobbyState::Empty || online_page.lobby.state == Online::LobbyState::Failed)
            Online::RefreshLobby();
    }
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
                               !panel.BindingEditorOpen() && !panel.expanded && !system_page.name_editor &&
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
            const bool opens = !system_page.stats_open &&
                               (system_page.selected == SystemPage::UserName || system_page.selected == SystemPage::PerformanceStats ||
                               system_page.selected == SystemPage::ClearCache || system_page.selected == SystemPage::Updates ||
                               system_page.selected == SystemPage::MiiEditor);
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
        // Y: everything back to the emulator's defaults, from any of its lists.
        if (graphics_page.open && actions & Refresh) {
            const bool saved = !ResetGraphicsSettings(graphics_page.settings) ||
                               SaveGraphicsSettings(graphics_page.settings);
            system_page.notice = Tr(saved ? T::GraphicsResetDone : T::SystemSaveFailed);
            system_notice_until = saved ? now + std::chrono::seconds{3} : std::chrono::steady_clock::time_point::max();
        }
        if (!graphics_page.open) configuration.selected = HomeGraphics;
        if (actions) dirty = true;
        actions = 0;
    }
}

bool LibrarySession::HandleQuit() {
    if (actions & Quit) {
        if (panel.BindingEditorOpen()) { CloseBindingEditor(); actions &= ~Quit; }
        else if (panel.expanded) { panel.expanded = false; dirty = true; actions &= ~Quit; }
        else if (settings && controller_section) {
            // A section goes back to the main page, on its entry.
            setting_row = ControllerSectionEntry + controller_section - 1;
            controller_section = 0; dirty = true; actions &= ~Quit;
        }
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
    if (actions & Panel && !configuration.open) { settings = !settings; controller_section = 0; panel.CloseBindingEditors(); panel.expanded = false; dirty = true; }
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
    case HomeControllers: settings = true; return_to_configuration = true; setting_row = 0; controller_section = 0; break;
    case HomeGraphics:
        graphics_page.open = true;
        graphics_page.cursor = {};
        system_page.notice.clear();
        break;
    default:
        system_page.open = true;
        system_page.selected = SystemPage::UserName;
        system_page.stats_open = false;
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
    if (panel.BindingEditorOpen()) { actions &= ~(Swap | Zone); }
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
        // The main page's rows: device, controller type, A/B/X/Y, deadzone, keyboard, controller
        // mapping, then the entries of the right stick's, the mouse's, the rumble's and the gyro's
        // sections (ControllerSection). The floating panel outside the Settings tab draws only
        // the first six.
        if (tab != LibraryTab::Settings) controller_section = 0;
        const std::vector<DsuRowRef> dsu_rows = DsuRows(dsu_status);
        const unsigned rows = controller_section ? ControllerSectionRows(controller_section, options, dsu_rows.size()) :
                              tab == LibraryTab::Settings ? ControllerSectionEntry + ControllerSectionCount : 6;
        if (setting_row >= rows) setting_row = 0;
        if (actions & Up) setting_row = (setting_row + rows - 1) % rows;
        if (actions & Down) setting_row = (setting_row + 1) % rows;
        if (actions & (Up | Down)) dirty = true;
        const bool back = (actions & Left) && !(actions & (Play | Right));
        const bool entry = !controller_section && setting_row >= ControllerSectionEntry;
        const bool opens = !controller_section && (setting_row == 0 || setting_row == 4 || setting_row == 5 || entry);
        if (opens && back) {
            if (configuration.open) { settings = return_to_configuration = false; dirty = true; }
        } else if (entry && actions & (Play | Right)) {
            controller_section = setting_row - ControllerSectionEntry + 1;
            setting_row = 0; dirty = true;
        } else if (controller_section == SectionDsu) {
            // The DSU page: A searches or types an address; on a server X removes it; on a pad
            // left, right and A step who it plays. X never reaches A/B/X/Y's swap from here.
            const ControllerSettingRow row = ControllerSectionRow(controller_section, setting_row, options);
            const std::optional<DsuRowRef> ref =
                row == ControllerSettingRow::DsuEntry && setting_row - 2 < dsu_rows.size()
                    ? std::optional{dsu_rows[setting_row - 2]} : std::nullopt;
            if (row == ControllerSettingRow::DsuSearch && actions & Play) {
                DsuClient::Search();
                dsu_status = DsuClient::Status();
                dirty = true;
            } else if (row == ControllerSettingRow::DsuAdd && actions & Play) {
                OpenOnlineEditor(EditorPurpose::DsuAddress);
                dirty = true;
            } else if (ref && !ref->pad && actions & Swap) {
                DsuClient::Remove(ref->server);
                dsu_status = DsuClient::Status();
                dirty = true;
            } else if (ref && ref->pad && actions & (Play | Left | Right)) {
                const DsuPadStatus& dsu_pad = dsu_status.servers[ref->server].pads[*ref->pad];
                DsuClient::SetAssign(dsu_pad.key, StepDsuAssign(dsu_pad.assign, back));
                dsu_status = DsuClient::Status();
                dirty = true;
            }
            actions &= ~Swap;
        } else if (controller_section && actions & (Play | Left | Right) &&
                   ControllerSectionRow(controller_section, setting_row, options) == ControllerSettingRow::GyroDsuPad) {
            // Which DSU pad moves the gyro: Automatic, then each pad that sends one.
            DsuClient::SetGyroPad(StepGyroDsuPad(dsu_status, back));
            dsu_status = DsuClient::Status();
            dirty = true;
        } else if (controller_section && actions & (Play | Left | Right)) {
            auto& stick = options.right_stick;
            switch (ControllerSectionRow(controller_section, setting_row, options)) {
            case ControllerSettingRow::InvertX: stick.invert_x = !stick.invert_x; break;
            case ControllerSettingRow::InvertY: stick.invert_y = !stick.invert_y; break;
            case ControllerSettingRow::StickSensitivity:
                stick.stick_sensitivity = StepSensitivity(StickSensitivitySteps, stick.stick_sensitivity, back); break;
            case ControllerSettingRow::MouseUse:
                ApplyMouseUse(StepEnum(MouseUseOf(options.motion_source, stick.mouse), MouseUseCount, back),
                              options.motion_source, stick.mouse);
                break;
            case ControllerSettingRow::MouseSensitivity:
                stick.mouse_sensitivity = StepSensitivity(MouseSensitivitySteps, stick.mouse_sensitivity, back); break;
            case ControllerSettingRow::Rumble: options.rumble.on = !options.rumble.on; break;
            case ControllerSettingRow::RumbleStrength:
                options.rumble.strength = StepSensitivity(RumbleStrengthSteps, options.rumble.strength, back); break;
            case ControllerSettingRow::GyroSource: {
                const auto source = StepEnum(options.motion_source, MotionSourceCount, back);
                if (source == MotionSource::Mouse) ApplyMouseUse(MouseUse::Gyro, options.motion_source, stick.mouse);
                else options.motion_source = source;
                break;
            }
            case ControllerSettingRow::GyroShake:
                options.motion_shake = StepEnum(options.motion_shake, MotionShakeButtonCount, back); break;
            case ControllerSettingRow::GyroDsuPad:
            case ControllerSettingRow::DsuSearch:
            case ControllerSettingRow::DsuAdd:
            case ControllerSettingRow::DsuEntry: break;
            }
            device_changed = true; dirty = true;
        } else if (actions & (Play | Left | Right)) {
            if (setting_row == 4) {
                panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
            } else if (setting_row == 5) {
                OpenPadPicker();
            } else if (setting_row == 0) {
                panel.expanded = true;
                panel.choice = options.controller_id == KeyboardControllerId ? 1 : 0;
                for (size_t i = 0; i < panel.devices.size(); ++i)
                    if (panel.devices[i].id == options.controller_id) panel.choice = i + 2;
                dirty = true;
            } else if (setting_row == 1) {
                options.style = StepConsoleControllerStyle(options.style, (actions & Left) != 0);
                device_changed = true; dirty = true;
            } else if (setting_row == 2) {
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
    // A mapped pad's letters come from its mapping: there is nothing here to swap.
    if (actions & Swap && !(pad && pad->bindings)) { options.swap_face_buttons = !options.swap_face_buttons; dirty = true; }
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
        game_graphics_cursor, &graphics_page.settings};
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
                  .controller_section = controller_section,
                  .options = options,
                  .notice = notice,
                  .panel = panel,
                  .configuration = configuration,
                  .system = system_page,
                  .graphics = graphics_page,
                  .online = online_page,
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

void LibrarySession::PollDsu() {
    // Only while a page that lists it shows: the player sees servers and pads come up live.
    const bool shown = tab == LibraryTab::Settings && settings &&
                       (controller_section == SectionDsu ||
                        (controller_section == SectionGyro && options.motion_source == MotionSource::Phone));
    DsuClient::SetWanted(DsuClient::Wanter::Library, shown);
    if (const auto version = DsuClient::StatusVersion(); version != dsu_version) {
        dsu_version = version;
        dsu_status = DsuClient::Status();
        if (shown) dirty = true;
    }
}

// The Online tab. The room and the public list live in Jadyx::Online; the tab copies them while
// it shows, so a join answered on the network thread redraws the screen.
void LibrarySession::PollOnline() {
    // The connection on every tab: the header marks the Online tab while in a room.
    auto status = Online::Snapshot();
    if (status.version != online_page.status.version) {
        online_page.status = std::move(status);
        if (!OnlineChatShows(online_page)) online_page.in_chat = false;
        dirty = true;
    }
    PollOnlineChat();
    PollNextendo();
    if (tab != LibraryTab::Online) return;
    auto lobby = Online::Lobby();
    if (lobby.version != online_page.lobby.version) {
        auto& page = online_page;
        // The focus stays on its room when the list reorders (pings arrive) or refreshes.
        const auto* focused = OnlineRoomAt(page, OnlineInRooms(page) ? page.selected : OnlinePage::FixedRows + page.last_room);
        const auto moved = focused ? Online::FindRoom(lobby.rooms, *focused) : std::nullopt;
        page.lobby = std::move(lobby);
        if (moved) {
            if (OnlineInRooms(page)) page.selected = OnlinePage::FixedRows + *moved;
            page.last_room = *moved;
        }
        const std::size_t rooms = page.lobby.rooms.size();
        page.last_room = rooms ? std::min(page.last_room, rooms - 1) : 0;
        // A list that shrank under the focus hands it to its last room, or back to the form.
        if (OnlineInRooms(page) && page.selected >= OnlineRowCount(page))
            page.selected = rooms ? OnlinePage::FixedRows + rooms - 1 : page.last_form;
        dirty = true;
    }
}

void LibrarySession::HandleOnlinePage(TimePoint now) {
    if (tab != LibraryTab::Online || settings || game_options.open) return;
    // The keyboard takes its input in the handlers above; the page waits behind it.
    if (system_page.name_editor || !actions) {
        actions &= ~(Up | Down | Left | Right | Play | Refresh | Pin | Swap | AddFolder | Options);
        return;
    }
    auto& page = online_page;
    // LT/RT (PgUp/PgDn) swap the view between Eden rooms and Nextendo.
    if (actions & (PreviousManagerTab | NextManagerTab)) {
        ShowOnlineNetwork(!page.nextendo);
        system_page.notice.clear();
        system_notice_until = now;
        dirty = true;
        actions &= Panel;
        return;
    }
    if (page.nextendo) {
        HandleNextendoPage(now);
        dirty = true;
        // A game the list starts keeps A for StartValidation.
        actions &= nextendo_launch ? (Panel | Play) : Panel;
        return;
    }
    if (page.in_chat) {
        HandleOnlineChat();
        dirty = true;
        actions &= Panel;
        return;
    }
    // Y switches the left card between joining and hosting.
    if (actions & Pin) {
        page.host = !page.host;
        system_page.notice.clear();
        system_notice_until = now;
        dirty = true;
        actions &= Panel;
        return;
    }
    if (page.host) {
        HandleOnlineHost(now);
        dirty = true;
        actions &= Panel;
        return;
    }
    const std::size_t rooms = page.lobby.rooms.size();
    if (OnlineInRooms(page)) {
        const std::size_t room = page.selected - OnlinePage::FixedRows;
        if (actions & Up && room > 0) page.selected--;
        if (actions & Down && room + 1 < rooms) page.selected++;
        page.last_room = page.selected - OnlinePage::FixedRows;
        if (actions & Left) page.selected = page.last_form;
    } else {
        if (actions & Up && page.selected > 0) page.selected--;
        if (actions & Down && page.selected + 1 < OnlinePage::FixedRows) page.selected++;
        page.last_form = page.selected;
        if (actions & Right && OnlineChatShows(page)) {
            // In a room the right column is its chat.
            page.in_chat = true;
            page.chat = {};
            Online::MarkChatRead();
        } else if (actions & Right && rooms) {
            page.selected = OnlinePage::FixedRows + std::min(page.last_room, rooms - 1);
        }
    }
    if (actions & Refresh) Online::RefreshLobby();
    if (actions & Quit) {
        SwitchTab(LibraryTab::Home);
    } else if (actions & Play) {
        switch (page.selected) {
        case OnlinePage::Nickname: OpenOnlineEditor(EditorPurpose::OnlineNickname); break;
        case OnlinePage::Address: OpenOnlineEditor(EditorPurpose::OnlineAddress); break;
        case OnlinePage::Port: OpenOnlineEditor(EditorPurpose::OnlinePort); break;
        case OnlinePage::Password: OpenOnlineEditor(EditorPurpose::OnlinePassword); break;
        case OnlinePage::Connect:
            if (page.status.phase == Online::Phase::Joined || page.status.phase == Online::Phase::Joining) {
                Online::Leave();
            } else if (page.fields.address.empty()) {
                system_page.notice = Tr(T::OnlineNoAddress);
                system_notice_until = now + std::chrono::seconds{3};
            } else {
                JoinOnline(OnlineNarrow(page.fields.address), page.fields.port, OnlineNarrow(page.password), now);
            }
            break;
        default:
            if (const auto* room = OnlineRoomAt(page, page.selected)) {
                if (room->password) {
                    page.pending_room = *room;
                    OpenOnlineEditor(EditorPurpose::RoomPassword);
                } else {
                    JoinOnline(room->address, room->port, {}, now);
                }
            }
            break;
        }
    }
    dirty = true;
    // View still opens the controller panel, as on the other tabs; the rest belongs to this one.
    actions &= Panel;
}

// Host mode: the rows stack, except players and port, which share a line.
void LibrarySession::HandleOnlineHost(TimePoint now) {
    auto& page = online_page;
    auto& row = page.host_selected;
    if (actions & Up) {
        if (row == OnlinePage::HostPort) row = OnlinePage::HostName;
        else if (row == OnlinePage::HostPassword) row = OnlinePage::HostSlots;
        else if (row > OnlinePage::HostNickname) row--;
    }
    if (actions & Down) {
        if (row == OnlinePage::HostSlots) row = OnlinePage::HostPassword;
        else if (row + 1 < OnlinePage::HostRows) row++;
    }
    if (actions & Left && row == OnlinePage::HostPort) row = OnlinePage::HostSlots;
    if (actions & Right && row == OnlinePage::HostSlots) row = OnlinePage::HostPort;
    if (actions & Quit) {
        SwitchTab(LibraryTab::Home);
        return;
    }
    if (!(actions & Play)) return;
    switch (row) {
    case OnlinePage::HostNickname: OpenOnlineEditor(EditorPurpose::OnlineNickname); break;
    case OnlinePage::HostName: OpenOnlineEditor(EditorPurpose::HostName); break;
    case OnlinePage::HostSlots: OpenOnlineEditor(EditorPurpose::HostSlots); break;
    case OnlinePage::HostPort: OpenOnlineEditor(EditorPurpose::HostPort); break;
    case OnlinePage::HostPassword: OpenOnlineEditor(EditorPurpose::HostPassword); break;
    case OnlinePage::HostCreate: {
        const auto phase = page.status.phase;
        if (phase == Online::Phase::Joined || phase == Online::Phase::Joining) {
            // In a room: the button closes the hosted one, or leaves the joined one first.
            if (OnlineHosting(page.status)) {
                Online::Leave();
                break;
            }
        }
        HostOnline(now);
        break;
    }
    default: break;
    }
}

void LibrarySession::HostOnline(TimePoint now) {
    if (RoomsRefused(now)) return;
    const auto& user = system_page.settings.user_name;
    const std::wstring_view name = user.empty() ? std::wstring_view{DefaultUserName} : std::wstring_view{user};
    const auto& page = online_page;
    Online::Host(OnlineNickname(page, name), {.name = OnlineRoomName(page, name),
                                             .slots = page.fields.slots,
                                             .port = page.fields.host_port,
                                             .password = OnlineNarrow(page.host_password)});
    system_page.notice.clear();
    system_notice_until = now;
}

void LibrarySession::OpenOnlineEditor(EditorPurpose purpose) {
    const auto& fields = online_page.fields;
    std::wstring initial;
    std::size_t max_length = Online::MaxPasswordLength;
    switch (purpose) {
    case EditorPurpose::OnlineNickname:
        initial = fields.nickname;
        max_length = Online::MaxNicknameLength;
        break;
    case EditorPurpose::OnlineAddress:
        initial = fields.address;
        max_length = Online::MaxAddressLength;
        break;
    case EditorPurpose::OnlinePort:
        initial = std::to_wstring(fields.port);
        max_length = 5;
        break;
    case EditorPurpose::OnlinePassword: initial = online_page.password; break;
    case EditorPurpose::HostName:
        initial = fields.room_name;
        max_length = Online::MaxNicknameLength;
        break;
    case EditorPurpose::HostSlots:
        initial = std::to_wstring(fields.slots);
        max_length = 2;
        break;
    case EditorPurpose::HostPort:
        initial = std::to_wstring(fields.host_port);
        max_length = 5;
        break;
    case EditorPurpose::HostPassword: initial = online_page.host_password; break;
    case EditorPurpose::ChatMessage: max_length = ChatMaxChars; break;
    case EditorPurpose::DsuAddress:
        // "a.b.c.d:port" at most.
        max_length = 21;
        break;
    case EditorPurpose::NextendoServer:
    case EditorPurpose::NextendoNat: {
        const auto& prefs = online_page.nx.preferences;
        initial = OnlineWiden(purpose == EditorPurpose::NextendoServer ? prefs.server_ip : prefs.nat_ip);
        max_length = 15; // "255.255.255.255"
        break;
    }
    default: break;
    }
    system_page.name_editor.emplace(std::move(initial), max_length, NumericEditor(purpose));
    system_page.editor_purpose = purpose;
    system_page.editor_notice.clear();
}

void LibrarySession::FinishOnlineEditor(bool accept, TimePoint now) {
    if (!system_page.name_editor) return;
    auto& page = online_page;
    if (!accept) {
        system_page.name_editor.reset();
        page.pending_room.reset();
        return;
    }
    std::wstring value = system_page.name_editor->Value();
    bool save = true;
    switch (system_page.editor_purpose) {
    case EditorPurpose::OnlineNickname: {
        while (!value.empty() && value.back() == L' ') value.pop_back();
        // Empty follows the user name; anything else is made acceptable to the rooms.
        page.fields.nickname = value.empty() ? std::wstring{} : OnlineWiden(Online::MakeNickname(value));
        break;
    }
    case EditorPurpose::OnlineAddress:
        SplitAddress(value, page.fields.port);
        page.fields.address = std::move(value);
        break;
    case EditorPurpose::OnlinePort: {
        const auto port = Online::ParsePort(value);
        if (!port) {
            system_page.editor_notice = Tr(T::OnlineBadPort);
            return;
        }
        page.fields.port = *port;
        break;
    }
    case EditorPurpose::OnlinePassword:
        page.password = std::move(value);
        save = false;
        break;
    case EditorPurpose::RoomPassword:
        save = false;
        if (const auto room = std::exchange(page.pending_room, std::nullopt))
            JoinOnline(room->address, room->port, OnlineNarrow(value), now);
        break;
    case EditorPurpose::HostName: {
        while (!value.empty() && value.back() == L' ') value.pop_back();
        // Empty follows the nickname; anything else is made acceptable to the room.
        page.fields.room_name = value.empty() ? std::wstring{} : OnlineWiden(Online::MakeRoomName(value));
        break;
    }
    case EditorPurpose::HostSlots: {
        const auto slots = Online::ParseSlots(value);
        if (!slots) {
            system_page.editor_notice = Tr(T::OnlineBadSlots);
            return;
        }
        page.fields.slots = *slots;
        break;
    }
    case EditorPurpose::HostPort: {
        const auto port = Online::ParsePort(value);
        if (!port) {
            system_page.editor_notice = Tr(T::OnlineBadPort);
            return;
        }
        page.fields.host_port = *port;
        break;
    }
    case EditorPurpose::HostPassword:
        page.host_password = std::move(value);
        save = false;
        break;
    case EditorPurpose::ChatMessage:
        save = false;
        SendOnlineChat(value);
        break;
    case EditorPurpose::DsuAddress:
        save = false;
        // Empty adds nothing; anything else must be an address.
        if (value.find_first_not_of(L' ') != std::wstring::npos && !DsuClient::Add(value)) {
            system_page.editor_notice = Tr(T::DsuAddInvalid);
            return;
        }
        dsu_status = DsuClient::Status();
        break;
    case EditorPurpose::NextendoServer:
    case EditorPurpose::NextendoNat: {
        // Saved by the service with the rest of its choices, not with the rooms' fields.
        save = false;
        const auto address = Network::Nextendo::CleanAddress(OnlineNarrow(value));
        if (!address) {
            system_page.editor_notice = Tr(T::NxBadAddress);
            return;
        }
        auto prefs = Network::Nextendo::ServicePreferences();
        (system_page.editor_purpose == EditorPurpose::NextendoServer ? prefs.server_ip : prefs.nat_ip) = *address;
        Network::Nextendo::SetEndpoints(prefs.server_ip, prefs.nat_ip, prefs.tls_mode);
        system_page.notice = Tr(T::SettingsSaved);
        system_notice_until = now + std::chrono::seconds{3};
        PollNextendo();
        break;
    }
    case EditorPurpose::UserName: break;
    }
    system_page.name_editor.reset();
    if (save) {
        const bool saved = Online::SaveFields(page.fields);
        system_page.notice = Tr(saved ? T::SettingsSaved : T::SystemSaveFailed);
        system_notice_until = now + std::chrono::seconds{3};
    }
}

// The room's chat: copied when it changes. Read while it shows; on the other tabs new messages
// from others show a few seconds in a card.
void LibrarySession::PollOnlineChat() {
    auto& page = online_page;
    const auto now = std::chrono::steady_clock::now();
    if (!page.toast.empty() && (now >= chat_toast_until || tab == LibraryTab::Online)) {
        page.toast.clear();
        dirty = true;
    }
    const std::uint64_t version = Online::ChatVersion();
    if (version == page.chat_version) return;
    page.chat_version = version;
    auto chat = Online::Chat();
    const std::uint64_t last_id = std::exchange(page.chat_last_id, chat.lines.empty() ? 0 : chat.lines.back().id);
    std::size_t added = 0;
    bool incoming = false;
    for (const auto& line : chat.lines) {
        if (line.id <= last_id) continue;
        ++added;
        incoming |= line.kind == Online::ChatKind::Message && !line.own;
    }
    page.chat_lines.clear();
    page.chat_lines.reserve(chat.lines.size());
    for (const auto& line : chat.lines) page.chat_lines.push_back(ChatLineInWords(line));
    if (tab == LibraryTab::Online && OnlineChatShows(page)) {
        page.chat.Grew(added, page.chat_lines.size());
        Online::MarkChatRead();
        page.chat_version = Online::ChatVersion();
    } else if (incoming && tab != LibraryTab::Online && Online::LoadChatNotices() != Online::ChatNotices::Off) {
        constexpr std::size_t Shown = 2;
        page.toast.clear();
        for (auto it = chat.lines.rbegin(); it != chat.lines.rend() && page.toast.size() < Shown; ++it)
            if (it->kind == Online::ChatKind::Message && !it->own) page.toast.push_back(ChatLineInWords(*it));
        std::reverse(page.toast.begin(), page.toast.end());
        page.toast_more = chat.unread > page.toast.size() ? chat.unread - page.toast.size() : 0;
        chat_toast_until = now + std::chrono::seconds{5};
    }
    dirty = true;
}

void LibrarySession::HandleOnlineChat() {
    auto& page = online_page;
    if (actions & Left && !page.chat.picking) {
        page.in_chat = false;
        return;
    }
    // The library's buttons, as the menu's: A writes, X picks a quick phrase, B goes back.
    std::optional<MenuAction> action;
    if (actions & Up) action = MenuAction::Up;
    else if (actions & Down) action = MenuAction::Down;
    else if (actions & Play) action = MenuAction::Confirm;
    else if (actions & Refresh) action = MenuAction::Refresh;
    else if (actions & Quit) action = MenuAction::Back;
    if (!action) return;
    using Effect = Online::ChatPage::Effect;
    switch (page.chat.Handle(*action, page.chat_lines.size())) {
    case Effect::None:
    case Effect::Redraw: break;
    case Effect::Compose:
        page.chat_notice.clear();
        OpenOnlineEditor(EditorPurpose::ChatMessage);
        break;
    case Effect::Send:
        SendOnlineChat(Tr(QuickPhraseText(Online::PhraseAt(page.chat.phrase))));
        break;
    case Effect::Back: page.in_chat = false; break;
    }
}

void LibrarySession::SendOnlineChat(std::wstring_view text) {
    auto& page = online_page;
    page.chat.scroll = 0;
    if (Online::SendChat(OnlineNarrow(text))) {
        page.chat_notice.clear();
        return;
    }
    if (!Online::InRoom()) page.chat_notice = Tr(T::ChatNotSent);
}

void LibrarySession::JoinOnline(const std::string& address, std::uint16_t port, const std::string& password,
                                TimePoint now) {
    if (RoomsRefused(now)) return;
    const auto& user = system_page.settings.user_name;
    Online::Join(OnlineNickname(online_page, user.empty() ? std::wstring_view{DefaultUserName} : std::wstring_view{user}),
                 address, port, password);
    system_page.notice.clear();
    system_notice_until = now;
}

bool LibrarySession::RoomsRefused(TimePoint now) {
    if (Network::Nextendo::RoomsAllowed()) return false;
    system_page.notice = Tr(T::NxRoomsPaused);
    system_notice_until = now + std::chrono::seconds{4};
    return true;
}

// The Nextendo view. The service runs on its own thread; the tab copies its state when it
// changes, and the games list is made again when the catalog or the library's games change.
void LibrarySession::PollNextendo() {
    namespace Nx = Network::Nextendo;
    const bool shown = tab == LibraryTab::Online && online_page.nextendo;
    if (shown != nextendo_shown) {
        nextendo_shown = shown;
        Nx::ShowingNextendo(shown);
    }
    // The portal a sign-in started closes once it is over, a few seconds later so the browser
    // still reads that it worked; never under an upload.
    // While a sign-in waits, the QR code always holds a code that pairs: a new one when the last
    // expired or paired a browser, unless that browser is in the middle of an upload.
    if (shown && !Nx::SignInUrl().empty()) {
        const auto status = network_portal.Status();
        if (status.running && status.code_expired && status.transfer.id.empty() && status.content.id.empty() &&
            network_portal.RenewCode()) {
            configuration.network = network_portal.Status();
            dirty = true;
        }
    }
    if (nextendo_portal) {
        const auto now = std::chrono::steady_clock::now();
        if (!Nx::SignInUrl().empty()) {
            nextendo_portal_close = {};
        } else if (nextendo_portal_close == std::chrono::steady_clock::time_point{}) {
            nextendo_portal_close = now + std::chrono::seconds{5};
        } else if (now >= nextendo_portal_close) {
            nextendo_portal = false;
            nextendo_portal_close = {};
            const auto status = network_portal.Status();
            if (status.running && status.transfer.id.empty() && status.content.id.empty() && !configuration.busy) {
                network_portal.Stop();
                configuration.network = network_portal.Status();
                dirty = true;
            }
        }
    }
    // The account on every tab: the header shows its name and picture and marks the Online tab.
    auto& page = online_page.nx;
    const std::uint64_t version = Nx::ServiceVersion();
    if (version != page.version) {
        page.version = version;
        page.preferences = Nx::ServicePreferences();
        page.view = Nx::ServiceView();
        page.avatar = Nx::ServiceAvatar();
        dirty = true;
    }
    if (tab != LibraryTab::Online) return;
    std::size_t titles = 0;
    for (const auto& entry : scan.entries) titles += entry.program_id != 0;
    // A rescan that moved the games under the rows makes them again too.
    bool moved = false;
    for (std::size_t row = 0; row < page.entries.size() && !moved; ++row)
        moved = page.entries[row] != NextendoPage::NoEntry &&
                NextendoRowEntry(page, row, scan.entries) == NextendoPage::NoEntry;
    if (version == page.games_version && titles == page.library_titles && !moved) return;
    page.games_version = version;
    page.library_titles = titles;
    // A few hundred rows at most, and only when something changed: made again whole.
    std::vector<Nx::InstalledTitle> installed;
    std::vector<std::size_t> installed_entry;
    installed.reserve(titles);
    installed_entry.reserve(titles);
    for (std::size_t i = 0; i < scan.entries.size(); ++i) {
        const auto& entry = scan.entries[i];
        if (entry.program_id == 0) continue;
        installed.push_back({FileSys::GetBaseTitleID(entry.program_id), OnlineNarrow(entry.name),
                             OnlineNarrow(entry.display_version)});
        installed_entry.push_back(i);
    }
    page.games = Nx::BuildCatalogRows(Nx::ServiceCatalog(), installed);
    // Each installed row's game in the scan: the first entry of its title, the base game first.
    page.entries.assign(page.games.size(), NextendoPage::NoEntry);
    for (std::size_t row = 0; row < page.games.size(); ++row) {
        if (!page.games[row].installed) continue;
        for (std::size_t i = 0; i < installed.size(); ++i) {
            if (installed[i].title_id != page.games[row].title_id) continue;
            const auto entry = installed_entry[i];
            auto& chosen = page.entries[row];
            if (chosen == NextendoPage::NoEntry ||
                (scan.entries[entry].program_id == page.games[row].title_id &&
                 scan.entries[chosen].program_id != page.games[row].title_id))
                chosen = entry;
        }
    }
    page.game = page.games.empty() ? 0 : std::min(page.game, page.games.size() - 1);
    if (page.games.empty()) page.in_games = false;
    dirty = true;
}

void LibrarySession::ShowOnlineNetwork(bool nextendo) {
    auto& page = online_page;
    page.nextendo = nextendo;
    page.in_chat = false;
    page.nx.in_games = false;
    PollNextendo();
}

void LibrarySession::HandleNextendoPage(TimePoint now) {
    namespace Nx = Network::Nextendo;
    auto& page = online_page.nx;
    const auto say = [&](T text) {
        system_page.notice = Tr(text);
        system_notice_until = now + std::chrono::seconds{3};
    };
    if (actions & Quit) {
        // While the console waits for the phone, B stops waiting; otherwise it leaves the tab.
        if (page.view.phase == Nx::Phase::SigningIn) {
            Nx::CancelSignIn();
            PollNextendo();
            return;
        }
        SwitchTab(LibraryTab::Home);
        return;
    }
    if (actions & Refresh) Nx::RefreshCatalog();
    if (page.in_games) {
        if (actions & Up && page.game > 0) page.game--;
        if (actions & Down && page.game + 1 < page.games.size()) page.game++;
        if (actions & Left) page.in_games = false;
        // A on an installed game starts it, as from Home: the library's check runs first.
        if (actions & Play) {
            const auto entry = NextendoRowEntry(page, page.game, scan.entries);
            if (entry != NextendoPage::NoEntry) {
                selected = entry;
                nextendo_launch = true;
            } else {
                say(T::NxGameNotInstalled);
            }
        }
        return;
    }
    if (actions & Up && page.selected > 0) page.selected--;
    if (actions & Down && page.selected + 1 < NextendoPage::Rows) page.selected++;
    if (actions & Right && !page.games.empty()) {
        page.in_games = true;
        return;
    }
    // On an address row: Y shows or hides both addresses, Menu puts the project's one back.
    if (const auto fallback = DefaultAddress(page.selected); !fallback.empty()) {
        if (actions & Pin) {
            page.show_addresses = !page.show_addresses;
            return;
        }
        if (actions & Options) {
            auto prefs = Nx::ServicePreferences();
            (page.selected == NextendoPage::ServerIp ? prefs.server_ip : prefs.nat_ip) = std::string(fallback);
            Nx::SetEndpoints(prefs.server_ip, prefs.nat_ip, prefs.tls_mode);
            say(T::NxAddressRestored);
            PollNextendo();
            return;
        }
    }
    if (!(actions & Play)) return;
    switch (page.selected) {
    case NextendoPage::Account:
        if (NextendoSignedIn(page)) {
            Nx::SignOut();
        } else if (page.view.phase == Nx::Phase::SignedOut) {
            // The console can't show the sign-in page: it is finished in a browser on the
            // network, through the portal, which opens for it.
            if (!network_portal.Status().running) {
                if (StartPortal()) nextendo_portal = true;
            }
            configuration.network = network_portal.Status();
            if (!configuration.network.running) {
                say(T::NxPhoneNoPortal);
                break;
            }
            Nx::SignIn();
        } else {
            WriteDiag("nextendo: sign-in pressed in phase " + std::to_string(static_cast<int>(page.view.phase)));
        }
        break;
    case NextendoPage::UseNextendo: {
        const bool on = !page.preferences.use_nextendo;
        if (!Nx::ChooseNextendo(on)) {
            say(T::NxGameRunning);
            break;
        }
        say(on ? T::NxChosen : T::NxRoomsChosen);
        break;
    }
    case NextendoPage::ServerIp: OpenOnlineEditor(EditorPurpose::NextendoServer); break;
    case NextendoPage::NatIp: OpenOnlineEditor(EditorPurpose::NextendoNat); break;
    case NextendoPage::TlsMode: {
        const auto prefs = Nx::ServicePreferences();
        Nx::SetEndpoints(prefs.server_ip, prefs.nat_ip, NextTlsMode(prefs.tls_mode));
        break;
    }
    default: break;
    }
    PollNextendo();
}

} // namespace Jadyx
