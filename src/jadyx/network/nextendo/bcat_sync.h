// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fetches a title's Nextendo BCAT package before the game runs and installs it into the folder the
// emulated bcat services read (bcat_package.h has the rules). The ZIP is checked, unpacked into a
// staging folder next to the cache and swapped in, so a failure at any step leaves the previous
// cache as it was. A 204, a network error or a timeout never touch it either: the game starts
// with what it already had.
//
// Blocks for at most `budget`; run it on the boot thread before any guest code.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Jadyx::Network::Nextendo {

enum class BcatSync : std::uint8_t {
    NothingPublished, ///< 204: the title has no package.
    UpToDate,         ///< Same package as the one installed.
    Installed,        ///< A new package replaced the cache.
    Kept,             ///< No answer, another status, or a write failed: the cache is unchanged.
    Refused,          ///< The package broke the rules: the cache is unchanged.
};

struct BcatSyncResult {
    BcatSync outcome = BcatSync::Kept;
    std::string detail; ///< For the log: the status, the error or the counts.
};

/// title_id: the base title. cache: that title's delivery cache folder (created when missing).
BcatSyncResult SyncBcat(std::uint64_t title_id, const std::filesystem::path& cache,
                        std::chrono::seconds budget);

const char* BcatSyncName(BcatSync outcome);

} // namespace Jadyx::Network::Nextendo
