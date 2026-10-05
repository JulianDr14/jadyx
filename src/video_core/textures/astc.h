// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Tegra::Texture::ASTC {

void Decompress(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                uint32_t block_width, uint32_t block_height, std::span<uint8_t> output);

// Independent CPU oracle for the isolated D3D12 sampling gate: Khronos LDR decode_unorm8,
// sRGB endpoint expansion when requested, and opaque magenta for invalid/HDR blocks.
// The existing renderer's Decompress rounding and error policy are unchanged.
void DecompressForSamplingProbe(std::span<const uint8_t> data, uint32_t width, uint32_t height,
                                uint32_t depth, uint32_t block_width, uint32_t block_height,
                                std::span<uint8_t> output, bool srgb);

} // namespace Tegra::Texture::ASTC
