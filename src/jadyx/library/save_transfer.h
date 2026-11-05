// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A game's save as Jadyx Portal moves it: downloaded as a .zip of the save folder's contents,
// and loaded back from a .zip, .7z or .rar the player made anywhere (this app, Eden or yuzu on a
// PC, Ryujinx...). What arrives is not ours, so every file of the archive is checked from its
// listing before anything is written; the new save is extracted beside the old one, which is
// moved to a backup only once the new one is complete. Every failure is one portal error word,
// optionally followed by ":" and a short detail the page shows (a file name, the ids found).

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>
#include "jadyx/mods/mod_archive.h"

namespace Jadyx {

/// At most this many files and this many bytes in a save, both ways.
inline constexpr std::size_t SaveMaxFiles = 8192;
inline constexpr std::uint64_t SaveMaxImportBytes = 1ull << 30;
inline constexpr std::uint64_t SaveMaxExportBytes = 512ull << 20;

/// The files an uploaded archive puts in the save, or why it cannot.
struct SavePlan {
    std::string error;  ///< Portal error word; empty when the archive is a save of this game.
    std::string detail; ///< What the page shows with the error (UTF-8, short), may be empty.
    std::vector<Mods::LayoutFile> files;
    std::uint64_t bytes{};
};

/// Checks an archive listing and maps it onto the save folder. Pure: tested without a disk.
/// Ignores what Mac and Windows add to archives, and steps into the folders a save is usually
/// zipped in (the title id, "0", Ryujinx's bis/user/save/<n>/0...).
SavePlan PlanSaveImport(const std::vector<Mods::ArchiveItem>& items, std::uint64_t application);

/// Zips `folder`'s contents (stored, no compression) into `zip`. Returns an error word.
std::string ExportSaveFolder(const std::filesystem::path& folder, std::string& zip);

/// Replaces `target` with the save in `archive`; the previous one is moved under `backups`.
/// Returns an error word, with ":detail" when there is one; `target` is unchanged on failure.
/// `log`, when given, hears what happened in words for the log (the library's reason, the backup).
std::string ImportSaveArchive(const std::filesystem::path& archive, std::uint64_t application,
                              const std::filesystem::path& target, const std::filesystem::path& backups,
                              std::stop_token stop, const Mods::ArchiveProgress& progress = {},
                              std::string* log = nullptr);

} // namespace Jadyx
