// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Deleting a game from the console: every dump of the application in internal storage, its
// saves, its updates/DLC and its Pulse sessions. Shared by the library's game options and
// Jadyx Portal. Only the library coordinator's serial storage worker runs it, so it never
// races an import, the content manager or the metadata reader.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include "jadyx/library/game_library.h"

namespace Jadyx {

/// Where one application lives in the library.
struct GameFiles {
    std::uint64_t application{};
    std::vector<std::filesystem::path> files; ///< Internal dumps, relative to the games root.
    std::vector<std::string> launch_paths;    ///< Every dump, as the Pulse sessions name it.
    bool external{}; ///< A dump lives in an external folder Jadyx does not delete from.
    bool operator==(const GameFiles&) const = default;
};

/// Collects the dumps of `application` (a base title id) from a library scan.
GameFiles FindGameFiles(std::span<const LibraryEntry> entries, std::uint64_t application);

/// Deletes the game, its saves, its updates/DLC and its Pulse sessions. Mods and cheats stay,
/// they are reused if the game comes back. Returns an error word, empty on success:
/// "external" (nothing deleted), "game", "content" or "saves".
std::string RemoveGame(const std::filesystem::path& root, const GameFiles& game);

} // namespace Jadyx
