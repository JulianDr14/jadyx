// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

/// ETC2 and EAC (OpenGL ES 3.0 / Vulkan core block formats) decoded on the CPU for hosts without
/// them, D3D12 among them. Whole 4x4 blocks at a time; no dependency on the rest of the emulator
/// so tools/xbox/tests/etc2-decode.cpp builds it alone.
namespace Tegra::Texture::ETC2 {

enum class Kind : std::uint8_t {
    Rgb,            ///< ETC2 RGB8 (ETC1 compatible) -> RGBA8, alpha 255
    RgbPunchthrough,///< ETC2 RGB8A1 -> RGBA8, alpha 0 or 255
    Rgba,           ///< ETC2 RGBA8 (EAC alpha block + RGB block) -> RGBA8
    R11,            ///< EAC R11 unsigned -> R16_UNORM, all 11 bits kept
    R11Signed,      ///< EAC R11 signed -> R16_SNORM
    Rg11,           ///< EAC RG11 unsigned -> R16G16_UNORM
    Rg11Signed,     ///< EAC RG11 signed -> R16G16_SNORM
};

/// Bytes of one 4x4 block of the guest format.
[[nodiscard]] constexpr std::size_t BlockBytes(Kind kind) {
    return kind == Kind::Rgb || kind == Kind::RgbPunchthrough || kind == Kind::R11 ||
                   kind == Kind::R11Signed
               ? 8
               : 16;
}

/// Bytes of one decoded texel.
[[nodiscard]] constexpr std::size_t TexelBytes(Kind kind) {
    return kind == Kind::R11 || kind == Kind::R11Signed ? 2 : 4;
}

/// Decodes one block into texels[16] laid out row by row (y * 4 + x), each TexelBytes(kind)
/// wide.
void DecodeBlock(Kind kind, const std::uint8_t* block, std::uint8_t* texels);

/// Decodes slices of width x height texels stored as consecutive rows of blocks (the unswizzled
/// layout), into tightly packed rows. Partial blocks at the right and bottom edges are clipped.
void Decompress(Kind kind, std::span<const std::uint8_t> input, std::uint32_t width,
                std::uint32_t height, std::uint32_t slices, std::span<std::uint8_t> output);

} // namespace Tegra::Texture::ETC2
