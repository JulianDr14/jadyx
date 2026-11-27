// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace VideoCommon {

/// Size of an image request in texels per sample, as the host will create it.
struct ImageExtentRequest {
    bool is_3d = false;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;  ///< Only read for 3D images.
    std::uint32_t layers = 1; ///< Only read for 1D and 2D images.
    std::uint32_t levels = 1;
};

/// The largest images either side can hold. The guest's own limits (NVN reports 16384 for 1D/2D
/// and 2048 for 3D and array layers) match D3D12 feature level 11's, so nothing a game may
/// legally ask for is lost: anything past them is a texture header read from garbage, which the
/// host could only fail to allocate.
struct ImageLimits {
    std::uint32_t max_2d = 16384;    ///< D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION (and 1D).
    std::uint32_t max_3d = 2048;     ///< D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION.
    std::uint32_t max_layers = 2048; ///< D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION.
    std::uint32_t max_levels = 15;   ///< log2(16384) + 1.
};

/// False when a dimension, the layer count or the level count is past the limits. Zero sizes pass:
/// the backends already raise them to one, and disabled render targets use them.
[[nodiscard]] constexpr bool FitsImageLimits(const ImageExtentRequest& request,
                                             const ImageLimits& limits = {}) {
    const std::uint32_t max_dim = request.is_3d ? limits.max_3d : limits.max_2d;
    if (request.width > max_dim || request.height > max_dim || request.levels > limits.max_levels) {
        return false;
    }
    return request.is_3d ? request.depth <= limits.max_3d : request.layers <= limits.max_layers;
}

} // namespace VideoCommon
