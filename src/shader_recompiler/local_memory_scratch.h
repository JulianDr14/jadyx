// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/div_ceil.h"

/// Guest local memory of fragment shaders kept in a storage buffer instead of a private array,
/// for hosts whose pixel shaders lose large private arrays (Profile::fragment_local_memory_
/// scratch_lanes). Small arrays stay private: drivers keep them in registers. So do arrays only
/// accessed at constant offsets, whatever their size: only indexed access loses its values.
///
/// Each wave takes a slot of the pool when it starts and frees it when its last live lane leaves,
/// at the epilogue or at a demote, so slots never leak while waves run concurrently across draws.
/// Allocation and release run on one lane per wave (elected among the live ones), since D3D12 drops
/// the stores and atomics of helper lanes.
///
/// The slot count is not part of the shader: the host writes it in the header of each pool it
/// creates, and grows the pool when its counters show waves waiting for slots, so shaders never
/// recompile and every GPU gets as many slots as its waves in flight need.
///
/// Buffer layout, in 32-bit words:
///  - [ACQUIRE_WORD]: waves that took a slot; also the rotating start of the bitmap search
///  - [OVERFLOW_WORD]: waves that found no free slot and shared the overflow slot (lost data)
///  - [WAIT_WORD]: waves that scanned the whole bitmap before finding a free slot
///  - [BITMAP_WORDS_WORD]: bitmap words of this pool (slots / 32), written by the host
///  - [BITMAP_BASE, +bitmap words): occupied slots
///  - [MASK_BASE, +MAX_SLOTS * MASK_WORDS): live lanes of each occupied slot
///  - [DATA_BASE, ...): data rows. Word w of lane l in slot s is at
///    DATA_BASE + (w * (slots + 1) + s) * lanes + l, where slot `slots` is the overflow slot: lanes
///    of a wave touch consecutive words, and a shader only touches the rows it needs.
namespace Shader::LocalMemoryScratch {

/// Descriptor set (register space in D3D12) and binding of the pool.
constexpr u32 DESCRIPTOR_SET = 28;
constexpr u32 BINDING = 0;

/// Arrays up to this many words stay private (64 words measured as register-promoted on the
/// Series; 128 lose their data).
constexpr u32 MAX_PRIVATE_WORDS = 64;
/// Larger local memory stays private too: it would need a pool too large for console memory.
constexpr u32 MAX_WORDS = 4096;

/// Slot counts a pool may have; always a power of two.
constexpr u32 MIN_SLOTS = 128;
constexpr u32 MAX_SLOTS = 2048;
constexpr u32 MIN_BITMAP_WORDS = MIN_SLOTS / 32;
constexpr u32 MAX_BITMAP_WORDS = MAX_SLOTS / 32;
/// Slot of the waves that found the pool full.
constexpr u32 INVALID_SLOT = 0xFFFFFFFF;
/// A ballot has 128 bits.
constexpr u32 MASK_WORDS = 4;
/// Bitmap search attempts before falling back to the overflow slot; each one rereads a bitmap
/// word, so a wave waits for a slot to free instead of failing at the first full scan.
constexpr u32 ACQUIRE_ATTEMPTS = 1024;

constexpr u32 ACQUIRE_WORD = 0;
constexpr u32 OVERFLOW_WORD = 1;
constexpr u32 WAIT_WORD = 2;
constexpr u32 BITMAP_WORDS_WORD = 3;
constexpr u32 HEADER_WORDS = 4;
constexpr u32 BITMAP_BASE = 16;
constexpr u32 MASK_BASE = BITMAP_BASE + MAX_BITMAP_WORDS;
constexpr u32 DATA_BASE = MASK_BASE + MAX_SLOTS * MASK_WORDS;
static_assert(MIN_SLOTS % 32 == 0 && (MIN_SLOTS & (MIN_SLOTS - 1)) == 0);
static_assert(MAX_SLOTS % 32 == 0 && (MAX_SLOTS & (MAX_SLOTS - 1)) == 0);
static_assert(HEADER_WORDS <= BITMAP_BASE);

/// Words of local memory a fragment shader keeps in the pool, or 0 to keep a private array.
[[nodiscard]] constexpr u32 PoolWords(u32 lanes, u32 local_memory_size) noexcept {
    const u32 words = Common::DivCeil(local_memory_size, 4U);
    if (lanes == 0 || words <= MAX_PRIVATE_WORDS || words > MAX_WORDS) {
        return 0;
    }
    return words;
}

/// Bytes of a pool with the given rows of data and slots (plus the overflow slot).
[[nodiscard]] constexpr u64 PoolBytes(u32 lanes, u32 rows, u32 slots) noexcept {
    return (u64{DATA_BASE} + u64{rows} * (slots + 1) * lanes) * sizeof(u32);
}

} // namespace Shader::LocalMemoryScratch
