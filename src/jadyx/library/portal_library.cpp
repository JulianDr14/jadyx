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

PortalLibrarySource::PortalLibrarySource(std::filesystem::path root_, std::filesystem::path cache_directory)
    : root{std::move(root_)}, cache{std::move(cache_directory)} {}

void PortalLibrarySource::Publish(const std::vector<LibraryEntry>& entries, std::uint32_t language_) {
    std::vector<Game> next;
    for (const auto& entry : entries) {
        if (!entry.title_loaded || !entry.program_id || entry.launch_path.empty()) continue;
        const auto application = FileSys::GetBaseTitleID(entry.program_id);
        // One card per game: a second dump of the same title adds nothing to manage.
        if (std::any_of(next.begin(), next.end(), [&](const auto& g) { return g.info.program_id == application; }))
            continue;
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
    const auto colon = tag.find(':');
    const auto kind = tag.substr(0, colon);
    std::uint64_t application{};
    if (colon == std::string::npos || (kind != "update" && kind != "dlc") ||
        std::from_chars(tag.data() + colon + 1, tag.data() + tag.size(), application, 16).ec != std::errc{})
        return "file";
    const auto game = Find(application);
    if (!game) return "game";
    return Content::ImportUploadedContent(package, kind == "update" ? Content::Kind::Update : Content::Kind::Dlc,
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

std::uint64_t PortalLibrarySource::Generation() {
    // The cache stamp folds in the content generation (as the metadata fill does). A page of
    // covers asks for it many times; one catalog read every few seconds is enough.
    std::lock_guard lock{generation_mutex};
    const auto now = std::chrono::steady_clock::now();
    if (now < generation_until) return generation;
    Content::Catalog catalog;
    if (Content::DefaultStore().Load(catalog) == Content::Error::None) {
        generation = catalog.generation;
        generation_until = now + std::chrono::seconds{3};
    }
    return generation;
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
    const auto file = IsStoragePath(game.launch_path) ? 0 : LibraryCache::StampFor(root / game.relative_path);
    const auto stamp = file ^ (Generation() * 0x9e3779b97f4a7c15ULL);
    auto record = cache.Load(game.launch_path, stamp, game_language, true);
    if (!record || record->icon.empty()) return std::nullopt;
    return std::string{record->icon.begin(), record->icon.end()};
}

} // namespace Jadyx
