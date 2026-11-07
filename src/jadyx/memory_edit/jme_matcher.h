// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit: finds a value in one 4 KiB page. Only at addresses aligned to its width, as a
// game keeps them, so no value crosses a page.

#include <cstddef>

#include "common/common_types.h"
#include "jadyx/memory_edit/jme_value.h"

namespace Jadyx::MemoryEdit {

inline constexpr size_t MATCH_PAGE_SIZE = 4096;
/// Words of the largest hit map: a bit per byte of the page.
inline constexpr size_t MATCH_WORDS = MATCH_PAGE_SIZE / 64;

/// The words of a hit map for `kind`.
[[nodiscard]] constexpr size_t MatchWords(ValueKind kind) {
    return MATCH_WORDS / ValueWidth(kind);
}

/// Clears the map and sets bit (offset / width) for each hit; returns how many. NaN matches nothing.
u32 MatchPage(const Query& query, const u8* page, u64* bits);
u32 MatchPageScalar(const Query& query, const u8* page, u64* bits);

} // namespace Jadyx::MemoryEdit
