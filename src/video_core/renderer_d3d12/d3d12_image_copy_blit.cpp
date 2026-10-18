// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_bc_copy.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"

// Image to image copies and blits.

namespace D3D12 {

using namespace TextureDetail;

namespace {
/// CopyTextureRegion copies a depth-stencil or multisampled subresource only whole: offsets 0
/// and no source box (its documented remarks). Anything else invalidates the command list.
bool CopiedWhole(const Image& image) {
    const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
    return (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
           desc.SampleDesc.Count > 1;
}

/// Whether copy covers its whole source and destination subresources.
bool CoversSubresources(const Image& dst, const Image& src, const ImageCopy& copy) {
    const auto covers = [&copy](const Image& image, s32 level,
                                 const VideoCommon::Offset3D& offset) {
        const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
        return offset.x == 0 && offset.y == 0 && offset.z == 0 &&
               copy.extent.width == std::max<u64>(1, desc.Width >> level) &&
               copy.extent.height == std::max(1U, desc.Height >> level);
    };
    return covers(src, copy.src_subresource.base_level, copy.src_offset) &&
           covers(dst, copy.dst_subresource.base_level, copy.dst_offset);
}
} // namespace

void TextureCacheRuntime::CopyImage(Image& dst, Image& src, std::span<const ImageCopy> copies) {
    if (std::ranges::any_of(copies, [](const ImageCopy& copy) { return copy.extent_in_blocks; })) {
        // Normalize only alias copies. Guessing units from alignment would corrupt ordinary
        // partial texel copies and silently under-copy aligned alias extents.
        boost::container::small_vector<ImageCopy, 16> normalized(copies.begin(), copies.end());
        for (auto& copy : normalized) {
            if (!copy.extent_in_blocks) continue;
            const s32 level = copy.src_subresource.base_level;
            if (level < 0 || level >= src.info.resources.levels) {
                WarnOnce(logged_decoded_copy, "alias copy has invalid source mip; skipped");
                return;
            }
            const auto width = AliasBlockExtent(copy.extent.width, copy.src_offset.x,
                std::max(1U, src.info.size.width >> level),
                VideoCore::Surface::DefaultBlockWidth(src.info.format));
            const auto height = AliasBlockExtent(copy.extent.height, copy.src_offset.y,
                std::max(1U, src.info.size.height >> level),
                VideoCore::Surface::DefaultBlockHeight(src.info.format));
            if (!width || !height) {
                WarnOnce(logged_decoded_copy, "alias copy exceeds source blocks; skipped");
                return;
            }
            copy.extent.width = *width;
            copy.extent.height = *height;
            copy.extent_in_blocks = false;
        }
        CopyImage(dst, src, std::span<const ImageCopy>{normalized.data(), normalized.size()});
        return;
    }
    if (&dst == &src) {
        // Whole-resource state tracking cannot hold COPY_SOURCE and COPY_DEST at once.
        WarnOnce(logged_self_copy, "copies within one image are not supported yet; skipped");
        return;
    }
    if (!src.Handle() || !dst.Handle()) {
        return;
    }
    if (!Image::AreCopyCompatible(src, dst)) {
        if (CopyDecodedBlocks(dst, src, copies)) {
            return;
        }
        if (dst.IsBcDecoded() && CopyBlocksIntoDecoded(dst, src, copies)) {
            return;
        }
        // Plain texels on one side, compressed blocks on the other: not copyable.
        WarnOnce(logged_decoded_copy, "copy between a host-decoded and a compressed image ({} -> "
                 "{}) skipped", src.info.format, dst.info.format);
        return;
    }
    // Keep exact blocks coherent for copies between decoded arrays. Capture validity before
    // transitioning dst to a writable state (which invalidates its previous version).
    Image* const src_blocks = src.GuestBlocks();
    Image* const dst_blocks = src_blocks && src.info.format == dst.info.format
                                  ? dst.EnsureGuestBlocks() : nullptr;
    if (src.IsDepthStencilPlanar() || dst.IsDepthStencilPlanar()) {
        // Plane by plane, and only between the same host layout (D24S8 or D32S8): anything else
        // reinterprets depth bits and needs a conversion shader.
        if (src.ResourceFormat() != dst.ResourceFormat() || src.info.num_samples > 1 ||
            dst.info.num_samples > 1) {
            WarnOnce(logged_depth_stencil_copy, "depth-stencil copy {} -> {} needs a conversion; "
                     "skipped", src.info.format, dst.info.format);
            return;
        }
        dst.CopyDepthStencilFrom(src, copies);
        return;
    }
    src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    // In the resource's own blocks: a decoded array copies texel by texel.
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(src.TransferFormat().copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(src.TransferFormat().copy_format);
    const u32 dst_block_w = VideoCore::Surface::DefaultBlockWidth(dst.TransferFormat().copy_format);
    if (src.ResourceFormat() != dst.ResourceFormat() && block_w == 1 && dst_block_w == 1) {
        // Guest reinterpretations (RGBA8 texels read as R11G11B10, R32 as RGBA8...) cross DXGI
        // format families, which CopyTextureRegion rejects (the debug layer invalidates the list;
        // without it the result is undefined). The bytes go through a buffer instead.
        CopyThroughBuffer(dst, src, copies);
        return;
    }
    if (src.Handle()->GetDesc().Dimension != dst.Handle()->GetDesc().Dimension) {
        // Layers of a 2D image to slices of a 3D one, or back: copying between dimensions needs
        // TextureCopyBetweenDimensionsSupported, which the Series runtime lacks (Close fails
        // with E_INVALIDARG). Through a buffer, a slice at a time.
        CopyThroughBuffer(dst, src, copies);
        return;
    }
    const bool whole_only = CopiedWhole(src) || CopiedWhole(dst);
    for (const auto& copy : copies) {
        if (whole_only && !CoversSubresources(dst, src, copy)) {
            // Part of a depth or multisampled subresource: picked out through a buffer.
            CopyThroughBuffer(dst, src, std::span{&copy, 1});
            continue;
        }
        const u32 depth = src.info.type == ImageType::e3D ? copy.extent.depth : 1U;
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            D3D12_TEXTURE_COPY_LOCATION source{.pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src.Subresource(copy.src_subresource.base_level, copy.src_subresource.base_layer + layer)};
            D3D12_TEXTURE_COPY_LOCATION target{.pResource = dst.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst.Subresource(copy.dst_subresource.base_level, copy.dst_subresource.base_layer + layer)};
            // Boxes on block-compressed images must cover whole blocks.
            const D3D12_BOX box{static_cast<u32>(copy.src_offset.x), static_cast<u32>(copy.src_offset.y), static_cast<u32>(copy.src_offset.z),
                static_cast<u32>(copy.src_offset.x) + Common::AlignUp(copy.extent.width, block_w),
                static_cast<u32>(copy.src_offset.y) + Common::AlignUp(copy.extent.height, block_h),
                static_cast<u32>(copy.src_offset.z) + depth};
            scheduler.CommandList()->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                                       copy.dst_offset.z, &source, &box);
        }
    }
    const auto whole_blocks = [](const Image& image, const ImageCopy& copy, bool source) {
        const auto& sub = source ? copy.src_subresource : copy.dst_subresource;
        const auto& offset = source ? copy.src_offset : copy.dst_offset;
        const u32 width = std::max(1U, image.info.size.width >> sub.base_level);
        const u32 height = std::max(1U, image.info.size.height >> sub.base_level);
        return PlanBcCopyAxis(offset.x, copy.extent.width, width, 0, UINT32_MAX) &&
               PlanBcCopyAxis(offset.y, copy.extent.height, height, 0, UINT32_MAX);
    };
    if (src_blocks && dst_blocks && src.info.format == dst.info.format &&
        std::ranges::all_of(copies, [&](const ImageCopy& copy) {
            return whole_blocks(src, copy, true) && whole_blocks(dst, copy, false) &&
                   src.HasGuestBlocks(copy);
        })) {
        CopyImage(*dst_blocks, *src_blocks, copies);
        for (const auto& copy : copies) dst.IncludeGuestBlocks(copy);
        dst.guest_blocks_version = dst.write_version;
    }
}

bool TextureCacheRuntime::CopyDecodedBlocks(Image& dst, Image& src,
                                            std::span<const ImageCopy> copies) {
    const auto reject = [&](const char* reason, const ImageCopy* copy = nullptr) {
        // Bound diagnostic volume independently of WarnOnce: retain the failing predicate
        // and representation version without logging every draw of an unsupported transfer.
        static u32 reports = 0;
        if (reports < 32) {
            ++reports;
            LOG_WARNING(Render,
                        "D3D12: BC block copy rejected: {}; {} @{:x} -> {} @{:x}, "
                        "shadow={} pending={} ahead={} version={}/{}, batch={}",
                        reason, src.info.format, src.gpu_addr, dst.info.format, dst.gpu_addr,
                        bool(src.guest_blocks), src.guest_blocks_upload_pending,
                        src.reinterpreted_ahead, src.guest_blocks_version, src.write_version,
                        copies.size());
            if (copy) {
                LOG_WARNING(Render,
                            "D3D12: rejected BC region: src mip={} layer={} count={} "
                            "offset={},{},{}; dst mip={} layer={} count={} offset={},{},{}; "
                            "extent={}x{}x{}; src base={}x{} dst base={}x{}",
                            copy->src_subresource.base_level, copy->src_subresource.base_layer,
                            copy->src_subresource.num_layers, copy->src_offset.x,
                            copy->src_offset.y, copy->src_offset.z,
                            copy->dst_subresource.base_level, copy->dst_subresource.base_layer,
                            copy->dst_subresource.num_layers, copy->dst_offset.x,
                            copy->dst_offset.y, copy->dst_offset.z, copy->extent.width,
                            copy->extent.height, copy->extent.depth,
                            src.info.size.width, src.info.size.height,
                            dst.info.size.width, dst.info.size.height);
            }
        }
        return false;
    };
    Image* const blocks = src.GuestBlocks();
    // D3D12's documented structured/BC reinterpretation pairs. Do not interpret decoded
    // colors as blocks, or route arbitrary same-sized DXGI formats through CopyTextureRegion.
    if (!blocks || dst.TransferFormat().converted || src.info.type != ImageType::e2D ||
        dst.info.type != ImageType::e2D || dst.info.num_samples != 1) {
        // Other incompatible copy paths also enter here. Diagnose only decoded BC sources.
        return src.IsBcDecoded() ? reject(!src.guest_blocks ? "missing shadow" :
            src.guest_blocks_upload_pending ? "pending upload" :
            src.reinterpreted_ahead ? "reinterpretation ahead" :
            src.guest_blocks_version != src.write_version ? "stale shadow" :
            "image type, samples or converted destination") : false;
    }
    const auto target_format = dst.ViewFormat();
    const u32 bytes = VideoCore::Surface::BytesPerBlock(src.info.format);
    if (!((bytes == 16 && (target_format == DXGI_FORMAT_R32G32B32A32_UINT ||
                           target_format == DXGI_FORMAT_R32G32B32A32_SINT)) ||
          (bytes == 8 && (target_format == DXGI_FORMAT_R32G32_UINT ||
                          target_format == DXGI_FORMAT_R32G32_SINT)))) {
        return reject("unsupported integer format pair");
    }
    const auto source_desc = blocks->Handle()->GetDesc();
    const auto target_desc = dst.Handle()->GetDesc();
    // Validate the entire batch before recording any copy. Failed validation leaves the existing
    // explicit CopySkipped diagnostic, and never submits a partially executed batch as success.
    for (const auto& copy : copies) {
        const auto& s = copy.src_subresource;
        const auto& d = copy.dst_subresource;
        if (s.base_level < 0 || s.base_level >= source_desc.MipLevels || d.base_level < 0 ||
            d.base_level >= target_desc.MipLevels || s.base_layer < 0 || d.base_layer < 0 ||
            s.num_layers <= 0 || s.num_layers != d.num_layers ||
            s.num_layers > source_desc.DepthOrArraySize - s.base_layer ||
            d.num_layers > target_desc.DepthOrArraySize - d.base_layer || copy.src_offset.z != 0 ||
            copy.dst_offset.z != 0 || copy.extent.depth != 1) {
            return reject("subresource or depth bounds", &copy);
        }
        for (u32 axis = 0; axis < 2; ++axis) {
            const s32 source_offset = axis == 0 ? copy.src_offset.x : copy.src_offset.y;
            const s32 target_offset = axis == 0 ? copy.dst_offset.x : copy.dst_offset.y;
            const u32 extent = axis == 0 ? copy.extent.width : copy.extent.height;
            const u32 source_size = std::max(1U, (axis == 0 ? src.info.size.width :
                                                src.info.size.height) >> s.base_level);
            const u32 target_size = std::max(1U, (axis == 0 ? dst.info.size.width :
                                                dst.info.size.height) >> d.base_level);
            const char* failure{};
            if (!PlanBcCopyAxis(source_offset, extent, source_size, target_offset, target_size,
                                4, &failure)) {
                // Separate cap keeps the detailed predicate available without per-frame spam.
                static u32 axis_reports = 0;
                if (axis_reports < 32) {
                    ++axis_reports;
                    LOG_WARNING(Render,
                                "D3D12: BC axis {} rejected: {}; src offset={} mip size={}, "
                                "extent={}, block size=4; dst offset={} mip size={}; "
                                "src resource base={}x{} dst resource base={}x{}",
                                axis == 0 ? "X" : "Y", failure, source_offset, source_size,
                                extent, target_offset, target_size, source_desc.Width,
                                source_desc.Height, target_desc.Width, target_desc.Height);
                }
                return reject(failure, &copy);
            }
        }
        if (!src.HasGuestBlocks(copy)) return reject("uninitialized block coverage", &copy);
    }
    blocks->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& copy : copies) {
        const u32 level = copy.src_subresource.base_level;
        // D3D12 copy boxes use whole blocks even for 1x1/2x2 tail mips. The destination receives
        // one integer texel for each block, not one for each decoded color texel.
        const D3D12_BOX box{
            static_cast<u32>(copy.src_offset.x),
            static_cast<u32>(copy.src_offset.y),
            0,
            static_cast<u32>(copy.src_offset.x) + Common::AlignUp(copy.extent.width, 4U),
            static_cast<u32>(copy.src_offset.y) + Common::AlignUp(copy.extent.height, 4U),
            1};
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            const D3D12_TEXTURE_COPY_LOCATION source{
                .pResource = blocks->Handle(),
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex =
                    blocks->Subresource(level, copy.src_subresource.base_layer + layer)};
            const D3D12_TEXTURE_COPY_LOCATION target{
                .pResource = dst.Handle(),
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst.Subresource(copy.dst_subresource.base_level,
                                                    copy.dst_subresource.base_layer + layer)};
            scheduler.CommandList()->CopyTextureRegion(&target, copy.dst_offset.x,
                                                       copy.dst_offset.y, 0, &source, &box);
        }
    }
    return true;
}
void TextureCacheRuntime::CopyThroughBuffer(Image& dst, Image& src,
                                            std::span<const ImageCopy> copies) {
    const u32 texel_bytes = VideoCore::Surface::BytesPerBlock(src.TransferFormat().copy_format);
    if (texel_bytes != VideoCore::Surface::BytesPerBlock(dst.TransferFormat().copy_format)) {
        WarnOnce(logged_reinterpret_copy,
                 "copy between formats of different texel sizes ({} -> "
                 "{}) skipped",
                 src.info.format, dst.info.format);
        return;
    }
    // A buffer holds no multisampled texels, and the two planes of a depth-stencil image have
    // their own path (CopyDepthStencilFrom).
    if (src.info.num_samples > 1 || dst.info.num_samples > 1 || src.IsDepthStencilPlanar() ||
        dst.IsDepthStencilPlanar()) {
        WarnOnce(logged_reinterpret_copy,
                 "copy of part of a multisampled or depth-stencil image ({} -> {}) skipped",
                 src.info.format, dst.info.format);
        return;
    }
    // A depth side goes to or from the buffer whole (see CopiedWhole); the region is picked
    // out of the buffer instead.
    const bool src_whole = CopiedWhole(src);
    const bool dst_whole = CopiedWhole(dst);
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(src.TransferFormat().copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(src.TransferFormat().copy_format);
    const bool src_3d = src.info.type == ImageType::e3D;
    const bool dst_3d = dst.info.type == ImageType::e3D;
    ID3D12Device* const d3d = device.Get();
    const D3D12_RESOURCE_DESC src_desc = src.Handle()->GetDesc();
    const D3D12_RESOURCE_DESC dst_desc = dst.Handle()->GetDesc();
    const auto whole_footprint = [d3d](const D3D12_RESOURCE_DESC& desc, u32 subresource,
                                       u64& bytes) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        d3d->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr,
                                   &bytes);
        return footprint;
    };
    auto* const commands = scheduler.CommandList();
    for (const auto& copy : copies) {
        // One slice at a time: a 2D image's layers are a 3D image's depth slices.
        const s32 slices = src_3d ? static_cast<s32>(std::max(1U, copy.extent.depth))
                                  : copy.src_subresource.num_layers;
        // Whole blocks, also on the 2x2 and 1x1 levels of a block-compressed image.
        const u32 width = Common::AlignUp(copy.extent.width, block_w);
        const u32 height = Common::AlignUp(copy.extent.height, block_h);
        const u32 row_bytes = width / block_w * texel_bytes;
        const u32 rows = height / block_h;
        if (row_bytes == 0 || rows == 0) {
            continue;
        }
        for (s32 slice = 0; slice < slices; ++slice) {
            const u32 src_subresource =
                src.Subresource(copy.src_subresource.base_level,
                                src_3d ? 0 : copy.src_subresource.base_layer + slice);
            const u32 dst_subresource =
                dst.Subresource(copy.dst_subresource.base_level,
                                dst_3d ? 0 : copy.dst_subresource.base_layer + slice);
            const u32 src_z = src_3d ? static_cast<u32>(copy.src_offset.z + slice) : 0U;
            const u32 dst_z = dst_3d ? static_cast<u32>(copy.dst_offset.z + slice) : 0U;
            // Source to buffer: the region alone, or the whole subresource with the region at
            // region_offset.
            D3D12_TEXTURE_COPY_LOCATION staged{.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            u64 staged_bytes{};
            u64 region_offset = 0;
            if (src_whole) {
                staged.PlacedFootprint = whole_footprint(src_desc, src_subresource, staged_bytes);
                region_offset = static_cast<u64>(copy.src_offset.y) / block_h *
                                    staged.PlacedFootprint.Footprint.RowPitch +
                                static_cast<u64>(copy.src_offset.x) / block_w * texel_bytes;
            } else {
                staged.PlacedFootprint.Footprint = {.Format = src.FootprintFormat(),
                                                    .Width = width,
                                                    .Height = height,
                                                    .Depth = 1,
                                                    .RowPitch = AlignPitch(row_bytes)};
                staged_bytes = static_cast<u64>(staged.PlacedFootprint.Footprint.RowPitch) * rows;
            }
            const u32 staged_pitch = staged.PlacedFootprint.Footprint.RowPitch;
            ComPtr<ID3D12Resource> transfer = transfer_buffers.Acquire(staged_bytes);
            staged.pResource = transfer.Get();
            const D3D12_TEXTURE_COPY_LOCATION source{
                .pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src_subresource};
            if (src_whole) {
                commands->CopyTextureRegion(&staged, 0, 0, 0, &source, nullptr);
            } else {
                const D3D12_BOX box{static_cast<u32>(copy.src_offset.x),
                                    static_cast<u32>(copy.src_offset.y), src_z,
                                    static_cast<u32>(copy.src_offset.x) + width,
                                    static_cast<u32>(copy.src_offset.y) + height, src_z + 1};
                commands->CopyTextureRegion(&staged, 0, 0, 0, &source, &box);
            }
            TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION target{
                .pResource = dst.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst_subresource};
            u64 dst_bytes{};
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT dst_footprint =
                dst_whole ? whole_footprint(dst_desc, dst_subresource, dst_bytes)
                          : D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
            const bool covers_dst = !dst_whole || (copy.dst_offset.x == 0 &&
                                                   copy.dst_offset.y == 0 &&
                                                   width == dst_footprint.Footprint.Width &&
                                                   height == dst_footprint.Footprint.Height);
            if (covers_dst && region_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0) {
                // The destination reads the region where it lies in the buffer: the same
                // bytes, described in the destination's format.
                D3D12_TEXTURE_COPY_LOCATION region = staged;
                region.PlacedFootprint.Offset = region_offset;
                region.PlacedFootprint.Footprint = {
                    .Format = dst_whole ? dst_footprint.Footprint.Format : dst.FootprintFormat(),
                    .Width = width,
                    .Height = height,
                    .Depth = 1,
                    .RowPitch = staged_pitch};
                if (dst_whole) {
                    commands->CopyTextureRegion(&target, 0, 0, 0, &region, nullptr);
                } else {
                    commands->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                                dst_z, &region, nullptr);
                }
                transfer_buffers.Release(std::move(transfer));
                continue;
            }
            // Otherwise the rows move to a second buffer laid out for the destination: its whole
            // subresource (read back first when the copy does not cover it), or the region.
            D3D12_TEXTURE_COPY_LOCATION placed{.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            u64 placed_offset = 0;
            if (dst_whole) {
                placed.PlacedFootprint = dst_footprint;
                placed_offset = static_cast<u64>(copy.dst_offset.y) / block_h *
                                    dst_footprint.Footprint.RowPitch +
                                static_cast<u64>(copy.dst_offset.x) / block_w * texel_bytes;
            } else {
                placed.PlacedFootprint.Footprint = {.Format = dst.FootprintFormat(),
                                                    .Width = width,
                                                    .Height = height,
                                                    .Depth = 1,
                                                    .RowPitch = AlignPitch(row_bytes)};
                dst_bytes = static_cast<u64>(placed.PlacedFootprint.Footprint.RowPitch) * rows;
            }
            const u32 placed_pitch = placed.PlacedFootprint.Footprint.RowPitch;
            ComPtr<ID3D12Resource> repacked = transfer_buffers.Acquire(dst_bytes);
            placed.pResource = repacked.Get();
            if (!covers_dst) {
                dst.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
                commands->CopyTextureRegion(&placed, 0, 0, 0, &target, nullptr);
                dst.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if (covers_dst && staged_pitch == placed_pitch) {
                // Nothing between the rows to keep: one copy.
                commands->CopyBufferRegion(repacked.Get(), placed_offset, transfer.Get(),
                                           region_offset,
                                           static_cast<u64>(rows - 1) * staged_pitch + row_bytes);
            } else {
                for (u32 row = 0; row < rows; ++row) {
                    commands->CopyBufferRegion(
                        repacked.Get(), placed_offset + static_cast<u64>(row) * placed_pitch,
                        transfer.Get(), region_offset + static_cast<u64>(row) * staged_pitch,
                        row_bytes);
                }
            }
            TransitionBuffer(commands, repacked.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (dst_whole) {
                commands->CopyTextureRegion(&target, 0, 0, 0, &placed, nullptr);
            } else {
                commands->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y, dst_z,
                                            &placed, nullptr);
            }
            transfer_buffers.Release(std::move(transfer));
            transfer_buffers.Release(std::move(repacked));
        }
    }
}

bool TextureCacheRuntime::CopyBlocksIntoDecoded(Image& dst, Image& src,
                                                std::span<const ImageCopy> copies) {
    using namespace VideoCore::Surface;
    // The source's texels are the destination's blocks, as the guest wrote them (each extent is
    // in source texels, a block each); copied as they are, they would land in a resource of
    // decoded texels.
    const PixelFormat block_format = dst.info.format;
    const u32 block_w = DefaultBlockWidth(block_format);
    const u32 block_h = DefaultBlockHeight(block_format);
    const FormatInfo blocks = Format(block_format);
    if (src.TransferFormat().converted || DefaultBlockWidth(src.info.format) != 1 ||
        DefaultBlockHeight(src.info.format) != 1 ||
        BytesPerBlock(src.info.format) != BytesPerBlock(block_format) || !blit_helper ||
        !blit_helper->IsAvailable() || src.info.num_samples > 1 ||
        (dst.Handle()->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return false;
    }
    Image* dst_blocks = nullptr;
    bool preserve_blocks = true;
    // The decoder and the preserved blocks must receive exactly the same valid batch. Validate
    // before emitting either representation; do not certify a shadow after a clipped/skipped copy.
    if (src.info.type != ImageType::e2D || dst.info.type != ImageType::e2D) {
        return false;
    }
    for (const auto& copy : copies) {
        const auto& s = copy.src_subresource;
        const auto& d = copy.dst_subresource;
        if (s.base_level < 0 || s.base_level >= src.info.resources.levels || d.base_level < 0 ||
            d.base_level >= dst.info.resources.levels || s.base_layer < 0 || d.base_layer < 0 ||
            s.num_layers <= 0 || s.num_layers != d.num_layers ||
            s.num_layers > src.info.resources.layers - s.base_layer ||
            d.num_layers > dst.info.resources.layers - d.base_layer || copy.src_offset.z != 0 ||
            copy.dst_offset.z != 0 || copy.extent.depth != 1) {
            return false;
        }
        if (copy.dst_offset.x % block_w != 0 || copy.dst_offset.y % block_h != 0) {
            preserve_blocks = false; // Decoded texels remain copyable, original blocks do not.
        }
        const u32 dw = std::max(1U, dst.info.size.width >> d.base_level);
        const u32 dh = std::max(1U, dst.info.size.height >> d.base_level);
        if (copy.dst_offset.x < 0 || copy.dst_offset.y < 0 ||
            static_cast<u32>(copy.dst_offset.x) >= dw ||
            static_cast<u32>(copy.dst_offset.y) >= dh || copy.extent.width == 0 ||
            copy.extent.height == 0 ||
            copy.extent.width > (dw - copy.dst_offset.x + block_w - 1) / block_w ||
            copy.extent.height > (dh - copy.dst_offset.y + block_h - 1) / block_h ||
            !PlanBcCopyAxis(copy.src_offset.x, copy.extent.width,
                            std::max(1U, src.info.size.width >> s.base_level), 0, UINT32_MAX, 1) ||
            !PlanBcCopyAxis(copy.src_offset.y, copy.extent.height,
                            std::max(1U, src.info.size.height >> s.base_level), 0, UINT32_MAX, 1)) {
            return false;
        }
    }
    if (preserve_blocks) dst_blocks = dst.EnsureGuestBlocks();
    static bool logged = false;
    if (!std::exchange(logged, true)) {
        LOG_INFO(Render,
                 "D3D12: blocks of {} {}x{} @{:x} decoded on the GPU into {} {}x{}x{} "
                 "@{:x}",
                 src.info.format, src.info.size.width, src.info.size.height, src.gpu_addr,
                 dst.info.format, dst.info.size.width, dst.info.size.height,
                 dst.info.resources.layers, dst.gpu_addr);
    }
    ID3D12Device* const device_handle = device.Get();
    auto* const commands = scheduler.CommandList();
    const u32 block_bytes = BytesPerBlock(block_format);
    for (const auto& copy : copies) {
        const u32 blocks_x = copy.extent.width;
        const u32 blocks_y = copy.extent.height;
        if (blocks_x == 0 || blocks_y == 0) {
            continue;
        }
        const u32 level = static_cast<u32>(copy.dst_subresource.base_level);
        const u32 level_width = std::max(1U, dst.info.size.width >> level);
        const u32 level_height = std::max(1U, dst.info.size.height >> level);
        const u32 dst_x = static_cast<u32>(copy.dst_offset.x);
        const u32 dst_y = static_cast<u32>(copy.dst_offset.y);
        if (dst_x >= level_width || dst_y >= level_height) {
            continue;
        }
        // Blocks past the level's edge (its last blocks, or the 4x4 block of a 2x2 level)
        // decode texels the level does not have.
        const u32 width = std::min(blocks_x * block_w, level_width - dst_x);
        const u32 height = std::min(blocks_y * block_h, level_height - dst_y);
        const u32 row_pitch = AlignPitch(blocks_x * block_bytes);
        const u64 size = static_cast<u64>(row_pitch) * blocks_y;
        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Width = blocks_x * block_w,
            .Height = blocks_y * block_h,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = blocks.resource,
            .SampleDesc = {.Count = 1},
        };
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            // The source's bytes to a buffer, then into a block-compressed texture of their own.
            ComPtr<ID3D12Resource> decoded;
            const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
            ThrowIfFailed(device_handle->CreateCommittedResource(
                              &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                              nullptr, IID_PPV_ARGS(&decoded)),
                          "Create block decode texture");
            ComPtr<ID3D12Resource> transfer = transfer_buffers.Acquire(size);
            D3D12_TEXTURE_COPY_LOCATION footprint{.pResource = transfer.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            footprint.PlacedFootprint.Footprint = {.Format = src.FootprintFormat(),
                                                   .Width = blocks_x,
                                                   .Height = blocks_y,
                                                   .Depth = 1,
                                                   .RowPitch = row_pitch};
            src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION source{
                .pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src.Subresource(copy.src_subresource.base_level,
                                                    copy.src_subresource.base_layer + layer)};
            const D3D12_BOX box{static_cast<u32>(copy.src_offset.x),
                                static_cast<u32>(copy.src_offset.y), 0,
                                static_cast<u32>(copy.src_offset.x) + blocks_x,
                                static_cast<u32>(copy.src_offset.y) + blocks_y, 1};
            commands->CopyTextureRegion(&footprint, 0, 0, 0, &source, &box);
            TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            footprint.PlacedFootprint.Footprint.Format = blocks.resource;
            footprint.PlacedFootprint.Footprint.Width = blocks_x * block_w;
            footprint.PlacedFootprint.Footprint.Height = blocks_y * block_h;
            const D3D12_TEXTURE_COPY_LOCATION target{
                .pResource = decoded.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = 0};
            commands->CopyTextureRegion(&target, 0, 0, 0, &footprint, nullptr);
            if (dst_blocks) {
                // Reuse the very same BC footprint used for hardware decoding. This preserves
                // every input bit even when src's DXGI format has no direct BC copy pairing.
                dst_blocks->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
                const D3D12_TEXTURE_COPY_LOCATION original{
                    .pResource = dst_blocks->Handle(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = dst_blocks->Subresource(
                        copy.dst_subresource.base_level, copy.dst_subresource.base_layer + layer)};
                commands->CopyTextureRegion(&original, dst_x, dst_y, 0, &footprint, nullptr);
            }
            transfer_buffers.Release(std::move(transfer));
            TransitionResource(commands, decoded.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            // Sampling decodes the blocks; drawn texel for texel into the destination's layer.
            const D3D12_CPU_DESCRIPTOR_HANDLE srv = view_descriptors.Allocate();
            const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
                .Format = blocks.view,
                .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1}};
            device_handle->CreateShaderResourceView(decoded.Get(), &srv_desc, srv);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_descriptors.Allocate();
            const D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{
                .Format = dst.ViewFormat(),
                .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY,
                .Texture2DArray = {.MipSlice = level,
                                   .FirstArraySlice = static_cast<u32>(
                                       copy.dst_subresource.base_layer + layer),
                                   .ArraySize = 1}};
            device_handle->CreateRenderTargetView(dst.Handle(), &rtv_desc, rtv);
            dst.Transition(D3D12_RESOURCE_STATE_RENDER_TARGET);
            const VideoCommon::Region2D dst_region{
                .start = {static_cast<s32>(dst_x), static_cast<s32>(dst_y)},
                .end = {static_cast<s32>(dst_x + width), static_cast<s32>(dst_y + height)}};
            const VideoCommon::Region2D src_region{
                .start = {0, 0}, .end = {static_cast<s32>(width), static_cast<s32>(height)}};
            blit_helper->BlitColor({rtv, dst.ViewFormat(), 1}, srv,
                                   blit_helper->NearestSampler(), dst_region, src_region,
                                   {blocks_x * block_w, blocks_y * block_h});
            view_descriptors.Free(srv);
            rtv_descriptors.Free(rtv);
            scheduler.DeferRelease(std::move(decoded));
        }
    }
    if (dst_blocks) {
        for (auto copy : copies) {
            const u32 width = std::max(1U, dst.info.size.width >> copy.dst_subresource.base_level);
            const u32 height = std::max(1U, dst.info.size.height >> copy.dst_subresource.base_level);
            copy.extent.width = std::min(copy.extent.width * block_w, width - copy.dst_offset.x);
            copy.extent.height = std::min(copy.extent.height * block_h, height - copy.dst_offset.y);
            dst.IncludeGuestBlocks(copy);
        }
        dst.guest_blocks_version = dst.write_version;
    }
    return true;
}

void TextureCacheRuntime::CopyImageMSAA(Image&, Image&, std::span<const ImageCopy>) {
    LOG_WARNING(Render, "D3D12: MSAA texture copies are deferred to phase 5");
}
void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src, std::span<const ImageCopy> copies) {
    if (VideoCore::Surface::BytesPerBlock(dst.info.format) == VideoCore::Surface::BytesPerBlock(src.info.format))
        CopyImage(dst, src, copies);
    else LOG_WARNING(Render, "D3D12: incompatible texture reinterpretation skipped ({} -> {})", src.info.format, dst.info.format);
}
void TextureCacheRuntime::ConvertImage(Framebuffer*, ImageView&, ImageView&) {
    LOG_WARNING(Render, "D3D12: shader texture conversion is deferred to phase 5");
}
namespace {
/// Where a copy through a view lands in its image.
struct CopyPlace {
    u32 subresource;
    u32 level;
    u32 layer;  ///< array layer of a 2D image, 0 otherwise
    u32 z;      ///< depth slice of a 3D image, 0 otherwise
    s32 width;  ///< of the level, in texels (whole blocks for a block-compressed resource)
    s32 height;
};

/// The view's first level and layer, clamped to its image as the view's descriptors are. A guest
/// view may reach past the image, and a subresource index past the resource is an invalid call
/// that fails Close of the whole command list.
CopyPlace ViewCopyPlace(const Image& image, const ImageView& view) {
    const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
    const bool is_3d = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const u32 layers = is_3d ? 1U : desc.DepthOrArraySize;
    const u32 level = std::min(static_cast<u32>(std::max(view.range.base.level, 0)),
                               static_cast<u32>(desc.MipLevels) - 1);
    const u32 layer = static_cast<u32>(std::max(view.range.base.layer, 0));
    const u32 depth = is_3d ? std::max(1U, static_cast<u32>(desc.DepthOrArraySize) >> level) : 1U;
    u64 width = std::max<u64>(1, desc.Width >> level);
    u32 height = std::max(1U, desc.Height >> level);
    const DXGI_FORMAT format = desc.Format;
    if ((format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
        (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB)) {
        width = Common::AlignUp(width, 4);
        height = Common::AlignUp(height, 4U);
    }
    return {
        .subresource = level + (is_3d ? 0U : std::min(layer, layers - 1) * desc.MipLevels),
        .level = level,
        .layer = is_3d ? 0U : std::min(layer, layers - 1),
        .z = is_3d ? std::min(layer, depth - 1) : 0U,
        .width = static_cast<s32>(width),
        .height = static_cast<s32>(height),
    };
}

bool InsideLevel(const CopyPlace& place, const Region2D& region) {
    return region.start.x >= 0 && region.start.y >= 0 && region.end.x <= place.width &&
           region.end.y <= place.height;
}
} // namespace

bool TextureCacheRuntime::BlitCanReplaceDestination(const Image& dst, const Image& src,
                                                    PixelFormat dst_format,
                                                    PixelFormat src_format) const noexcept {
    // BlitImage either copies, draws the whole region or skips. Only a depth-stencil target is
    // drawn in part (depth without stencil), and its stencil must keep the guest's bytes.
    const SurfaceType dst_type = VideoCore::Surface::GetFormatType(dst_format);
    const SurfaceType src_type = VideoCore::Surface::GetFormatType(src_format);
    return &dst != &src && dst.info.num_samples == 1 && src.info.num_samples == 1 &&
           (dst_type == SurfaceType::ColorTexture || dst_type == SurfaceType::Depth) &&
           (src_type == SurfaceType::ColorTexture) == (dst_type == SurfaceType::ColorTexture);
}
bool TextureCacheRuntime::BlitImage(Framebuffer*, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation) {
    // Blend operations are drawn as copies, as in the Vulkan backend.
    Image* const dst_image = dst.SourceImage();
    Image* const src_image = src.SourceImage();
    if (!dst_image || !src_image || !dst_image->Handle() || !src_image->Handle()) {
        return false;
    }
    if (dst_image == src_image) {
        BlitWithinImage(dst, src, dst_region, src_region, filter);
        return false;
    }
    if (dst_image->info.num_samples > 1 || src_image->info.num_samples > 1) {
        WarnOnce(logged_msaa_blit, "MSAA blits and resolves need phase 5; skipped");
        return false;
    }
    const SurfaceType src_type = VideoCore::Surface::GetFormatType(src.format);
    const SurfaceType dst_type = VideoCore::Surface::GetFormatType(dst.format);
    const bool src_color = src_type == SurfaceType::ColorTexture;
    if (src_color != (dst_type == SurfaceType::ColorTexture)) {
        WarnOnce(logged_mixed_blit, "blit between color and depth ({} -> {}) skipped", src.format,
                 dst.format);
        return false;
    }
    const s32 dst_width = dst_region.end.x - dst_region.start.x;
    const s32 dst_height = dst_region.end.y - dst_region.start.y;
    const bool same_extent = dst_width > 0 && dst_height > 0 &&
                             dst_width == src_region.end.x - src_region.start.x &&
                             dst_height == src_region.end.y - src_region.start.y;
    const CopyPlace src_place = ViewCopyPlace(*src_image, src);
    const CopyPlace dst_place = ViewCopyPlace(*dst_image, dst);
    // A box past either level is an invalid copy; the shader path clamps it instead.
    if (src_color && same_extent && src.format == dst.format &&
        InsideLevel(src_place, src_region) && InsideLevel(dst_place, dst_region)) {
        if (!Image::AreCopyCompatible(*src_image, *dst_image)) {
            WarnOnce(logged_decoded_copy, "copy between a host-decoded and a compressed image ({} "
                     "-> {}) skipped", src.format, dst.format);
            return false;
        }
        // An unscaled copy: exact for every format, integers included.
        src_image->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        dst_image->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        if (TypelessFamily(src_image->ResourceFormat()) !=
            TypelessFamily(dst_image->ResourceFormat())) {
            // One view format over resources of two families (R11G11B10 over R32...):
            // CopyTextureRegion between them fails the list. The bytes go through a buffer.
            const ImageCopy copy{
                .src_subresource = {.base_level = static_cast<s32>(src_place.level),
                                    .base_layer = static_cast<s32>(src_place.layer)},
                .dst_subresource = {.base_level = static_cast<s32>(dst_place.level),
                                    .base_layer = static_cast<s32>(dst_place.layer)},
                .src_offset = {src_region.start.x, src_region.start.y,
                               static_cast<s32>(src_place.z)},
                .dst_offset = {dst_region.start.x, dst_region.start.y,
                               static_cast<s32>(dst_place.z)},
                .extent = {static_cast<u32>(dst_width), static_cast<u32>(dst_height), 1},
            };
            CopyThroughBuffer(*dst_image, *src_image, std::span{&copy, 1});
            return true;
        }
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = src_image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = src_place.subresource};
        const D3D12_TEXTURE_COPY_LOCATION target{
            .pResource = dst_image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = dst_place.subresource};
        const D3D12_BOX box{static_cast<u32>(src_region.start.x),
                            static_cast<u32>(src_region.start.y), src_place.z,
                            static_cast<u32>(src_region.end.x),
                            static_cast<u32>(src_region.end.y), src_place.z + 1};
        scheduler.CommandList()->CopyTextureRegion(&target, static_cast<u32>(dst_region.start.x),
                                                   static_cast<u32>(dst_region.start.y),
                                                   dst_place.z, &source, &box);
        return true;
    }
    if (!blit_helper || !blit_helper->IsAvailable()) {
        WarnOnce(logged_no_blit_helper, "scaled blits need the shader path; skipped");
        return false;
    }
    const VideoCommon::Extent2D src_size{src.size.width, src.size.height};
    if (src_color) {
        if (VideoCore::Surface::IsPixelFormatInteger(src.format) ||
            VideoCore::Surface::IsPixelFormatInteger(dst.format)) {
            // The blit shader samples and writes floats.
            WarnOnce(logged_integer_blit, "scaled blits of integer formats ({} -> {}) skipped",
                     src.format, dst.format);
            return false;
        }
        if (!dst.RenderTarget().ptr) {
            WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                     dst.format);
            return false;
        }
        src_image->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src.PrepareRead(Shader::TextureType::Color2D);
        dst.PrepareRender();
        const bool linear = filter == Tegra::Engines::Fermi2D::Filter::Bilinear;
        return blit_helper->BlitColor({dst.RenderTarget(), Format(dst.format).view, 1},
                                      src.Handle(Shader::TextureType::Color2D),
                                      linear ? blit_helper->LinearSampler()
                                             : blit_helper->NearestSampler(),
                                      dst_region, src_region, src_size);
    }
    if (!dst.DepthStencil().ptr) {
        WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                 dst.format);
        return false;
    }
    if (dst_type == SurfaceType::DepthStencil) {
        // Writing stencil from a shader needs SV_StencilRef, which the Series lacks.
        WarnOnce(logged_stencil_blit, "depth-stencil blits copy depth only ({})", dst.format);
    }
    src_image->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dst_image->Transition(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const bool drawn = blit_helper->BlitDepth({dst.DepthStencil(), Format(dst.format).dsv, 1},
                                              src.Handle(Shader::TextureType::Color2D),
                                              dst_region, src_region, src_size);
    return drawn && dst_type == SurfaceType::Depth;
}
void TextureCacheRuntime::BlitWithinImage(ImageView& dst, ImageView& src,
                                          const Region2D& dst_region, const Region2D& src_region,
                                          Tegra::Engines::Fermi2D::Filter filter) {
    // Whole-resource state tracking cannot hold a shader resource and a target at once, and a
    // copy cannot read and write one subresource; a scratch copy of the source region can.
    Image& image = *src.SourceImage();
    const PixelFormat image_format = image.info.format;
    const bool color = VideoCore::Surface::GetFormatType(src.format) == SurfaceType::ColorTexture &&
                       VideoCore::Surface::GetFormatType(dst.format) == SurfaceType::ColorTexture;
    const FormatInfo src_format = Format(src.format);
    if (!color || image.info.num_samples > 1 || image.info.type == ImageType::e3D ||
        image.IsBcDecoded() || image.IsGpuDecoded() ||
        VideoCore::Surface::DefaultBlockWidth(image_format) != 1 ||
        VideoCore::Surface::DefaultBlockHeight(image_format) != 1 ||
        TypelessFamily(src_format.srv) != TypelessFamily(image.ResourceFormat())) {
        WarnOnce(logged_self_blit, "blit within one {} image ({} -> {}) skipped", image_format,
                 src.format, dst.format);
        return;
    }
    const s32 dst_width = dst_region.end.x - dst_region.start.x;
    const s32 dst_height = dst_region.end.y - dst_region.start.y;
    const s32 src_width = src_region.end.x - src_region.start.x;
    const s32 src_height = src_region.end.y - src_region.start.y;
    const bool copy = dst_width > 0 && dst_height > 0 && dst_width == src_width &&
                      dst_height == src_height && src.format == dst.format;
    if (!copy) {
        if (!blit_helper || !blit_helper->IsAvailable()) {
            WarnOnce(logged_no_blit_helper, "scaled blits need the shader path; skipped");
            return;
        }
        if (VideoCore::Surface::IsPixelFormatInteger(src.format) ||
            VideoCore::Surface::IsPixelFormatInteger(dst.format)) {
            WarnOnce(logged_integer_blit, "scaled blits of integer formats ({} -> {}) skipped",
                     src.format, dst.format);
            return;
        }
        if (!dst.RenderTarget().ptr) {
            WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                     dst.format);
            return;
        }
    }
    // The source rectangle, flips undone, clamped to its level.
    const CopyPlace src_place = ViewCopyPlace(image, src);
    const CopyPlace dst_place = ViewCopyPlace(image, dst);
    const s32 left = std::clamp(std::min(src_region.start.x, src_region.end.x), 0, src_place.width);
    const s32 top = std::clamp(std::min(src_region.start.y, src_region.end.y), 0, src_place.height);
    const s32 right = std::clamp(std::max(src_region.start.x, src_region.end.x), 0, src_place.width);
    const s32 bottom =
        std::clamp(std::max(src_region.start.y, src_region.end.y), 0, src_place.height);
    if (right <= left || bottom <= top) {
        return;
    }
    if (copy && !InsideLevel(dst_place, {.start = dst_region.start,
                                         .end = {dst_region.start.x + right - left,
                                                 dst_region.start.y + bottom - top}})) {
        WarnOnce(logged_self_blit, "blit within one {} image reaches past its level; skipped",
                 image_format);
        return;
    }
    const u32 width = static_cast<u32>(right - left);
    const u32 height = static_cast<u32>(bottom - top);
    EnsureSelfBlitScratch(image.ResourceFormat(), width, height);
    CommandStream* const commands = scheduler.CommandList();
    image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionSelfBlitScratch(D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_TEXTURE_COPY_LOCATION image_source{
        .pResource = image.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = src_place.subresource};
    const D3D12_TEXTURE_COPY_LOCATION scratch{
        .pResource = self_blit_scratch.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0};
    const D3D12_BOX box{static_cast<u32>(left), static_cast<u32>(top), 0,
                        static_cast<u32>(right), static_cast<u32>(bottom), 1};
    commands->CopyTextureRegion(&scratch, 0, 0, 0, &image_source, &box);
    if (copy) {
        // An unscaled copy: exact for every format, integers included.
        TransitionSelfBlitScratch(D3D12_RESOURCE_STATE_COPY_SOURCE);
        image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION target{
            .pResource = image.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = dst_place.subresource};
        const D3D12_BOX scratch_box{0, 0, 0, width, height, 1};
        commands->CopyTextureRegion(&target, static_cast<u32>(dst_region.start.x),
                                    static_cast<u32>(dst_region.start.y), 0, &scratch,
                                    &scratch_box);
        return;
    }
    TransitionSelfBlitScratch(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv = view_descriptors.Allocate();
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = src_format.srv,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0,
                      .ResourceMinLODClamp = 0.0f},
    };
    device.Get()->CreateShaderResourceView(self_blit_scratch.Get(), &srv_desc, srv);
    // Same orientation as the guest's region, moved to where the scratch holds it.
    const Region2D scratch_region{
        .start = {src_region.start.x - left, src_region.start.y - top},
        .end = {src_region.end.x - left, src_region.end.y - top},
    };
    dst.PrepareRender();
    const bool linear = filter == Tegra::Engines::Fermi2D::Filter::Bilinear;
    blit_helper->BlitColor({dst.RenderTarget(), Format(dst.format).view, 1}, srv,
                           linear ? blit_helper->LinearSampler() : blit_helper->NearestSampler(),
                           dst_region, scratch_region, {self_blit_width, self_blit_height});
    // The helper copies the descriptor into the shader-visible ring while recording.
    view_descriptors.Free(srv);
}

