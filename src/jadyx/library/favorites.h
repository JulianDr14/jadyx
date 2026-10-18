// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pinned games: the player's favorites, first in Home's "Recent and pinned" row and the
// Library's Pinned filter. Kept in LocalState/jadyx/favorites.txt, one launch path per line.
// Pure logic up to the persistence block at the end.

#include <functional>
#include <set>
#include <string>
#include <string_view>

namespace Jadyx {

/// Launch paths (LibraryEntry::launch_path) of the pinned games.
using Favorites = std::set<std::string, std::less<>>;

namespace FavoritesDetail {
inline constexpr std::string_view Header = "jadyx-favorites 1";
}

/// Unreadable or empty lines are skipped; an unknown header reads as no favorites.
inline Favorites ParseFavorites(std::string_view text) {
    Favorites favorites;
    bool header = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!header) {
            if (line != FavoritesDetail::Header) return {};
            header = true;
            continue;
        }
        if (!line.empty()) favorites.emplace(line);
    }
    return favorites;
}

inline std::string SerializeFavorites(const Favorites& favorites) {
    std::string out{FavoritesDetail::Header};
    out += '\n';
    for (const auto& path : favorites) {
        if (path.empty() || path.find_first_of("\r\n") != std::string::npos) continue;
        out += path;
        out += '\n';
    }
    return out;
}

// ---- Persistence (favorites.cpp) ----

Favorites LoadFavorites();
bool SaveFavorites(const Favorites& favorites);

} // namespace Jadyx
