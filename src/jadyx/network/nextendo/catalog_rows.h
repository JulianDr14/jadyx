// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Nextendo catalog as the Online tab lists it: every game the project serves, crossed with
// the games installed here, with a verdict per row. Pure, so the desktop gate covers it.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/online/nextendo/catalog.h"

namespace Jadyx::Network::Nextendo {

/// A game of the library: its base title id, its name and its version with the update applied.
struct InstalledTitle {
    std::uint64_t title_id = 0;
    std::string name;
    std::string version;
};

/// Ordered as listed: what is installed and playable first.
enum class RowVerdict : std::uint8_t {
    Ready,        // installed, enabled, and this version is accepted
    WrongVersion, // installed, but the server wants another version
    NeedsUpdate,  // needs a capability this build of Jadyx lacks
    Disabled,     // switched off by the project for now
    NotInstalled, // served, but not in this library
};

struct CatalogRow {
    std::string name;            // the catalog's, else the library's, else the title id in hex
    std::uint64_t title_id = 0;  // the installed one, or the catalog's first
    RowVerdict verdict = RowVerdict::NotInstalled;
    bool installed = false;
    std::string installed_version;   // empty when not installed
    std::string recommended_version; // what the catalog recommends; may be empty
    /// The row's game in `installed`, or NoInstalled. With several copies of a title, the best:
    /// an accepted version first, then any known version, then one whose version is unknown.
    std::size_t installed_index = NoInstalled;
    static constexpr std::size_t NoInstalled = ~std::size_t{0};
};

/// One row per catalog game. Installed games come first, by verdict then name; the rest by name.
std::vector<CatalogRow> BuildCatalogRows(const Core::Online::Nextendo::GameCatalog& catalog,
                                         std::span<const InstalledTitle> installed);

/// "0100000000010000": how a title id reads when the catalog gives no name.
std::string TitleIdText(std::uint64_t title_id);

} // namespace Jadyx::Network::Nextendo
