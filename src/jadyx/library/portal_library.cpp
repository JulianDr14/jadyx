// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/portal_library.h"
#include <algorithm>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <fmt/format.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/file_sys/common_funcs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "jadyx/content/content_actions.h"
#include "jadyx/content/content_service.h"
#include "jadyx/library/save_transfer.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/utf8.h"

namespace Jadyx {
namespace {

/// The current user's save folder for `application`, the one the game opens; empty when there
/// is no user profile.
std::filesystem::path GameSavePath(std::uint64_t application) {
    // Reading the profiles makes the default user when there is none, as a game's boot would:
    // one at a time, so two portal jobs never make two.
    static std::mutex profiles_mutex;
    std::lock_guard lock{profiles_mutex};
    Service::Account::ProfileManager profiles;
    const auto index = static_cast<std::size_t>(std::clamp<int>(
        Settings::values.current_user.GetValue(), 0, static_cast<int>(Service::Account::MAX_USERS) - 1));
    const auto user = profiles.GetUser(index);
    if (!user) return {};
    // SaveDataFactory::GetFullPath: the newer account layout when it exists, else the older one.
    const auto saves = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir) / "user" / "save";
    const auto future = saves / "account" / user->RawString() / fmt::format("{:016X}", application & ~0xFFull) / "0";
    std::error_code ec;
    if (std::filesystem::is_directory(future, ec)) return future;
    const auto id = user->AsU128();
    return saves / "0000000000000000" / fmt::format("{:016X}{:016X}", id[1], id[0]) /
           fmt::format("{:016X}", application);
}

std::string ExportSave(std::uint64_t application, std::string& zip) {
    const auto folder = GameSavePath(application);
    if (folder.empty()) return "profile";
    const auto error = ExportSaveFolder(folder, zip);
    if (!error.empty() && error != "no_save")
        LOG_WARNING(Frontend, "Portal save {:016X}: export failed: {}", application, error);
    return error;
}

std::string ImportSave(const std::filesystem::path& archive, std::uint64_t application, std::stop_token stop,
                       const Mods::ArchiveProgress& progress) {
    const auto target = GameSavePath(application);
    if (target.empty()) return "profile";
    const auto backups = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "mods" / "save-backups";
    std::string log;
    const auto error = ImportSaveArchive(archive, application, target, backups, stop, progress, &log);
    if (error.empty()) LOG_INFO(Frontend, "Portal save {:016X}: loaded, {}", application, log);
    else LOG_WARNING(Frontend, "Portal save {:016X}: not loaded: {} ({})", application, error, log);
    return error;
}

} // namespace

std::uint64_t ContentTagApplication(const std::string& tag) {
    const auto colon = tag.find(':');
    const auto kind = tag.substr(0, colon);
    std::uint64_t application{};
    if (colon == std::string::npos || (kind != "update" && kind != "dlc" && kind != "save") ||
        std::from_chars(tag.data() + colon + 1, tag.data() + tag.size(), application, 16).ec != std::errc{})
        return 0;
    return application;
}

PortalLibrarySource::PortalLibrarySource(std::filesystem::path root_, std::filesystem::path cache_directory)
    : root{std::move(root_)}, cache{std::move(cache_directory)} {}

void PortalLibrarySource::Publish(const std::vector<LibraryEntry>& entries, std::uint32_t language_) {
    std::vector<Game> previous;
    {
        std::lock_guard lock{mutex};
        previous = games;
    }
    std::vector<Game> next;
    const auto listed = [&](std::uint64_t application) {
        return std::any_of(next.begin(), next.end(), [&](const auto& g) { return g.info.program_id == application; });
    };
    for (const auto& entry : entries) {
        if (entry.launch_path.empty()) continue;
        if (!entry.title_loaded) {
            // A rescan reads metadata again; keep showing what was known of a dump still present
            // rather than emptying the page until the fill reaches it.
            const auto known = std::find_if(previous.begin(), previous.end(),
                [&](const auto& g) { return g.launch_path == entry.launch_path; });
            if (known != previous.end() && !listed(known->info.program_id)) next.push_back(*known);
            continue;
        }
        if (!entry.program_id) continue;
        const auto application = FileSys::GetBaseTitleID(entry.program_id);
        // One card per game: a second dump of the same title adds nothing to manage.
        if (listed(application)) continue;
        auto files = FindGameFiles(entries, application);
        const bool removable = !files.external;
        next.push_back({.info = {.program_id = application, .name = Utf16ToUtf8(entry.name),
                                 .developer = Utf16ToUtf8(entry.developer),
                                 .version = Utf16ToUtf8(entry.display_version), .removable = removable},
                        .launch_path = entry.launch_path, .relative_path = entry.relative_path,
                        .files = std::move(files)});
    }
    std::lock_guard lock{mutex};
    if (next == games && language_ == language) return;
    games = std::move(next);
    language = language_;
    ++revision;
}

