// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/common_types.h"
#include "dynarmic/common/crypto/crc32.h"

namespace Dynarmic::Backend::X64 {

/// One core's indirect-branch cache: guest location descriptor -> host entrypoint, read by the
/// A64 fast-dispatch terminal handler. The handler finds it through the core's JIT state, so
/// cores sharing one code cache each keep their own table and fill it without synchronization.
class FastDispatchTable {
public:
    struct Entry {
        u64 location_descriptor = ~u64{0};
        const void* code_ptr = nullptr;
    };
    static_assert(sizeof(Entry) == 0x10);

    // 256K entries, 4 MiB a core. A million (16 MiB, all of it committed by the fill on every
    // clear) cost the four cores 64 MiB of a commit-limited console for an indirect-branch cache.
    static constexpr std::size_t entry_count = 0x40000;
    /// Mask of the byte offset of a slot (entry_count entries of sizeof(Entry) bytes).
    static constexpr u32 offset_mask = static_cast<u32>((entry_count - 1) * sizeof(Entry));
    static_assert((entry_count & (entry_count - 1)) == 0, "entry_count must be a power of two");

    /// `crc32` must match what the emitted handler does: CRC32 r64,r64 when the host has SSE4.2.
    explicit FastDispatchTable(bool crc32)
        : entries{std::make_unique<Entry[]>(entry_count)}, crc32{crc32} {}

    FastDispatchTable(const FastDispatchTable&) = delete;
    FastDispatchTable& operator=(const FastDispatchTable&) = delete;

    [[nodiscard]] Entry* Data() noexcept { return entries.get(); }
    [[nodiscard]] const Entry* Data() const noexcept { return entries.get(); }

    /// Byte offset of `descriptor`'s slot. Matches the handler: the low 32 bits of the
    /// descriptor seed a CRC32-C over the eight bytes of the table's address.
    [[nodiscard]] u32 SlotOffset(u64 descriptor) const noexcept {
        const u64 hash = crc32 ? Common::Crypto::CRC32::ComputeCRC32Castagnoli(
                                     static_cast<u32>(descriptor), reinterpret_cast<u64>(Data()), sizeof(u64))
                               : descriptor;
        return static_cast<u32>(hash) & offset_mask;
    }

    [[nodiscard]] Entry& Slot(u64 descriptor) noexcept {
        return entries[SlotOffset(descriptor) / sizeof(Entry)];
    }

    /// Drops `descriptor` if it holds its slot. A slot holding another descriptor is left alone.
    void Invalidate(u64 descriptor) noexcept {
        Entry& entry = Slot(descriptor);
        if (entry.location_descriptor == descriptor) {
            entry = {};
        }
    }

    void Clear() noexcept {
        std::fill_n(entries.get(), entry_count, Entry{});
    }

private:
    std::unique_ptr<Entry[]> entries;
    bool crc32;
};

}  // namespace Dynarmic::Backend::X64
