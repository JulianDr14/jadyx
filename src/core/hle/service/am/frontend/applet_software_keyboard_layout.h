// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Service::AM::Frontend {

/// Which KeyboardConfig tail follows SwkbdConfigCommon in the normal keyboard's config storage.
enum class SwkbdConfigLayout {
    Old,      ///< 0x3E0 bytes: 1.0.0 and 2.0.0 applets.
    Old2,     ///< 0x400 bytes: 3.0.0 to 5.1.0 applets.
    New,      ///< 0x4C8 bytes: 6.0.0 and every later applet.
    Truncated ///< Shorter than the first layout's size, or between two known sizes.
};

inline constexpr std::size_t SwkbdConfigSizeOld = 0x3E0;
inline constexpr std::size_t SwkbdConfigSizeOld2 = 0x400;
inline constexpr std::size_t SwkbdConfigSizeNew = 0x4C8;

/// Picks the layout from the storage size, which is what fixes the layout (switchbrew: 0x3E0, then
/// 0x400 from 3.0.0, then 0x4C8 from 6.0.0). The library version only tells which SDK built the game:
/// newer SDKs (0x10000E and up) keep the 0x4C8 layout, so a size match beats a version table that
/// would need a new entry per SDK. Larger storages are read as the newest layout plus unused bytes.
[[nodiscard]] constexpr SwkbdConfigLayout SwkbdConfigLayoutForSize(std::size_t size) {
    if (size == SwkbdConfigSizeOld) {
        return SwkbdConfigLayout::Old;
    }
    if (size == SwkbdConfigSizeOld2) {
        return SwkbdConfigLayout::Old2;
    }
    if (size >= SwkbdConfigSizeNew) {
        return SwkbdConfigLayout::New;
    }
    return SwkbdConfigLayout::Truncated;
}

} // namespace Service::AM::Frontend
