// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace VideoCommon {

/// A 2D blit rectangle in texels: [x0, x1) x [y0, y1). x1 < x0 or y1 < y0 is a flip.
struct BlitRect {
    std::int32_t x0;
    std::int32_t y0;
    std::int32_t x1;
    std::int32_t y1;
};

/// True when the blit copies texel for texel: same size, no flip, no scaling, and the source
/// rectangle lies inside its `src_width` x `src_height` level.
[[nodiscard]] constexpr bool IsUnscaledBlit(const BlitRect& src, const BlitRect& dst,
                                            std::uint32_t src_width, std::uint32_t src_height) {
    const std::int64_t width = std::int64_t{src.x1} - src.x0;
    const std::int64_t height = std::int64_t{src.y1} - src.y0;
    return width > 0 && height > 0 && std::int64_t{dst.x1} - dst.x0 == width &&
           std::int64_t{dst.y1} - dst.y0 == height && src.x0 >= 0 && src.y0 >= 0 &&
           src.x1 <= std::int64_t{src_width} && src.y1 <= std::int64_t{src_height};
}

/// True when the rectangle is exactly a `width` x `height` target, so the blit writes every texel
/// and whatever the target held before never matters. Rectangles reaching past the target are
/// not accepted: the copy clips them, which is not worth reasoning about here.
[[nodiscard]] constexpr bool CoversWholeTarget(const BlitRect& dst, std::uint32_t width,
                                               std::uint32_t height) {
    return dst.x0 == 0 && dst.y0 == 0 && std::int64_t{dst.x1} == std::int64_t{width} &&
           std::int64_t{dst.y1} == std::int64_t{height};
}

/// True when a blit drawn as the D3D12 backend draws it rewrites the whole `dst_width` x
/// `dst_height` target. Same signed extents are a copy, which writes every texel only when the
/// source lies inside its level; any other extent (scaled or flipped) is a draw over the
/// destination rectangle, whose sampler clamps a source reaching past its level.
[[nodiscard]] constexpr bool BlitRewritesTarget(const BlitRect& src, const BlitRect& dst,
                                                std::uint32_t src_width, std::uint32_t src_height,
                                                std::uint32_t dst_width,
                                                std::uint32_t dst_height) {
    if (!CoversWholeTarget(dst, dst_width, dst_height) || dst_width == 0 || dst_height == 0 ||
        src.x1 == src.x0 || src.y1 == src.y0) {
        return false;
    }
    const bool copy = std::int64_t{src.x1} - src.x0 == std::int64_t{dst.x1} - dst.x0 &&
                      std::int64_t{src.y1} - src.y0 == std::int64_t{dst.y1} - dst.y0;
    return !copy || IsUnscaledBlit(src, dst, src_width, src_height);
}

} // namespace VideoCommon