PortalLibrary PortalLibrarySource::View() {
    return {
        .games = [this] {
            std::lock_guard lock{mutex};
            std::vector<PortalGame> list;
            list.reserve(games.size());
            for (const auto& game : games) list.push_back(game.info);
            return std::pair{revision, std::move(list)};
        },
        .cover = [this](std::uint64_t application) { return Cover(application); },
        .contents = [](std::uint64_t application) {
            Content::Catalog catalog;
            if (Content::DefaultStore().Load(catalog) != Content::Error::None) throw std::runtime_error{"catalog"};
            std::vector<PortalContent> list;
            for (const auto& r : catalog.revisions) {
                if (r.application != application) continue;
                list.push_back({.id = r.id, .kind = r.kind == Content::Kind::Update ? "update" : "dlc",
                                .name = r.name, .version = r.display_version, .title = r.title,
                                .bytes = r.bytes, .enabled = r.enabled, .internal = r.internal});
            }
            return list;
        },
        .progress = [this] { return std::pair{installed.load() >> 20, install_total.load() >> 20}; },
        .save_export = [this](std::uint64_t application, std::string& zip) -> std::string {
            if (!Find(application)) return "game";
            return ExportSave(application, zip);
        },
    };
}

std::optional<PortalLibrarySource::Game> PortalLibrarySource::Find(std::uint64_t application) const {
    std::lock_guard lock{mutex};
    const auto found = std::find_if(games.begin(), games.end(),
        [&](const auto& g) { return g.info.program_id == application; });
    if (found == games.end()) return std::nullopt;
    return *found;
}

std::string PortalLibrarySource::Install(const std::filesystem::path& package, const std::string& tag,
                                         std::stop_token stop, const Content::Progress& progress) {
    // The portal validated the tag when the upload was created; parse it again, never trust it.
    const auto application = ContentTagApplication(tag);
    if (!application) return "file";
    const auto game = Find(application);
    if (!game) return "game";
    const auto report = [&](std::uint64_t done, std::uint64_t total) {
        Progress(done, total);
        if (progress) progress(done, total);
    };
    if (tag.starts_with("save:")) return ImportSave(package, application, stop, report);
    return Content::ImportUploadedContent(package, tag.starts_with("update:") ? Content::Kind::Update : Content::Kind::Dlc,
        application, game->launch_path, stop, report);
}

std::string PortalLibrarySource::Remove(const PortalRemoval& removal) {
    const auto game = Find(removal.application);
    if (!game) return "game";
    return removal.content.empty() ? RemoveGame(root, game->files) :
                                     Content::RemoveContent(removal.application, removal.content);
}

std::optional<std::string> PortalLibrarySource::Cover(std::uint64_t application) {
    const auto found = Find(application);
    if (!found) return std::nullopt;
    const auto& game = *found;
    std::uint32_t game_language{};
    {
        std::lock_guard lock{mutex};
        game_language = language;
    }
    // The cover belongs to the dump: an update/DLC of the game does not invalidate it.
    const auto stamp = IsStoragePath(game.launch_path) ? 0 : LibraryCache::StampFor(root / game.relative_path);
    auto record = cache.Load(game.launch_path, stamp, game_language, true);
    if (!record || record->icon.empty()) return std::nullopt;
    return std::string{record->icon.begin(), record->icon.end()};
}

} // namespace Jadyx
