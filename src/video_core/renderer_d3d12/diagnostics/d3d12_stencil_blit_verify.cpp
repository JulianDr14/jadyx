// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SetStencilBlitVerify: the first scaled depth-stencil blits read the stencil plane of the source
// and the target back and compare the target with the nearest-texel copy of the source that
// d3d12_blit_stencil_bit.frag draws. Deliberately synchronous; diagnostics only.

#include <algorithm>
#include <cmath>
#include <string>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

namespace D3D12 {

using namespace TextureDetail;

namespace {

/// The source texels a target pixel may read along one axis: the shader takes
/// floor(start + extent * (pixel + 0.5 - first) / size), and float rounding can land a center
/// exactly on a texel edge either way, so both neighbours of an edge count as expected.
std::pair<s32, s32> SourceTexels(s32 pixel, s32 first, s32 size, s32 start, s32 end, s32 limit) {
    const double position = start + static_cast<double>(end - start) *
                                        (pixel + 0.5 - first) / static_cast<double>(size);
    constexpr double EDGE = 1.0 / 1024.0;
    const auto clamp = [limit](double value) {
        return std::clamp(static_cast<s32>(std::floor(value)), 0, limit - 1);
    };
    return {clamp(position - EDGE), clamp(position + EDGE)};
}

/// The stencil plane of a subresource, copied into a readback buffer.
struct StencilReadback {
    StagingBufferRef buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;

    [[nodiscard]] u8 At(s32 x, s32 y) const {
        return buffer.mapped_span[static_cast<size_t>(y) * footprint.Footprint.RowPitch +
                                  static_cast<size_t>(x)];
    }
};

} // Anonymous namespace

void TextureCacheRuntime::VerifyStencilBlit(Image& dst_image, u32 dst_subresource,
                                            Image& src_image, u32 src_subresource,
                                            const Region2D& dst_region,
                                            const Region2D& src_region) {
    auto* const commands = scheduler.CommandList();
    const auto read_back = [&](Image& image, u32 subresource) {
        // Plane 1 follows every subresource of plane 0.
        const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
        const u32 plane_subresource = subresource + desc.MipLevels * desc.DepthOrArraySize;
        StencilReadback result{};
        u64 size = 0;
        device.Get()->GetCopyableFootprints(&desc, plane_subresource, 1, 0, &result.footprint,
                                             nullptr, nullptr, &size);
        result.buffer = DownloadStagingBuffer(size, true);
        result.footprint.Offset = result.buffer.offset;
        image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = image.Handle(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = plane_subresource};
        const D3D12_TEXTURE_COPY_LOCATION destination{
            .pResource = result.buffer.buffer,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = result.footprint};
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        // The span starts at the buffer's offset; rows are read from its start.
        result.footprint.Offset = 0;
        return result;
    };
    const StencilReadback src = read_back(src_image, src_subresource);
    const StencilReadback dst = read_back(dst_image, dst_subresource);
    scheduler.Finish();

    const s32 left = std::min(dst_region.start.x, dst_region.end.x);
    const s32 top = std::min(dst_region.start.y, dst_region.end.y);
    const s32 right = std::min<s32>(std::max(dst_region.start.x, dst_region.end.x),
                                    static_cast<s32>(dst.footprint.Footprint.Width));
    const s32 bottom = std::min<s32>(std::max(dst_region.start.y, dst_region.end.y),
                                     static_cast<s32>(dst.footprint.Footprint.Height));
    const s32 width = std::max(dst_region.start.x, dst_region.end.x) - left;
    const s32 height = std::max(dst_region.start.y, dst_region.end.y) - top;
    const s32 src_width = static_cast<s32>(src.footprint.Footprint.Width);
    const s32 src_height = static_cast<s32>(src.footprint.Footprint.Height);
    u64 checked = 0;
    u64 mismatches = 0;
    u64 nonzero = 0;
    u32 bits = 0;
    std::string first_mismatch;
    for (s32 y = std::max(top, 0); y < bottom; ++y) {
        const auto [sy0, sy1] =
            SourceTexels(y, top, height, src_region.start.y, src_region.end.y, src_height);
        for (s32 x = std::max(left, 0); x < right; ++x) {
            const auto [sx0, sx1] =
                SourceTexels(x, left, width, src_region.start.x, src_region.end.x, src_width);
            const u8 got = dst.At(x, y);
            ++checked;
            nonzero += got != 0;
            bits |= got;
            if (got == src.At(sx0, sy0) || got == src.At(sx1, sy0) || got == src.At(sx0, sy1) ||
                got == src.At(sx1, sy1)) {
                continue;
            }
            if (mismatches++ == 0) {
                first_mismatch = fmt::format(", first at ({}, {}): {:#04x}, source ({}, {}) {:#04x}",
                                             x, y, got, sx0, sy0, src.At(sx0, sy0));
            }
        }
    }
    const std::string summary = fmt::format(
        "D3D12: stencil blit check {}x{} from ({}, {})-({}, {}) of {}x{}: {} of {} texels differ, "
        "{} nonzero, bits {:#04x}{}",
        width, height, src_region.start.x, src_region.start.y, src_region.end.x, src_region.end.y,
        src_width, src_height, mismatches, checked, nonzero, bits, first_mismatch);
    if (mismatches == 0) {
        LOG_INFO(Render, "{}", summary);
    } else {
        LOG_ERROR(Render, "{}", summary);
    }
}

} // namespace D3D12
