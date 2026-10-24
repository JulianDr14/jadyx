// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "jadyx/storage/atomic_file.h"
#include "jadyx/storage/nxbox_import.h"
#include <utility>

namespace Jadyx::NxboxImport {
inline std::optional<std::string> ReadTransfer(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::file_size(path, error) > MaxFileBytes || error)
        return std::nullopt;
    return ReadWholeFile(path);
}
enum class ImportResult { Absent, AlreadyImported, Imported, Invalid, StorageError };
// No marker before every destination is persisted. A retry never replaces a saved setting.
// The sender must finish the transfer before launching Jadyx (settings.json is published last).
template <typename Has, typename Write>
ImportResult ImportTransfer(const std::filesystem::path& directory,
                            const std::filesystem::path& pending, Has&& has, Write&& write) {
    try {
        if (!std::filesystem::is_directory(directory))
            return ImportResult::Absent;
        if (std::filesystem::exists(directory / "imported.flag"))
            return ImportResult::AlreadyImported;
        const auto settings_text = ReadTransfer(directory / "settings.json");
        const auto library_text = ReadTransfer(directory / "library.json");
        const auto receipt_text = ReadTransfer(directory / "receipt.json");
        if (!settings_text || !library_text || !receipt_text)
            return ImportResult::Invalid;
        const auto settings = ParseSettings(*settings_text);
        const auto library = ParseLibrary(*library_text);
        const auto receipt = ParseReceipt(*receipt_text);
        if (!settings || !library || !receipt)
            return ImportResult::Invalid;
        ApplySettings(*settings, std::forward<Has>(has), std::forward<Write>(write));
        if (!WriteFileAtomically(pending, SerializeLibrary(*library)) ||
            !WriteFileAtomically(directory / "imported.flag", "jadyx-nxbox-import 1\n"))
            return ImportResult::StorageError;
        return ImportResult::Imported;
    } catch (...) {
        return ImportResult::StorageError;
    }
}
// Title IDs survive package/path changes. Resolve each record once to a real Jadyx launch path;
// retain missing USB titles until the user authorizes their folder. Destination writes precede
// replacing the pending list, so max counters and set union make crash recovery idempotent.
template <typename EntryRange, typename FavoriteSet, typename HistoryMap>
std::vector<Game> ResolveGames(const std::vector<Game>& games, const EntryRange& entries,
                               FavoriteSet& favorites, HistoryMap& history) {
    std::vector<Game> pending;
    for (const auto& game : games) {
        const auto found = std::ranges::find_if(
            entries, [&](const auto& entry) { return entry.program_id == game.program_id; });
        if (found == entries.end()) {
            pending.push_back(game);
            continue;
        }
        if (game.pinned)
            favorites.insert(found->launch_path);
        auto& record = history[found->launch_path];
        MergeCounters(game, record.seconds);
        record.last_played = std::max(record.last_played, game.last_played);
        record.sessions = std::max(record.sessions, game.sessions);
        record.program_id = game.program_id;
    }
    return pending;
}
} // namespace Jadyx::NxboxImport
