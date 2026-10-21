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

GameFiles FindGameFiles(std::span<const LibraryEntry> entries, std::uint64_t application) {
    GameFiles game{.application = application};
    for (const auto& entry : entries) {
        if (!entry.program_id || FileSys::GetBaseTitleID(entry.program_id) != application) continue;
        game.launch_paths.push_back(entry.launch_path);
        if (IsStoragePath(entry.launch_path)) game.external = true;
        else game.files.push_back(entry.relative_path);
    }
    return game;
}

std::string RemoveGame(const fs::path& root, const GameFiles& game) {
    if (game.external) return "external";
    if (!game.application || game.files.empty() ||
        !std::all_of(game.files.begin(), game.files.end(), Inside)) return "game";
    // The game first: if its file is locked nothing else is lost.
    for (const auto& file : game.files)
        if (!RemoveDump(root, file)) return "game";
    for (const auto& path : game.launch_paths) PulseSessions::Discard(path);
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