void TextureCacheRuntime::EnsureSelfBlitScratch(DXGI_FORMAT format, u32 width, u32 height) {
    if (self_blit_scratch && self_blit_format == format && self_blit_width >= width &&
        self_blit_height >= height) {
        return;
    }
    if (self_blit_scratch) {
        if (self_blit_format == format) {
            width = std::max(width, self_blit_width);
            height = std::max(height, self_blit_height);
        }
        scheduler.DeferRelease(std::move(self_blit_scratch));
    }
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(device.Get()->CreateCommittedResource(
                      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                      IID_PPV_ARGS(&self_blit_scratch)),
                  "Create self-blit scratch texture");
    self_blit_state = D3D12_RESOURCE_STATE_COMMON;
    self_blit_format = format;
    self_blit_width = width;
    self_blit_height = height;
    LOG_INFO(Render, "D3D12: blits within one image go through a {}x{} scratch (DXGI {})", width,
             height, static_cast<u32>(format));
}

void TextureCacheRuntime::TransitionSelfBlitScratch(D3D12_RESOURCE_STATES next) {
    if (self_blit_state == next) {
        return;
    }
    const D3D12_RESOURCE_BARRIER barrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {.pResource = self_blit_scratch.Get(),
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = self_blit_state,
                       .StateAfter = next}};
    scheduler.CommandList()->ResourceBarrier(1, &barrier);
    self_blit_state = next;
}

} // namespace D3D12
