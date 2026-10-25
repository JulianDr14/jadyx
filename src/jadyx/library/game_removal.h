// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Deleting a game from the console: its dumps in internal storage, its Pulse sessions and, when
// no other copy of it stays in the library, its saves and updates/DLC. Shared by the library's game options and
// Jadyx Portal. Only the library coordinator's serial storage worker runs it, so it never
// races an import, the content manager or the metadata reader.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include "jadyx/library/game_library.h"

namespace Jadyx {

/// The dumps of one application to delete.
struct GameFiles {
    std::uint64_t application{};
    std::vector<std::filesystem::path> files; ///< Dumps to delete: relative to the games root, or absolute in Dev Mode's storage.
    std::vector<std::string> launch_paths;    ///< Those dumps, as the Pulse sessions name them.
    bool external{}; ///< Nothing to delete: the dumps live in external folders Jadyx does not delete from.
    bool kept{};     ///< Another dump stays (external, or not chosen): so do the saves and updates/DLC.
    bool operator==(const GameFiles&) const = default;
};

/// Every internal dump of `application` (a base title id) in a library scan.
GameFiles FindGameFiles(std::span<const LibraryEntry> entries, std::uint64_t application);
/// Only the dump at `launch_path`: the copy chosen in the console's game options.
GameFiles FindDumpFiles(std::span<const LibraryEntry> entries, const std::string& launch_path);

/// Deletes the dumps and their Pulse sessions; unless a copy is kept, also the saves and the
/// updates/DLC. Mods and cheats stay, they are reused if the game comes back. Returns an error
/// word, empty on success: "external" (nothing deleted), "game", "content" or "saves".
std::string RemoveGame(const std::filesystem::path& root, const GameFiles& game);

} // namespace Jadyx
