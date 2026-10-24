// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

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
} // namespace Jadyx
