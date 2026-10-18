// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/favorites.h"

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "jadyx/storage/atomic_file.h"

namespace Jadyx {
namespace {
std::filesystem::path FavoritesPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "favorites.txt";
}
} // namespace

Favorites LoadFavorites() {
    const auto text = ReadWholeFile(FavoritesPath());
    return text ? ParseFavorites(*text) : Favorites{};
}

bool SaveFavorites(const Favorites& favorites) {
    if (WriteFileAtomically(FavoritesPath(), SerializeFavorites(favorites))) return true;
    LOG_ERROR(Frontend, "Favorites: could not save {}", Common::FS::PathToUTF8String(FavoritesPath()));
    return false;
}

} // namespace Jadyx
