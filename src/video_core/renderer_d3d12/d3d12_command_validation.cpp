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
namespace TextureDetail {
DXGI_FORMAT TypelessFamily(DXGI_FORMAT format);
}
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

DXGI_FORMAT LocationFormat(const D3D12_TEXTURE_COPY_LOCATION& location,
                           const D3D12_RESOURCE_DESC& desc) {
    return location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
               ? location.PlacedFootprint.Footprint.Format
               : desc.Format;
}

/// Depth-stencil and multisampled subresources are only copied whole.
bool WholeOnly(const D3D12_TEXTURE_COPY_LOCATION& location, const D3D12_RESOURCE_DESC& desc) {
    return location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
           ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
            desc.SampleDesc.Count > 1);
}

std::string Describe(const D3D12_TEXTURE_COPY_LOCATION& location,
                     const D3D12_RESOURCE_DESC& desc) {
    if (location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        const D3D12_SUBRESOURCE_FOOTPRINT& footprint = location.PlacedFootprint.Footprint;
        return fmt::format("footprint format {} {}x{}x{} pitch {}",
                           static_cast<u32>(footprint.Format), footprint.Width, footprint.Height,
                           footprint.Depth, footprint.RowPitch);
    }
    return fmt::format("subresource {} of format {} dim {} {}x{}x{} levels {} samples {} "
                       "flags {:#x}",
                       location.SubresourceIndex, static_cast<u32>(desc.Format),
                       static_cast<u32>(desc.Dimension), desc.Width, desc.Height,
                       desc.DepthOrArraySize, desc.MipLevels, desc.SampleDesc.Count,
                       static_cast<u32>(desc.Flags));
}

void Report(const std::string& what) {
    static std::atomic<u32> reported{};
    BUG_TRACK(CopySkipped, "invalid command dropped: {}", what);
    if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_ERROR(Render, "D3D12: dropped an invalid {}", what);
    }
}

