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
