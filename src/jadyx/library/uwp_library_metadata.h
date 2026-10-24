// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/settings_enums.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/library_cache.h"
namespace Jadyx {
struct MetadataSource {
    std::filesystem::path relative_path;
    std::string launch_path;
};
struct MetadataOptions {
    Settings::Language language{Settings::Language::EnglishAmerican}; ///< Title language.
    bool icons{};      ///< Request visible covers; completed background fills also publish icons.
    bool cache_only{}; ///< Never open a game: only what the cache already has.
};
// The cache first; on a miss, the same loaders as qt_common/game_list/worker.cpp, and the result
// is stored. A Core::System (filesystem services only, no guest Initialize/Load/Run) is created
// only for the first miss, so a fully cached page costs a few file reads.
void ReadLibraryMetadata(const std::filesystem::path& root, std::vector<LibraryEntry>& entries,
                         const LibraryCache& cache, MetadataOptions options, std::stop_token stop,
                         const std::vector<MetadataSource>& sources);
}
