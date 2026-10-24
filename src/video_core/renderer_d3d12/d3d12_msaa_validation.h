// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace D3D12 {

// Validate before SamplesLog2 (which asserts on invalid counts) and before any
// shifts/clamping can silently truncate the guest's expanded sample grid.
[[nodiscard]] constexpr std::string_view ValidateMsaaShape(
    std::uint32_t samples, bool is_2d, int levels, std::uint32_t depth, std::uint32_t width,
    std::uint32_t height, std::uint32_t block_width, std::uint32_t block_height) {
    if (samples == 0 || samples > 16 || (samples & (samples - 1)) != 0) {
        return "sample count must be 1/2/4/8/16";
    }
    if (samples == 1) {
        return {};
    }
    if (!is_2d || depth != 1 || levels != 1 || block_width != 1 || block_height != 1) {
        return "MSAA requires an uncompressed 2D image with one mip and depth 1";
    }
    const std::uint32_t grid_width = samples >= 8 ? 4 : 2;
    const std::uint32_t grid_height = samples == 16 ? 4 : samples >= 4 ? 2 : 1;
    if (width == 0 || height == 0 || width % grid_width != 0 || height % grid_height != 0) {
        return "image dimensions are not aligned to the MSAA sample grid";
    }
    return {};
}

// Query exactly once for the requested count. Never probe a lower count as a
// fallback: Texture2DMS accesses and the PSO must agree with the guest image.
template <typename Query>
[[nodiscard]] std::string_view ValidateMsaaAllocation(std::uint32_t samples,
                                                      bool attachment_supported, Query&& query) {
    if (samples == 1) {
        return {};
    }
    if (!attachment_supported) {
        return "MSAA format lacks attachment support";
    }
    if (!query(samples)) {
        return "exact MSAA sample count is unsupported or its quality query failed";
    }
    return {};
}

} // namespace D3D12
