// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"

#include "core/core.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace Jadyx {
using I18n::Tr;
namespace {
/// Background batches stay short so a page change never waits long behind them: games opened
/// through the loaders per fill batch, cache records read per names batch.
constexpr size_t FillBatch = 6;
constexpr size_t NamesBatch = 64;
} // namespace

void LibrarySession::RefreshSetup() {
    if (setup_future.valid()) return;
    std::packaged_task<FileSetupStatus()> task{[] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
        return ReadFileSetupStatus();
    }};
    setup_future = task.get_future();
    setup_worker = std::jthread(std::move(task));
}

// Forget what the entries know and read it again (cache first): after the game language
// changed or the cache was cleared. Results in flight belong to the old generation.
void LibrarySession::ReloadMetadata() {
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
}

// A game's updates/DLC changed (0: any game's): only its entries read their metadata again.
// Its cache record no longer matches the catalog, so the fill rereads it from the files.
void LibrarySession::ForgetContent(std::uint64_t application) {
    metadata = {}; ++generation;
    for (auto& entry : scan.entries) {
        if (application && FileSys::GetBaseTitleID(entry.program_id) != application) continue;
        entry.title_loaded = entry.metadata_loaded = false;
        entry.build_id = {}; entry.display_version.clear();
    }
    metadata_dirty = true; names_cursor = fill_cursor = 0;
}

// Each game's play time and last play, and the library's order (play_history.h). `keep` stays
// selected where it lands; otherwise the first game (the last one played) is.
void LibrarySession::RefreshPlay(std::string_view keep) {
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
}

// The Pulse session each game keeps, as the tiles and the options show it.
void LibrarySession::RefreshPulse() {
    PulseSessions::Prune();
    for (auto& entry : scan.entries) {
        const auto kept = PulseSessions::Find(entry.launch_path);
        entry.pulse_saved = kept ? PulseSessions::FormatTime(kept->saved) : std::wstring{};
        entry.pulse_current = kept && kept->current;
    }
    dirty = true;
}

LibrarySession::Covers LibrarySession::CoversForTab() {
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
        const auto around = LibraryLayout::CoversAround({selected, rail}, scan.entries.size(),
                                                        LibraryLayout::RailCount(scan.entries));
        const size_t row = rail ? 0 : selected / LibraryLayout::Columns * LibraryLayout::Columns;
        for (size_t i = row; i < std::min(row + LibraryLayout::Columns, around.end); ++i) add(i);
        for (size_t i = row + LibraryLayout::Columns; i < around.end; ++i) add(i);
        for (size_t i = 0; i < around.rail; ++i) add(i);
        for (size_t i = around.first; i < row; ++i) add(i);
        // Rows 0 and 1 share a first entry: the key is both ends of the window.
        result.key = (around.first + around.end * 65536) * 16;
    }
    return result;
}

