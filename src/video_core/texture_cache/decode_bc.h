// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

[[nodiscard]] u32 ConvertedBytesPerBlock(VideoCore::Surface::PixelFormat pixel_format);

void DecompressBCn(std::span<const u8> input, std::span<u8> output, BufferImageCopy& copy,
                   VideoCore::Surface::PixelFormat pixel_format);

/// Whether a decoded BC6H image of this type is held as R11G11B10_FLOAT (4 bytes a texel) instead
/// of R16G16B16A16_FLOAT: unsigned BC6H 3D textures, whose volumes are large (a 512x256x368
/// light volume is 368 MiB as RGBA16F and 184 MiB packed).
[[nodiscard]] bool PacksBc6hAsR11G11B10(VideoCore::Surface::PixelFormat pixel_format,
                                        ImageType type) noexcept;

/// ConvertedBytesPerBlock for an image of this type (PacksBc6hAsR11G11B10 takes 4 bytes).
[[nodiscard]] u32 ConvertedBytesPerBlock(VideoCore::Surface::PixelFormat pixel_format,
                                         ImageType type);

/// DecompressBCn for an image of this type: PacksBc6hAsR11G11B10 images come out packed.
void DecompressBCn(std::span<const u8> input, std::span<u8> output, BufferImageCopy& copy,
                   VideoCore::Surface::PixelFormat pixel_format, ImageType type);

/// BC1-3 colors decoded by DecompressBCn use the guest texture unit's interpolation
/// (GuestBc1Palette) instead of exact thirds. Off by default.
void SetBcGuestPalette(bool enabled) noexcept;
[[nodiscard]] bool GetBcGuestPalette() noexcept;

/// The four RGBA8 colors of a BC1 color block (8 bytes) as the guest's texture unit decodes them:
/// red and blue interpolate the 5-bit endpoints, green the 8-bit ones in 1/256 steps.
/// four_colors: BC2/BC3 blocks, which never use the three-color mode.
[[nodiscard]] std::array<std::array<u8, 4>, 4> GuestBc1Palette(const u8* block,
                                                               bool four_colors) noexcept;

} // namespace VideoCommon
