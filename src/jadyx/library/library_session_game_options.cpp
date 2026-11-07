// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"

namespace Jadyx {
using I18n::Tr;

void LibrarySession::Notify(TimePoint now, T text) {
    options_notice = Tr(text);
    options_notice_until = now + std::chrono::seconds{4};
}

void LibrarySession::OpenGameOptions(bool on_game) {
    if (actions & Options && on_game && !game_options.open && !settings && !configuration.open &&
        !panel.BindingEditorOpen() && !loading && !picker && !validation.valid() && !scan.entries.empty()) {
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
}

void LibrarySession::HandleGameOptions(TimePoint now) {
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
            ForgetContent(*changed);
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
    if (game_options.activate && !configuration.open && scan.entries[game_options.game].title_loaded)
        ActivateGameOption(now);
    if (content_session) HandleContentPane();
    else if (game_options.page == GameOption::Mods && mod_session) HandleModsPane(now);
    else if (game_options.page == GameOption::Cheats && cheat_session && (actions || requested_manager_tab))
        HandleCheatsPane(now);
    else if (actions && GameOptions()[game_options.selected] == GameOption::Graphics) HandleGameGraphicsPane(now);
    else if (actions) HandlePlainOption(now);
    actions = 0;
}

void LibrarySession::ActivateGameOption(TimePoint now) {
    game_options.activate = false;
    dirty = true; // The title may arrive with no input: nothing else redraws the pane.
    game_options.page = GameOptionHasManager(GameOptions()[game_options.selected]) ?
        std::optional{GameOptions()[game_options.selected]} : std::nullopt;
    if (GameOptions()[game_options.selected] == GameOption::Graphics) {
        const auto& game = scan.entries[game_options.game];
        game_graphics_id = game.program_id;
        game_graphics = game.program_id ? LoadGameGraphics(game.program_id) : GameGraphics{};
        game_graphics_row = 0;
        if (!game.program_id) Notify(now, T::CheatsUnavailable);
    }
    if (game_options.page == GameOption::Updates || game_options.page == GameOption::Dlc) {
        const auto& game = scan.entries[game_options.game];
        if (!game.title_loaded || !game.program_id) {
            Notify(now, T::CheatsUnavailable); game_options.page.reset();
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
            Notify(now, T::ModsHomebrew);
            game_options.page.reset();
        } else if (!game.title_loaded) {
            Notify(now, T::CheatsUnavailable);
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
            Notify(now, T::CheatsHomebrew);
            game_options.page.reset();
        } else if (!game.title_loaded || !known) {
            Notify(now, T::CheatsUnavailable);
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

void LibrarySession::HandleContentPane() {
    dirty |= content_session->Poll();
    if (content_session->Busy()) dirty = true;
    const auto send = [&](unsigned bits, Content::Input input) {
        if ((actions & bits) && content_session && content_session->Handle(input)) {
            const bool changed = content_session->Changed();
            const auto application = content_session->Application();
            content_session.reset(); game_options = {};
            if (changed) ForgetContent(application);
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
}

void LibrarySession::HandleModsPane(TimePoint now) {
    using Kind = Mods::ModsCommand::Kind;
    using Mods::ModJob;
    const ModJob finished = mod_session->Poll();
    if (finished != ModJob::None) {
        const auto& result = mod_session->View();
        const bool online = result.origin.kind == Mods::SourceKind::GameBanana ||
                            result.origin.kind == Mods::SourceKind::Cnx;
        if (result.error != Mods::ModError::None) Notify(now, Mods::ErrorText(result.error));
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
                options_notice = result.save      ? std::wstring{Tr(T::ModsSaveInstalled)}
                                 : result.updated ? std::wstring{Tr(T::ModsUpdated)}
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
    if (failed) Notify(now, Mods::ErrorText(*failed));
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
            if (!mod_session->Toggle(command.index)) Notify(now, T::ModsSaveFailed);
            break;
        case Kind::Move:
            if (!mod_session->Move(command.index, command.to)) Notify(now, T::ModsSaveFailed);
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
                Notify(now, T::ModsDownloadRefused);
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
}

void LibrarySession::HandleCheatsPane(TimePoint now) {
    using Cheats::CheatsEffect;
    const auto send = [&](unsigned bits, MenuAction action) {
        if (bits && !(actions & bits)) return;
        const auto input = Cheats::CheatsInputForAction(action);
        if (!input) return;
        switch (Cheats::HandleCheatsInput(cheats_page, *cheat_session, *input,
                                          Cheats::CheatsLayout::VisibleRows)) {
        case CheatsEffect::Changed:
            if (!cheat_session->Saved()) Notify(now, T::CheatsSaveFailed);
            else options_notice.clear();
            break;
        case CheatsEffect::OverBudget: Notify(now, T::CheatsOverBudget); break;
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
}

void LibrarySession::HandleGameGraphicsPane(TimePoint now) {
    // The game's rows: Up on the first goes back to the sections, B closes the options.
    const auto& game = scan.entries[game_options.game];
    const bool loaded = game.program_id != 0 && game_graphics_id == game.program_id;
    if (actions & Up && game_graphics_row == 0) {
        game_options.focus = GameOptionsFocus::Sections;
        actions &= ~Up;
    }
    const auto send = [&](unsigned bit, GraphicsInput input) {
        if (!loaded || !(actions & bit) || !HandleGameGraphicsInput(game_graphics, game_graphics_row, input))
            return;
        Notify(now, SaveGameGraphics(game.program_id, game_graphics) ? T::SettingsSaved : T::SystemSaveFailed);
    };
    send(Up, GraphicsInput::Up);
    send(Down, GraphicsInput::Down);
    send(Left, GraphicsInput::Left);
    send(Right, GraphicsInput::Right);
    send(Play, GraphicsInput::Confirm);
    if (actions & Quit) HandleGameOptionsInput(game_options, GameOptionsInput::Back);
    dirty = true;
}

void LibrarySession::HandlePlainOption(TimePoint now) {
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
            Notify(now, files.external ? T::GameRemoveExternal : T::GameRemoveFailed);
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
            Notify(now, T::PulseNone);
        } else if (std::exchange(game_options.discard, false)) {
            Notify(now, PulseSessions::Discard(game.launch_path) ? T::PulseDiscarded : T::PulseDiscardFailed);
            RefreshPulse();
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

} // namespace Jadyx
