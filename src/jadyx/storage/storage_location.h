// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Dev Mode's own storage on D: (the size players set in "Manage dev storage"). Everything stays
// in LocalState except the games a Jadyx Portal upload sends there: the library lists and deletes
// them next to the internal ones. That storage outlives the app but is erased whenever its size
// changes.

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace Jadyx {

inline const std::filesystem::path& DeveloperFiles() {
    static const std::filesystem::path path{L"D:\\DevelopmentFiles"};
    return path;
}
inline std::filesystem::path DeveloperGames() { return DeveloperFiles() / L"Jadyx" / L"games"; }
inline std::filesystem::path DeveloperTransfers() { return DeveloperFiles() / L"Jadyx" / L"transfers" / L"nsp"; }

/// Dev Mode's storage is present.
inline bool DeveloperStorageAvailable() {
    std::error_code ec;
    return std::filesystem::is_directory(DeveloperFiles(), ec);
}

/// `path` lies under the developer games folder.
inline bool InDeveloperGames(const std::filesystem::path& path) {
    const auto relative = path.lexically_normal().lexically_relative(DeveloperGames());
    return !relative.empty() && relative.is_relative() && *relative.begin() != L"..";
}

/// What an upload leaves free on the volume behind `path`: room for app updates on the developer
/// volume, a margin elsewhere.
inline std::uint64_t StorageReserveFor(const std::filesystem::path& path) {
    const auto& native = path.native();
    const bool developer = native.size() >= 2 && (native[0] == L'D' || native[0] == L'd') && native[1] == L':';
    return developer ? 2ULL << 30 : 64ULL << 20;
}

} // namespace Jadyx
