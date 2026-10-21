// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/portal_library.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include "core/file_sys/common_funcs.h"
#include "jadyx/content/content_actions.h"
#include "jadyx/content/content_service.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/utf8.h"

namespace Jadyx {

std::uint64_t ContentTagApplication(const std::string& tag) {
    const auto colon = tag.find(':');
    const auto kind = tag.substr(0, colon);
    std::uint64_t application{};
    if (colon == std::string::npos || (kind != "update" && kind != "dlc") ||
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
    return Content::ImportUploadedContent(package, tag.starts_with("update:") ? Content::Kind::Update : Content::Kind::Dlc,
        application, game->launch_path, stop, [&](std::uint64_t done, std::uint64_t total) {
            Progress(done, total);
            if (progress) progress(done, total);
        });
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