void LibrarySession::StartScan(StorageFolder added) {
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
        RefreshSystemVersions(stop);
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        auto result = ScanGameLibrary(root, stop);
        // The filesystem's own text is English whatever the interface language: log it only.
        if (!result.error.empty()) {
            LOG_WARNING(Frontend, "Library scan: internal folder: {}", result.error);
            result.error = I18n::ToUtf8(Tr(T::LibraryScanFailed));
        }
        const auto adopt = [&](std::span<LibraryEntry> entries, const std::wstring& source) {
            for (auto& entry : entries) {
                const auto path = (root / entry.relative_path).u8string();
                entry.launch_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
                entry.source_name = source;
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
        };
        adopt(result.entries, std::wstring{Tr(T::SourceInternal)});
        // Games Jadyx Portal sent to Dev Mode's storage. Their relative path is absolute, so
        // `root / relative_path` names them wherever the library resolves a dump.
        if (DeveloperStorageAvailable()) {
            auto developer = ScanGameLibrary(DeveloperGames(), stop);
            if (!developer.error.empty() && !developer.missing_root)
                LOG_WARNING(Frontend, "Library scan: developer folder: {}", developer.error);
            for (auto& entry : developer.entries) entry.relative_path = DeveloperGames() / entry.relative_path;
            adopt(developer.entries, std::wstring{Tr(T::SourceDeveloper)});
            result.limited |= developer.limited;
            result.entries.insert(result.entries.end(), std::make_move_iterator(developer.entries.begin()),
                                  std::make_move_iterator(developer.entries.end()));
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
}

void LibrarySession::PollSetup() {
    if (setup_future.valid() && setup_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try { configuration.status = setup_future.get(); }
        catch (...) { configuration.notice = Tr(T::SetupCheckFailed); }
        configuration.selected = std::min(configuration.selected, ConfigurationRowCount(configuration) - 1);
        dirty = true;
    }
}

void LibrarySession::FinishScan() {
    if (loading && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        // A rescan keeps the game the player was on; the first scan starts at the last one played.
        const auto kept = selected < scan.entries.size() ? scan.entries[selected].launch_path : std::string{};
        try {
            auto rescanned = future.get();
            if (keep_titles) KeepKnownTitles(scan.entries, rescanned.entries);
            scan = std::move(rescanned);
            if (keep_titles && *keep_titles) ForgetContent(*keep_titles);
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
        RefreshPlay(kept);
        RefreshPulse();
        if (!configuration.busy) RefreshSetup();
        notice = Wide(scan.error);
        if (notice.empty() && !initial_notice.empty()) notice = Wide(initial_notice);
        initial_notice = {};
        if (notice.empty()) notice = std::move(after_scan);
        after_scan.clear();
        if (scan.limited) notice = Tr(T::ScanLimit);
        dirty = true;
    }
}

void LibrarySession::AutoPick() {
    if (!app_mode && !auto_pick.empty() && !configuration.open && !loading && !picker && !validation.valid()) {
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
}

void LibrarySession::StartValidation(bool on_game) {
    if (actions & Play && on_game && !configuration.open && !settings && !panel.keyboard.open && !loading &&
        !scan.entries.empty() && !network_portal.BlockForLocalOperation()) {
        notice=Tr(T::NetworkBusy); actions &= ~Play; dirty=true;
    }
    if (actions & Play && on_game && !configuration.open && !settings && !panel.keyboard.open && !loading &&
        !scan.entries.empty()) {
        network_portal.Stop();
        selected_path = scan.entries[selected].launch_path;
        if (selected_program_id) *selected_program_id = scan.entries[selected].program_id;
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
            const auto resolved = Content::ResolveLaunchContent(*system.GetFilesystem(), path, provider);
            if (resolved.error == Content::Error::Unavailable) return std::string{"content-missing"};
            return resolved.error == Content::Error::None ? std::string{} : I18n::ToUtf8(Content::LaunchErrorText(resolved));
        }};
        validation = task.get_future();
        validation_worker = std::jthread(std::move(task));
        actions = 0;
    }
}

bool LibrarySession::HandleMissingContent() {
    if (missing_content) {
        if (actions & Quit) { missing_content = false; actions = 0; }
        else if (actions & Swap) { Content::AllowMissingForNextLaunch(selected_path); launch = selected_path; return true; }
        else if (actions & Play) missing_content = false;
        else actions = 0;
        dirty = true;
    }
    return false;
}

bool LibrarySession::PollValidation() {
    if (validation.valid()) {
        actions = 0;
        if (validation.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try {
                const auto error = validation.get();
                if (error.empty()) { launch = selected_path; return true; }
                if (error == "content-missing") missing_content = true;
                else notice = Wide(error);
            } catch (const std::exception& e) {
                LOG_WARNING(Frontend, "Game check failed: {}", e.what());
                notice = Tr(T::GameLoadFailed);
            }
            dirty = true;
        }
    }
    return false;
}

void LibrarySession::MergeMetadata() {
    if (!configuration.busy && metadata.valid() && metadata.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try {
            auto batch = metadata.get();
            if (batch.generation == generation) {
                // Merge field by field: a names-only batch must not drop icons already shown.
                const auto covers = CoversForTab();
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
}

void LibrarySession::ScheduleMetadata() {
    // Metadata, one worker at a time and in priority order: the covers around the cursor, then
    // every title the cache already has, then the rest of the library into the cache.
    const auto covers = CoversForTab();
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
                [library_root = root, &reader_cache = cache, requested = std::move(requested), batch_generation = generation,
                     request](std::stop_token stop) mutable {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
                    ReadLibraryMetadata(library_root, requested, reader_cache, request, stop);
                    return MetadataBatch{std::move(requested), batch_generation, request.icons,
                                         stop.stop_requested()};
                }};
            metadata_background = !request.icons;
            metadata = task.get_future();
            metadata_worker = std::jthread(std::move(task));
        }
    }
}

} // namespace Jadyx
