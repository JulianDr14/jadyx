// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace D3D12::HeapPacking {
struct Placement {
    std::uint64_t offset;
    std::uint64_t waste;
};

/// The smallest fitting range preserves larger holes. Padding remains a reusable free range.
inline std::optional<Placement> Fit(std::uint64_t offset, std::uint64_t bytes,
                                   std::uint64_t size, std::uint64_t alignment) {
    if (!size || !alignment || (alignment & (alignment - 1))) return std::nullopt;
    const auto padding = (alignment - (offset & (alignment - 1))) & (alignment - 1);
    if (padding > bytes || size > bytes - padding ||
        offset > std::numeric_limits<std::uint64_t>::max() - padding - size)
        return std::nullopt;
    return Placement{offset + padding, bytes - size};
}

/// Placement alignment D3D12 grants a small sampled texture instead of 64 KiB.
constexpr std::uint64_t small_alignment = 4096;
/// Element of a texture format: its bytes and the texels it spans (4x4 for BC).
struct TexelBlock {
    std::uint32_t bytes{};
    std::uint32_t width{1};
    std::uint32_t height{1};
};

/// A 2D texture D3D12 places at 4 KiB: sampled only (no render target, depth or MSAA) and a
/// top level covered by at most 16 standard 4 KiB tiles. The tile is 4096 bytes of elements,
/// 64x64 for 1 byte down to 16x16 for 16. Asking for more makes the debug layer report an
/// error, so the shape is checked first. A zero block (unknown format) never qualifies.
inline bool SmallPlacementCandidate(bool texture_2d, bool render_target_or_depth,
                                    std::uint32_t samples, std::uint64_t width,
                                    std::uint32_t height, TexelBlock block) {
    if (!texture_2d || render_target_or_depth || samples != 1 || !width || !height ||
        !block.width || !block.height || block.bytes == 0 || block.bytes > 16 ||
        (block.bytes & (block.bytes - 1))) {
        return false;
    }
    const std::uint32_t log2_bytes = block.bytes >= 8 ? (block.bytes == 16 ? 4 : 3)
                                     : block.bytes >= 4 ? 2
                                                        : block.bytes - 1;
    const std::uint64_t tile_w = 64 >> (log2_bytes / 2);
    const std::uint64_t tile_h = 4096 / block.bytes / tile_w;
    const std::uint64_t blocks_w = (width + block.width - 1) / block.width;
    const std::uint64_t blocks_h = (std::uint64_t{height} + block.height - 1) / block.height;
    const std::uint64_t tiles_w = (blocks_w + tile_w - 1) / tile_w;
    const std::uint64_t tiles_h = (blocks_h + tile_h - 1) / tile_h;
    return tiles_w <= 16 && tiles_h <= 16 && tiles_w * tiles_h <= 16;
}

/// Heaps require alignment, not power-of-two capacity. Zero reports overflow/invalid input.
inline std::uint64_t BlockSize(std::uint64_t size, std::uint64_t alignment,
                               std::uint64_t minimum) {
    alignment = std::max<std::uint64_t>(alignment, 65536);
    if (alignment & (alignment - 1)) return 0;
    size = std::max(size, minimum);
    const auto padding = (alignment - (size & (alignment - 1))) & (alignment - 1);
    if (size > std::numeric_limits<std::uint64_t>::max() - padding) return 0;
    return size + padding;
}
} // namespace D3D12::HeapPacking
