// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/div_ceil.h"

/// Guest local memory of fragment shaders kept in a storage buffer instead of a private array,
/// for hosts whose pixel shaders lose large private arrays (Profile::fragment_local_memory_
/// scratch_lanes). Small arrays stay private: drivers keep them in registers.
///
/// Each wave takes a slot of the pool when it starts and frees it when its last live lane leaves,
/// at the epilogue or at a demote, so slots never leak while waves run concurrently across draws.
/// Allocation and release run on one lane per wave (elected among the live ones), since D3D12 drops
/// the stores and atomics of helper lanes.
///
/// Buffer layout, in 32-bit words:
///  - [HINT_WORD]: rotating start of the bitmap search
///  - [OVERFLOW_WORD]: waves that found no free slot and shared OVERFLOW_SLOT (lost data, no hang)
///  - [BITMAP_BASE, +BITMAP_WORDS): occupied slots
///  - [MASK_BASE, +SLOTS * MASK_WORDS): live lanes of each occupied slot
///  - [DATA_BASE, ...): data rows. Word w of lane l in slot s is at
///    DATA_BASE + (w * TOTAL_SLOTS + s) * lanes + l: lanes of a wave touch consecutive words, and a
///    shader only touches the rows it needs, so the pool grows by rows without moving slots.
namespace Shader::LocalMemoryScratch {

/// Descriptor set (register space in D3D12) and binding of the pool.
constexpr u32 DESCRIPTOR_SET = 28;
constexpr u32 BINDING = 0;

/// Arrays up to this many words stay private (64 words measured as register-promoted on the
/// Series; 128 lose their data).
constexpr u32 MAX_PRIVATE_WORDS = 64;
/// Larger local memory stays private too: it would need a pool too large for console memory.
constexpr u32 MAX_WORDS = 4096;

constexpr u32 SLOTS = 128;
constexpr u32 OVERFLOW_SLOT = SLOTS;
constexpr u32 TOTAL_SLOTS = SLOTS + 1;
/// A ballot has 128 bits.
constexpr u32 MASK_WORDS = 4;
/// Bitmap search attempts before falling back to OVERFLOW_SLOT; each one rereads a bitmap word, so
/// a wave waits for a slot to free instead of failing at the first full scan.
constexpr u32 ACQUIRE_ATTEMPTS = 1024;

constexpr u32 HINT_WORD = 0;
constexpr u32 OVERFLOW_WORD = 1;
constexpr u32 BITMAP_BASE = 16;
constexpr u32 BITMAP_WORDS = SLOTS / 32;
constexpr u32 MASK_BASE = BITMAP_BASE + BITMAP_WORDS;
constexpr u32 DATA_BASE = MASK_BASE + SLOTS * MASK_WORDS;
static_assert(SLOTS % 32 == 0);

/// Words of local memory a fragment shader keeps in the pool, or 0 to keep a private array.
[[nodiscard]] constexpr u32 PoolWords(u32 lanes, u32 local_memory_size) noexcept {
    const u32 words = Common::DivCeil(local_memory_size, 4U);
    if (lanes == 0 || words <= MAX_PRIVATE_WORDS || words > MAX_WORDS) {
        return 0;
    }
    return words;
}

/// Bytes of a pool with the given rows of data.
[[nodiscard]] constexpr u64 PoolBytes(u32 lanes, u32 rows) noexcept {
    return (u64{DATA_BASE} + u64{rows} * TOTAL_SLOTS * lanes) * sizeof(u32);
}

} // namespace Shader::LocalMemoryScratch