/// At most one write state, and then no read state (DEPTH_WRITE with DEPTH_READ included).
bool ValidStates(D3D12_RESOURCE_STATES states) {
    constexpr u32 write_states =
        D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
        D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
        D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
    const u32 bits = static_cast<u32>(states);
    const u32 writes = bits & write_states;
    return writes == 0 || (writes == bits && (writes & (writes - 1)) == 0);
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
    const D3D12_RESOURCE_DESC dst_desc = dst.pResource->GetDesc();
    const D3D12_RESOURCE_DESC src_desc = src.pResource->GetDesc();
    const DXGI_FORMAT dst_format = LocationFormat(dst, dst_desc);
    const DXGI_FORMAT src_format = LocationFormat(src, src_desc);
    const bool dst_blocks = BlockCompressed(dst_format);
    const bool src_blocks = BlockCompressed(src_format);
    const auto fail = [&](const std::string& reason) {
        Report(fmt::format("CopyTextureRegion: {} (to {} at {},{},{} from {})", reason,
                           Describe(dst, dst_desc), x, y, z, Describe(src, src_desc)));
        return false;
    };
    if (dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst_desc.SampleDesc.Count != src_desc.SampleDesc.Count) {
        return fail("sample counts differ");
    }
    // Needs TextureCopyBetweenDimensionsSupported (options 13), which the Series runtime lacks.
    if (dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst_desc.Dimension != src_desc.Dimension) {
        return fail("copy between texture dimensions");
    }
    // Between textures only within a format family, or between a block-compressed format and
    // the integer one of its block size: R11G11B10 into R32 fails the list (0.2.93.24 probe).
    if (dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && dst_blocks == src_blocks &&
        TextureDetail::TypelessFamily(dst_format) != TextureDetail::TypelessFamily(src_format)) {
        return fail("copy between format families");
    }
    // Block-compressed copies move whole 4x4 blocks, also on the 2x2 and 1x1 levels whose
    // single block the extent already rounds up to: a box or footprint trimmed to the level's
    // texels fails the list just like a misaligned one.
    if (src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && src_blocks &&
        (src_extent.width % 4 != 0 || src_extent.height % 4 != 0)) {
        return fail("block-compressed footprint not in whole blocks");
    }
    if (dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && dst_blocks &&
        (dst_extent.width % 4 != 0 || dst_extent.height % 4 != 0)) {
        return fail("block-compressed footprint not in whole blocks");
    }
    if (dst_blocks && (x % 4 != 0 || y % 4 != 0)) {
        return fail("block-compressed destination offset not on a block");
    }
    u64 width = src_extent.width;
    u32 height = src_extent.height;
    u32 depth = src_extent.depth;
    if (box) {
        if (box->right > src_extent.width || box->bottom > src_extent.height ||
            box->back > src_extent.depth) {
            return fail(fmt::format("source box {},{},{}-{},{},{} past {}x{}x{}", box->left,
                                    box->top, box->front, box->right, box->bottom, box->back,
                                    src_extent.width, src_extent.height, src_extent.depth));
        }
        if (box->left >= box->right || box->top >= box->bottom || box->front >= box->back) {
            return true; // empty: the runtime does nothing
        }
        if (src_blocks && (box->left % 4 != 0 || box->top % 4 != 0 || box->right % 4 != 0 ||
                           box->bottom % 4 != 0)) {
            return fail(fmt::format("block-compressed source box {},{}-{},{} not on blocks",
                                    box->left, box->top, box->right, box->bottom));
        }
        width = box->right - box->left;
        height = box->bottom - box->top;
        depth = box->back - box->front;
    }
    // Between a block-compressed format and the integer one of its block size, one texel of the
    // integer side is one block of the other.
    if (src_blocks && !dst_blocks) {
        width /= 4;
        height /= 4;
    } else if (!src_blocks && dst_blocks) {
        width *= 4;
        height *= 4;
    }
    if (x + width > dst_extent.width || y + height > dst_extent.height ||
        z + depth > dst_extent.depth) {
        return fail(fmt::format("{}x{}x{} past a destination of {}x{}x{}", width, height, depth,
                                dst_extent.width, dst_extent.height, dst_extent.depth));
    }
    const bool whole_src = !box || (box->left == 0 && box->top == 0 && box->front == 0 &&
                                    box->right == src_extent.width &&
                                    box->bottom == src_extent.height &&
                                    box->back == src_extent.depth);
    const bool whole_dst = x == 0 && y == 0 && z == 0 && width == dst_extent.width &&
                           height == dst_extent.height && depth == dst_extent.depth;
    if ((WholeOnly(dst, dst_desc) || WholeOnly(src, src_desc)) && !(whole_src && whole_dst)) {
        return fail(fmt::format("partial {}x{}x{} copy of a depth-stencil or multisampled "
                                "subresource",
                                width, height, depth));
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
    if (!ValidStates(transition.StateBefore) || !ValidStates(transition.StateAfter) ||
        transition.StateBefore == transition.StateAfter) {
        const D3D12_RESOURCE_DESC desc = transition.pResource->GetDesc();
        Report(fmt::format("transition: states {:#x} -> {:#x} (dim {} {}x{}x{} format {})",
                           static_cast<u32>(transition.StateBefore),
                           static_cast<u32>(transition.StateAfter),
                           static_cast<u32>(desc.Dimension), desc.Width, desc.Height,
                           desc.DepthOrArraySize, static_cast<u32>(desc.Format)));
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

bool ValidCopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
    if (!dst || !src || dst == src) {
        Report("CopyResource: null or identical resources");
        return false;
    }
    const D3D12_RESOURCE_DESC d = dst->GetDesc();
    const D3D12_RESOURCE_DESC s = src->GetDesc();
    if (d.Dimension != s.Dimension || d.Width != s.Width || d.Height != s.Height ||
        d.DepthOrArraySize != s.DepthOrArraySize || d.MipLevels != s.MipLevels ||
        d.SampleDesc.Count != s.SampleDesc.Count) {
        Report(fmt::format("CopyResource: dim {} {}x{}x{} levels {} samples {} format {} from "
                           "dim {} {}x{}x{} levels {} samples {} format {}",
                           static_cast<u32>(d.Dimension), d.Width, d.Height, d.DepthOrArraySize,
                           d.MipLevels, d.SampleDesc.Count, static_cast<u32>(d.Format),
                           static_cast<u32>(s.Dimension), s.Width, s.Height, s.DepthOrArraySize,
                           s.MipLevels, s.SampleDesc.Count, static_cast<u32>(s.Format)));
        return false;
    }
    return true;
}

bool ValidClearRects(const char* call, D3D12_CPU_DESCRIPTOR_HANDLE target, UINT rect_count,
                     const D3D12_RECT* rects) {
    if (target.ptr == 0) {
        Report(fmt::format("{}: null descriptor", call));
        return false;
    }
    if (rect_count != 0 && !rects) {
        Report(fmt::format("{}: {} rects without an array", call, rect_count));
        return false;
    }
    for (UINT i = 0; i < rect_count; ++i) {
        const D3D12_RECT& rect = rects[i];
        if (rect.left < 0 || rect.top < 0 || rect.left > rect.right || rect.top > rect.bottom) {
            Report(fmt::format("{}: rect {},{}-{},{}", call, rect.left, rect.top, rect.right,
                               rect.bottom));
            return false;
        }
    }
    return true;
}

bool ValidRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets) {
    if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT || (count != 0 && !targets)) {
        Report(fmt::format("OMSetRenderTargets: {} targets{}", count,
                           targets ? "" : " without an array"));
        return false;
    }
    return true;
}

