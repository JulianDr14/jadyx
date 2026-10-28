// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Tegra::Texture::ASTC {

void Decompress(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                uint32_t block_width, uint32_t block_height, std::span<uint8_t> output);

/// What the blocks of an image decode to, read from their endpoints (no texel is decoded).
struct AlphaScan {
    bool alpha = false;      ///< Some texel has an alpha below 1.
    bool white_mask = false; ///< With alpha: every texel's RGB is 1, 1, 1.
    bool black_mask = false; ///< With alpha: every texel's RGB is 0, 0, 0.
};

/// Blocks are 16 bytes in any order (swizzled guest memory is fine); reserved blocks, the zeroed
/// padding of block-linear images, count for nothing. Exact for LDR blocks: endpoint modes with
/// alpha whose alpha endpoints are all 1 are opaque. HDR alpha counts as alpha, HDR color as
/// neither white nor black, invalid blocks as anything.
[[nodiscard]] AlphaScan ScanAlpha(std::span<const uint8_t> blocks, uint32_t block_width,
                                  uint32_t block_height);

} // namespace Tegra::Texture::ASTC
