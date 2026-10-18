// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <string>

#include <fmt/format.h>

#include "common/bug_tracker.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_command_validation.h"

namespace D3D12 {
namespace {

bool BlockCompressed(DXGI_FORMAT format) {
    return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
           (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

u32 PlaneCount(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        return 2;
    default:
        return 1;
    }
}

u32 SubresourceCount(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return 1;
    }
    const u32 array = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                          ? 1U
                          : static_cast<u32>(desc.DepthOrArraySize);
    return static_cast<u32>(desc.MipLevels) * array * PlaneCount(desc.Format);
}

struct Extent {
    u64 width;
    u32 height;
    u32 depth;
};

/// Size of one subresource, with block-compressed levels rounded up to whole blocks as the
/// runtime accepts them.
Extent SubresourceExtent(const D3D12_RESOURCE_DESC& desc, u32 subresource) {
    const u32 level = subresource % desc.MipLevels;
    Extent extent{
        .width = std::max<u64>(1, desc.Width >> level),
        .height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      ? 1U
                      : std::max(1U, desc.Height >> level),
        .depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                     ? std::max(1U, static_cast<u32>(desc.DepthOrArraySize) >> level)
                     : 1U,
    };
    if (BlockCompressed(desc.Format)) {
        extent.width = (extent.width + 3) & ~u64{3};
        extent.height = (extent.height + 3) & ~3U;
    }
    return extent;
}

/// The extent a copy location offers, or a reason it offers none.
bool LocationExtent(const D3D12_TEXTURE_COPY_LOCATION& location, Extent& extent,
                    std::string& reason) {
    if (!location.pResource) {
        reason = "null resource";
        return false;
    }
    const D3D12_RESOURCE_DESC desc = location.pResource->GetDesc();
    if (location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            reason = "subresource index on a buffer";
            return false;
        }
        const u32 count = SubresourceCount(desc);
        if (location.SubresourceIndex >= count) {
            reason = fmt::format("subresource {} of {} (dim {} {}x{}x{} levels {} format {})",
                                 location.SubresourceIndex, count,
                                 static_cast<u32>(desc.Dimension), desc.Width, desc.Height,
                                 desc.DepthOrArraySize, desc.MipLevels,
                                 static_cast<u32>(desc.Format));
            return false;
        }
        extent = SubresourceExtent(desc, location.SubresourceIndex);
        return true;
    }
    const D3D12_SUBRESOURCE_FOOTPRINT& footprint = location.PlacedFootprint.Footprint;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        reason = "placed footprint on a texture";
        return false;
    }
    if (location.PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0 ||
        footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0 || footprint.Width == 0 ||
        footprint.Height == 0 || footprint.Depth == 0) {
        reason = fmt::format("footprint offset {} pitch {} size {}x{}x{}",
                             location.PlacedFootprint.Offset, footprint.RowPitch, footprint.Width,
                             footprint.Height, footprint.Depth);
        return false;
    }
    const u32 rows = BlockCompressed(footprint.Format) ? (footprint.Height + 3) / 4
                                                       : footprint.Height;
    // The last row may be shorter than the pitch; every earlier row is a full pitch.
    const u64 least = location.PlacedFootprint.Offset +
                      (static_cast<u64>(rows) * footprint.Depth - 1) * footprint.RowPitch + 1;
    if (least > desc.Width) {
        reason = fmt::format("footprint offset {} pitch {} rows {}x{} past a buffer of {} bytes",
                             location.PlacedFootprint.Offset, footprint.RowPitch, rows,
                             footprint.Depth, desc.Width);
        return false;
    }
    extent = {footprint.Width, footprint.Height, footprint.Depth};
    return true;
}

void Report(const std::string& what) {
    static std::atomic<u32> reported{};
    BUG_TRACK(CopySkipped, "invalid command dropped: {}", what);
    if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_ERROR(Render, "D3D12: dropped an invalid {}", what);
    }
}

} // Anonymous namespace

bool ValidCopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y, UINT z,
                            const D3D12_TEXTURE_COPY_LOCATION& src, const D3D12_BOX* box) {
    std::string reason;
    Extent dst_extent{};
    Extent src_extent{};
    if (!LocationExtent(dst, dst_extent, reason)) {
        Report(fmt::format("CopyTextureRegion: destination {}", reason));
        return false;
    }
    if (!LocationExtent(src, src_extent, reason)) {
        Report(fmt::format("CopyTextureRegion: source {}", reason));
        return false;
    }
    u64 width = src_extent.width;
    u32 height = src_extent.height;
    u32 depth = src_extent.depth;
    if (box) {
        if (box->right > src_extent.width || box->bottom > src_extent.height ||
            box->back > src_extent.depth) {
            Report(fmt::format("CopyTextureRegion: source box {},{},{}-{},{},{} past {}x{}x{}",
                               box->left, box->top, box->front, box->right, box->bottom,
                               box->back, src_extent.width, src_extent.height,
                               src_extent.depth));
            return false;
        }
        if (box->left >= box->right || box->top >= box->bottom || box->front >= box->back) {
            return true; // empty: the runtime does nothing
        }
        width = box->right - box->left;
        height = box->bottom - box->top;
        depth = box->back - box->front;
    }
    if (x + width > dst_extent.width || y + height > dst_extent.height ||
        z + depth > dst_extent.depth) {
        Report(fmt::format("CopyTextureRegion: {}x{}x{} at {},{},{} past a destination of "
                           "{}x{}x{}",
                           width, height, depth, x, y, z, dst_extent.width, dst_extent.height,
                           dst_extent.depth));
        return false;
    }
    return true;
}

bool ValidCopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                           UINT64 src_offset, UINT64 bytes) {
    if (!dst || !src) {
        Report("CopyBufferRegion: null resource");
        return false;
    }
    const u64 dst_size = dst->GetDesc().Width;
    const u64 src_size = src->GetDesc().Width;
    if (dst_offset > dst_size || bytes > dst_size - dst_offset || src_offset > src_size ||
        bytes > src_size - src_offset) {
        Report(fmt::format("CopyBufferRegion: {} bytes from {} of {} to {} of {}", bytes,
                           src_offset, src_size, dst_offset, dst_size));
        return false;
    }
    return true;
}

bool ValidBarrier(const D3D12_RESOURCE_BARRIER& barrier) {
    if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        return true;
    }
    const D3D12_RESOURCE_TRANSITION_BARRIER& transition = barrier.Transition;
    if (!transition.pResource) {
        Report("transition: null resource");
        return false;
    }
    if (transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        return true;
    }
    const D3D12_RESOURCE_DESC desc = transition.pResource->GetDesc();
    const u32 count = SubresourceCount(desc);
    if (transition.Subresource >= count) {
        Report(fmt::format("transition: subresource {} of {} (dim {} {}x{}x{} levels {} format {})",
                           transition.Subresource, count, static_cast<u32>(desc.Dimension),
                           desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels,
                           static_cast<u32>(desc.Format)));
        return false;
    }
    return true;
}

} // namespace D3D12
