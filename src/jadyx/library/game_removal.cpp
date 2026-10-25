// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/game_removal.h"
#include <algorithm>
#include <system_error>
#include <fmt/format.h>
#include "common/fs/path_util.h"
#include "core/file_sys/common_funcs.h"
#include "jadyx/content/content_actions.h"
#include "jadyx/pulse/pulse_sessions.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/storage/storage_location.h"

namespace Jadyx {
namespace {
namespace fs = std::filesystem;

std::vector<fs::path> Children(const fs::path& folder) {
    std::vector<fs::path> result;
    std::error_code ec;
    for (fs::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && !it->is_symlink(ec)) result.push_back(it->path());
    return result;
}

/// The title's folders in every layout SaveDataFactory::GetFullPath writes (account, device,
/// cache and temporary saves, old and future layouts), for every user.
std::vector<fs::path> SaveFolders(std::uint64_t application) {
    const auto saves = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    const auto title = fmt::format("{:016X}", application);
    std::vector<fs::path> result{saves / "user" / "save" / "device" / title, saves / "user" / "save" / "cache" / title};
    for (const auto& per_user : {saves / "user" / "save" / "0000000000000000", saves / "user" / "save" / "account",
                                 saves / "user" / "0000000000000000", saves / "temp" / "0000000000000000"})
        for (const auto& user : Children(per_user)) result.push_back(user / title);
    return result;
}

bool Inside(const fs::path& relative) {
    const auto normal = relative.lexically_normal();
    return !normal.empty() && normal.is_relative() && *normal.begin() != "..";
}

/// Deletes one dump, then the folders it leaves empty up to (not including) the root.
bool RemoveDump(const fs::path& root, const fs::path& relative) {
    std::error_code ec;
    const auto file = root / relative;
    if (!fs::remove(file, ec) && ec) return false;
    for (auto folder = file.parent_path(); folder != root && Inside(folder.lexically_relative(root));
         folder = folder.parent_path())
        if (!fs::remove(folder, ec)) break; // Not empty (or gone): leave the rest alone.
    return true;
}
} // namespace

namespace {
/// The dumps of `application` that `chosen` picks; the rest are kept.
template <typename Chosen>
GameFiles Collect(std::span<const LibraryEntry> entries, std::uint64_t application, Chosen chosen) {
    GameFiles game{.application = application};
    for (const auto& entry : entries) {
        if (!entry.program_id || FileSys::GetBaseTitleID(entry.program_id) != application) continue;
        if (!chosen(entry) || IsStoragePath(entry.launch_path)) {
            game.kept = true;
            continue;
        }
        game.launch_paths.push_back(entry.launch_path);
        game.files.push_back(entry.relative_path);
    }
    game.external = game.files.empty() && game.kept;
    return game;
}
} // namespace

GameFiles FindGameFiles(std::span<const LibraryEntry> entries, std::uint64_t application) {
    return Collect(entries, application, [](const LibraryEntry&) { return true; });
}

GameFiles FindDumpFiles(std::span<const LibraryEntry> entries, const std::string& launch_path) {
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const auto& e) { return e.launch_path == launch_path; });
    if (found == entries.end() || !found->program_id) return {};
    const auto application = FileSys::GetBaseTitleID(found->program_id);
    auto game = Collect(entries, application, [&](const LibraryEntry& e) { return e.launch_path == launch_path; });
    // The chosen copy itself is external: never fall back to deleting another one.
    game.external = IsStoragePath(launch_path);
    return game;
}

std::string RemoveGame(const fs::path& root, const GameFiles& game) {
    if (game.external) return "external";
    // A dump Jadyx Portal sent to Dev Mode's storage is named by its absolute path.
    const auto developer = [](const fs::path& file) { return file.is_absolute() && InDeveloperGames(file); };
    if (!game.application || game.files.empty() ||
        !std::all_of(game.files.begin(), game.files.end(), [&](const auto& f) { return Inside(f) || developer(f); }))
        return "game";
    // The game first: if its file is locked nothing else is lost.
    for (const auto& file : game.files)
        if (!(developer(file) ? RemoveDump(DeveloperGames(), file.lexically_normal().lexically_relative(DeveloperGames()))
                              : RemoveDump(root, file))) return "game";
    for (const auto& path : game.launch_paths) PulseSessions::Discard(path);
    // Another copy still plays: its saves and updates/DLC are its own too.
    if (game.kept) return {};
    if (!Content::RemoveApplicationContent(game.application).empty()) return "content";
    bool saves = true;
    for (const auto& folder : SaveFolders(game.application)) {
        std::error_code ec;
        fs::remove_all(folder, ec);
        saves &= !ec;
    }
    return saves ? std::string{} : "saves";
}

} // namespace Jadyx
