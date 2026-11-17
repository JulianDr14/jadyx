// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Dynarmic::Backend::X64::LinkSite {

/// Layout of a block-link instruction whose target lives in one naturally aligned field.
///
/// A link is retargeted by a single aligned store into that field, so a core executing the site
/// while another core relinks it sees either the old or the new target, never a torn instruction:
/// on x86 an aligned 4- or 8-byte field cannot span a cache line, and the instruction bytes around
/// it never change. This is what lets cores share code while one of them links or unlinks blocks.
struct Encoding {
    std::uint8_t opcode_size;  ///< Bytes before the field.
    std::uint8_t field_size;   ///< Bytes of the field: 4 (rel32) or 8 (imm64).
};

inline constexpr Encoding jmp_rel32{1, 4};      ///< E9 rel32
inline constexpr Encoding jcc_rel32{2, 4};      ///< 0F 8x rel32
inline constexpr Encoding mov_rcx_imm64{2, 8};  ///< 48 B9 imm64

/// Nop bytes to emit at `site` so the field after the opcode is naturally aligned.
constexpr std::size_t PaddingFor(std::uintptr_t site, Encoding enc) noexcept {
    const std::uintptr_t field = site + enc.opcode_size;
    return static_cast<std::size_t>((enc.field_size - field % enc.field_size) % enc.field_size);
}

/// Address of the field of a site emitted at `site` (before its padding).
constexpr std::uintptr_t FieldAddress(std::uintptr_t site, Encoding enc) noexcept {
    return site + PaddingFor(site, enc) + enc.opcode_size;
}

/// End of the instruction, where a rel32 is measured from.
constexpr std::uintptr_t InstructionEnd(std::uintptr_t site, Encoding enc) noexcept {
    return FieldAddress(site, enc) + enc.field_size;
}

/// The rel32 from `instruction_end` to `target`, or nullopt when out of range.
constexpr std::optional<std::int32_t> Rel32(std::uintptr_t instruction_end, std::uintptr_t target) noexcept {
    const auto delta = static_cast<std::int64_t>(target - instruction_end);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(delta);
}

static_assert(PaddingFor(0x1000, jmp_rel32) == 3);
static_assert(PaddingFor(0x1003, jmp_rel32) == 0);
static_assert(FieldAddress(0x1001, jcc_rel32) % 4 == 0);
static_assert(FieldAddress(0x1007, mov_rcx_imm64) % 8 == 0);
static_assert(Rel32(0x2000, 0x1000) == -0x1000);

}  // namespace Dynarmic::Backend::X64::LinkSite
