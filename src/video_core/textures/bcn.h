// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Tegra::Texture::BCN {

void CompressBC1(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output);

void CompressBC3(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output);

/// BC4 of the alpha channel of RGBA8 texels (the alpha masks of AstcAlphaMaskFor).
void CompressBC4Alpha(std::span<const u8> data, u32 width, u32 height, u32 depth,
                      std::span<u8> output);

} // namespace Tegra::Texture::BCN
