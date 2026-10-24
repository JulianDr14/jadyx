// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "jadyx/library/game_library.h"

namespace Jadyx {
inline bool UsbPlayInPlaceEnabled(const char* value) {
    return value && std::string_view{value} == "1";
}

// Compare broker-provided Windows folder paths without touching the filesystem.
// A previously selected subfolder takes precedence over automatic drive discovery.
inline bool StorageFoldersOverlap(std::wstring_view first, std::wstring_view second) {
    if (first.empty() || second.empty()) {
        return false;
    }
    const auto normalize = [](std::wstring_view path) {
        std::wstring result{path};
        for (auto& character : result) {
            if (character == L'\\') {
                character = L'/';
            } else if (character >= L'A' && character <= L'Z') {
                character = static_cast<wchar_t>(character + (L'a' - L'A'));
            }
        }
        while (!result.empty() && result.back() == L'/') {
            result.pop_back();
        }
        return result;
    };
    const auto left = normalize(first);
    const auto right = normalize(second);
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto contains = [](std::wstring_view parent, std::wstring_view child) {
        return child == parent || (child.starts_with(parent) && child.size() > parent.size() &&
                                   child[parent.size()] == L'/');
    };
    return contains(left, right) || contains(right, left);
}
// A root left in FutureAccessList by the old discovery code must not take
// precedence over an explicitly selected subfolder on the same drive.
inline bool IsStorageDriveRoot(std::wstring_view path) {
    return path.size() >= 2 && path[1] == L':' &&
           path.find_first_not_of(L"/\\", 2) == std::wstring_view::npos;
}
inline bool ShouldDiscoverUsbDrive(std::wstring_view drive,
                                   const std::vector<std::wstring>& registered) {
    return !drive.empty() &&
           std::none_of(registered.begin(), registered.end(),
                        [&](const auto& path) { return StorageFoldersOverlap(drive, path); });
}
inline bool ShouldScanStorageSource(std::wstring_view path,
                                    const std::vector<std::wstring>& registered) {
    return !IsStorageDriveRoot(path) ||
           std::none_of(registered.begin(), registered.end(), [&](const auto& other) {
               return !IsStorageDriveRoot(other) && StorageFoldersOverlap(path, other);
           });
}

// Automatic and legacy whole-drive sources only inspect these folders. Files
// at the root or in an allowed folder are candidates; never recurse below two
// directory components. Explicitly selected subfolders retain the normal depth.
inline bool ShouldDescendUsbFolder(std::string_view relative) {
    std::string lower{relative};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lower == "games" || lower == "switch" || lower == "nxbox" || lower == "jadyx" ||
           lower == "nxbox/games" || lower == "jadyx/games";
}
struct StorageScanBudget {
    unsigned visited{};
    bool limited{};
    bool Visit() {
        if (visited >= 10000) {
            limited = true;
            return false;
        }
        ++visited;
        return true;
    }
};
inline void AppendStorageScan(LibraryScan& target, LibraryScan source) {
    target.limited |= source.limited;
    target.entries.insert(target.entries.end(), std::make_move_iterator(source.entries.begin()),
                          std::make_move_iterator(source.entries.end()));
}
} // namespace Jadyx
