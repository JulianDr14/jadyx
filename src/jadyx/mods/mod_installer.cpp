// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/mods/mod_installer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "jadyx/storage/atomic_file.h"
#include "jadyx/mods/mod_manifest.h"

namespace Jadyx::Mods {
namespace {
namespace fs = std::filesystem;

/// Left free after an install, so a mod never takes the space saves and caches need.
constexpr std::uint64_t SpaceMargin = 256ull << 20;

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path Utf8Path(const std::string& text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

std::vector<std::string> FolderNames(const fs::path& root) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it{root, ec}, end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) names.push_back(Utf8(it->path().filename()));
    return names;
}

} // namespace

ModError FromArchiveError(ArchiveError error) {
    switch (error) {
    case ArchiveError::None: return ModError::None;
    case ArchiveError::Encrypted: return ModError::Encrypted;
    case ArchiveError::TooLarge: return ModError::TooLarge;
    case ArchiveError::Write: return ModError::Write;
    case ArchiveError::Cancelled: return ModError::Cancelled;
    case ArchiveError::Unreadable: break;
    }
    return ModError::Unreadable;
}

std::string TitleFolder(std::uint64_t title_id) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(title_id));
    return text;
}

ModLibrary::ModLibrary(ModPaths paths_, std::uint64_t title_id_, std::vector<std::string> reserved_)
    : paths{std::move(paths_)}, title_id{title_id_}, reserved{std::move(reserved_)} {}

std::vector<std::string> ModLibrary::ModFolders() const {
    auto folders = FolderNames(TitleRoot());
    std::erase_if(folders, [&](const std::string& folder) {
        return std::find(reserved.begin(), reserved.end(), folder) != reserved.end();
    });
    return folders;
}

fs::path ModLibrary::TitleRoot() const {
    return paths.load / TitleFolder(title_id);
}

fs::path ModLibrary::ManifestPath() const {
    return paths.data / (TitleFolder(title_id) + ".json");
}

fs::path ModLibrary::FolderPath(const std::string& folder) const {
    return TitleRoot() / Utf8Path(folder);
}