bool ValidPredication(ID3D12Resource* buffer, UINT64 offset) {
    if (!buffer) {
        return true;
    }
    const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset % 8 != 0 ||
        offset + 8 > desc.Width) {
        Report(fmt::format("SetPredication: offset {} in a resource of dim {} and {} bytes",
                           offset, static_cast<u32>(desc.Dimension), desc.Width));
        return false;
    }
    return true;
}

bool ValidExecuteIndirect(ID3D12CommandSignature* signature, ID3D12Resource* arguments,
                          UINT64 arguments_offset, ID3D12Resource* count, UINT64 count_offset) {
    if (!signature || !arguments) {
        Report("ExecuteIndirect: null signature or argument buffer");
        return false;
    }
    const u64 arguments_size = arguments->GetDesc().Width;
    if (arguments_offset % 4 != 0 || arguments_offset >= arguments_size) {
        Report(fmt::format("ExecuteIndirect: arguments at {} of {} bytes", arguments_offset,
                           arguments_size));
        return false;
    }
    if (count) {
        const u64 count_size = count->GetDesc().Width;
        if (count_offset % 4 != 0 || count_offset + 4 > count_size) {
            Report(fmt::format("ExecuteIndirect: count at {} of {} bytes", count_offset,
                               count_size));
            return false;
        }
    }
    return true;
}

bool ValidQuery(const char* call, ID3D12QueryHeap* heap) {
    if (!heap) {
        Report(fmt::format("{}: null query heap", call));
        return false;
    }
    return true;
}

bool ValidResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT count,
                           ID3D12Resource* dst, UINT64 dst_offset) {
    if (!heap || !dst) {
        Report("ResolveQueryData: null heap or destination");
        return false;
    }
    u64 result_size = 8;
    if (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS) {
        result_size = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    } else if (type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 &&
               type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
        result_size = sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    }
    const D3D12_RESOURCE_DESC desc = dst->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || dst_offset % 8 != 0 ||
        dst_offset + result_size * count > desc.Width) {
        Report(fmt::format("ResolveQueryData: {} results of type {} at {} of {} bytes", count,
                           static_cast<u32>(type), dst_offset, desc.Width));
        return false;
    }
    return true;
}

} // namespace D3D12
