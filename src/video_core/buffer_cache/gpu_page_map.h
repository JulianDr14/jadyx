// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>

#include "common/common_types.h"

namespace VideoCommon {

/// Which 64 KiB pages may hold a range the GPU wrote: a superset of the buffer cache's
/// gpu_modified_ranges, so "no page set" answers "not modified" without walking the range set.
/// A page stays set after its ranges are subtracted, until the map is rebuilt from the set.
class GpuPageMap {
public:
    static constexpr u32 PAGE_BITS = 16;
    static constexpr u64 NUM_PAGES = 1ULL << 18; // 16 GiB of device address space
    /// Answers of "maybe" the range set denied, after a subtract, that pay for a rebuild.
    static constexpr u32 REBUILD_AFTER = 64;

    void Mark(u64 addr, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        const u64 first = addr >> PAGE_BITS;
        const u64 last = (addr + size - 1) >> PAGE_BITS;
        if (last >= NUM_PAGES) {
            beyond = true;
        }
        for (u64 page = first; page <= last && page < NUM_PAGES; ++page) {
            words[page >> 6] |= 1ULL << (page & 63);
        }
    }

    void Reset() noexcept {
        std::memset(words.data(), 0, sizeof(words));
        beyond = false;
        stale = false;
        denied = 0;
    }

    /// False only when no GPU-written range can touch [addr, addr + size).
    [[nodiscard]] bool MayIntersect(u64 addr, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const u64 first = addr >> PAGE_BITS;
        const u64 last = (addr + size - 1) >> PAGE_BITS;
        if (last >= NUM_PAGES) {
            return true;
        }
        const u64 first_word = first >> 6;
        const u64 last_word = last >> 6;
        const u64 head = ~0ULL << (first & 63);
        const u64 tail = ~0ULL >> (63 - (last & 63));
        if (first_word == last_word) {
            return (words[first_word] & head & tail) != 0;
        }
        if ((words[first_word] & head) != 0 || (words[last_word] & tail) != 0) {
            return true;
        }
        for (u64 word = first_word + 1; word < last_word; ++word) {
            if (words[word] != 0) {
                return true;
            }
        }
        return false;
    }

    /// A range left the set: the pages it covered may now be set for nothing.
    void NoteSubtract() noexcept {
        stale = true;
    }

    /// The range set denied a "maybe". True when the map is worth rebuilding.
    [[nodiscard]] bool NoteDenied() noexcept {
        return stale && ++denied >= REBUILD_AFTER;
    }

    [[nodiscard]] bool Beyond() const noexcept {
        return beyond;
    }

private:
    std::array<u64, NUM_PAGES / 64> words{};
    bool beyond{}; ///< a range past the map exists: those addresses always answer "maybe"
    bool stale{};
    u32 denied{};
};

} // namespace VideoCommon