fs::path ModLibrary::NewScratch(const char* kind) const {
    static std::atomic<std::uint64_t> counter{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return paths.data / kind / TitleFolder(title_id) /
           (std::to_string(stamp) + '-' + std::to_string(counter++));
}

void ModLibrary::ClearScratch() const {
    std::error_code ec;
    for (const char* kind : {"staging", "trash"}) fs::remove_all(paths.data / kind / TitleFolder(title_id), ec);
}

std::uint64_t ModLibrary::FreeSpace() const {
    std::error_code ec;
    fs::create_directories(paths.data, ec);
    const auto space = fs::space(paths.data, ec);
    return ec ? 0 : space.available;
}

std::vector<ModRecord> ModLibrary::Load(std::int64_t now) {
    ClearScratch();
    std::vector<ModRecord> mods;
    if (const auto text = ReadWholeFile(ManifestPath())) {
        if (auto parsed = ParseManifest(*text)) mods = std::move(*parsed);
    }
    if (Reconcile(mods, ModFolders(), now)) Save(mods);
    return mods;
}

bool ModLibrary::Save(const std::vector<ModRecord>& mods) const {
    return WriteFileAtomically(ManifestPath(), SerializeManifest(mods));
}

ModError ModLibrary::Install(std::vector<std::unique_ptr<ArchiveSource>>& volumes,
                             const std::vector<ModCandidate>& chosen, const ModSource& source,
                             std::vector<ModRecord>& mods, std::int64_t now, std::stop_token stop,
                             const ArchiveProgress& progress, std::size_t& done, std::string& detail) {
    done = 0;
    std::uint64_t total = 0;
    for (const auto& candidate : chosen) total += candidate.bytes;
    const auto free = FreeSpace();
    if (free < SpaceMargin || total > free - SpaceMargin) return ModError::Space;

    std::uint64_t before = 0;
    for (const auto& candidate : chosen) {
        const auto staging = NewScratch("staging");
        // Declared sizes can lie: allow what is left of the free space, never more.
        const auto limit = FreeSpace() > SpaceMargin ? FreeSpace() - SpaceMargin : 0;
        const auto step = [&](std::uint64_t written, std::uint64_t) {
            if (progress) progress(before + written, std::max(total, before + written));
        };
        const auto error = ExtractArchive(volumes, candidate.files, staging, limit, stop, step, &detail);
        std::error_code ec;
        if (error != ArchiveError::None) {
            fs::remove_all(staging, ec);
            return FromArchiveError(error);
        }
        // Every folder on disk counts, reserved ones included.
        std::vector<std::string> taken = FolderNames(TitleRoot());
        taken.insert(taken.end(), reserved.begin(), reserved.end());
        const auto folder = FolderName(candidate.name, taken);
        fs::create_directories(TitleRoot(), ec);
        fs::rename(staging, FolderPath(folder), ec);
        if (ec) {
            detail = "cannot place " + folder + ": " + ec.message();
            fs::remove_all(staging, ec);
            return ModError::Write;
        }
        ModRecord record;
        record.folder = folder;
        record.name = candidate.name;
        record.source = source;
        record.installed = now;
        mods.insert(mods.begin(), std::move(record));
        if (!Save(mods)) {
            detail = "cannot save the mod list";
            return ModError::Write;
        }
        before += candidate.bytes;
        ++done;
    }
    return ModError::None;
}

fs::path ModLibrary::SaveFolder() const {
    // The save factory's layouts: user/save/0000000000000000/<user>/<title id>, and the newer
    // user/save/account/<user>/<title id>/0 that it prefers when it exists.
    std::vector<fs::path> saves;
    std::error_code ec;
    const auto title = TitleFolder(title_id);
    for (fs::directory_iterator it{paths.saves / "user" / "save" / "0000000000000000", ec}, end; !ec && it != end;
         it.increment(ec))
        saves.push_back(it->path() / title);
    ec.clear();
    for (fs::directory_iterator it{paths.saves / "user" / "save" / "account", ec}, end; !ec && it != end;
         it.increment(ec))
        saves.push_back(it->path() / title / "0");
    // A directory's time moves when files are added, not when they are rewritten: its files' too.
    fs::path newest;
    fs::file_time_type newest_time{};
    for (const auto& save : saves) {
        std::error_code inner;
        if (!fs::is_directory(save, inner)) continue;
        auto written = fs::last_write_time(save, inner);
        for (fs::directory_iterator it{save, inner}, end; !inner && it != end; it.increment(inner)) {
            std::error_code time;
            const auto when = fs::last_write_time(it->path(), time);
            if (!time) written = std::max(written, when);
        }
        if (newest.empty() || written > newest_time) {
            newest = save;
            newest_time = written;
        }
    }
    return newest;
}

ModError ModLibrary::InstallSave(std::vector<std::unique_ptr<ArchiveSource>>& volumes, const ModCandidate& save,
                                 std::stop_token stop, const ArchiveProgress& progress, std::string& detail) {
    const auto target = SaveFolder();
    if (target.empty()) return ModError::NoSave;
    const auto free = FreeSpace();
    if (free < SpaceMargin || save.bytes > free - SpaceMargin) return ModError::Space;
    // The new save is built beside the old one, from a copy of it: files the archive does not
    // bring stay as they were.
    const auto staging = NewScratch("staging");
    std::error_code ec;
    fs::create_directories(staging.parent_path(), ec);
    fs::copy(target, staging, fs::copy_options::recursive, ec);
    if (ec) {
        detail = "cannot copy the save: " + ec.message();
        fs::remove_all(staging, ec);
        return ModError::Write;
    }
    const auto limit = FreeSpace() > SpaceMargin ? FreeSpace() - SpaceMargin : 0;
    const auto error = ExtractArchive(volumes, save.files, staging, limit, stop, progress, &detail);
    if (error != ArchiveError::None) {
        fs::remove_all(staging, ec);
        return FromArchiveError(error);
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    const auto backup = paths.data / "save-backups" / TitleFolder(title_id) / std::to_string(stamp);
    fs::create_directories(backup.parent_path(), ec);
    fs::rename(target, backup, ec);
    if (ec) {
        detail = "cannot back up the save: " + ec.message();
        fs::remove_all(staging, ec);
        return ModError::Write;
    }
    fs::rename(staging, target, ec);
    if (ec) {
        detail = "cannot place the save: " + ec.message();
        std::error_code undo;
        fs::rename(backup, target, undo);
        fs::remove_all(staging, undo);
        return ModError::Write;
    }
    return ModError::None;
}

ModError ModLibrary::Remove(std::vector<ModRecord>& mods, const std::string& folder) {
    const auto record = std::find_if(mods.begin(), mods.end(),
                                     [&](const ModRecord& mod) { return mod.folder == folder; });
    if (record == mods.end()) return ModError::None;
    std::error_code ec;
    const auto path = FolderPath(folder);
    if (fs::exists(path, ec)) {
        const auto trash = NewScratch("trash");
        fs::create_directories(trash.parent_path(), ec);
        fs::rename(path, trash, ec);
        if (ec) return ModError::Write;
        fs::remove_all(trash, ec); // What is left goes with the next Load.
    }
    mods.erase(record);
    return Save(mods) ? ModError::None : ModError::Write;
}

ModError ModLibrary::Replace(std::vector<ModRecord>& mods, const std::string& old, const std::string& fresh) {
    const auto find = [&](const std::string& folder) {
        return std::find_if(mods.begin(), mods.end(), [&](const ModRecord& mod) { return mod.folder == folder; });
    };
    auto from = find(fresh);
    auto to = find(old);
    if (from == mods.end() || to == mods.end() || from == to) return ModError::None;
    from->enabled = to->enabled;
    if (from < to) std::rotate(from, from + 1, to + 1);
    else std::rotate(to, from, from + 1);
    // `old` sits right after `fresh` now, or right before it: Remove finds it by name either way.
    return Remove(mods, old);
}

} // namespace Jadyx::Mods
