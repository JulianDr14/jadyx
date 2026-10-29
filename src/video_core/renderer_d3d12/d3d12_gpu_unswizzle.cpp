// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <bit>
#include <limits>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"

// Big block-linear color textures unswizzled by a compute shader on upload: the CPU only copies
// the guest bytes into staging. The block size, stride alignment and layer stride are computed the
// same way as the CPU UnswizzleImage, so both paths read the same guest bytes.

namespace D3D12 {

using namespace TextureDetail;

namespace {
constexpr u32 GOB_X_SHIFT = 6; // 64 bytes
constexpr u32 GOB_Y_SHIFT = 3; // 8 rows
constexpr u32 GOB_SHIFT = 9;   // 512 bytes
} // namespace

void TextureCacheRuntime::UnswizzleImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles) {
    if (!image.Handle() || !image.CanTransfer()) {
        return;
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploads, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploadBytes, image.guest_size_bytes);
    const PixelFormat copy_format = image.TransferFormat().copy_format;
    const u32 bytes_per_block = VideoCore::Surface::BytesPerBlock(copy_format);
    const u32 tile_width = VideoCore::Surface::DefaultBlockWidth(copy_format);
    const u32 tile_height = VideoCore::Surface::DefaultBlockHeight(copy_format);
    // Staging offsets are placement aligned, so the root SRV address is word aligned.
    const D3D12_GPU_VIRTUAL_ADDRESS source = map.buffer->GetGPUVirtualAddress() + map.offset;
    const u64 buffer_size = map.buffer->GetDesc().Width;
    const u64 readable = map.offset < buffer_size
                             ? std::min<u64>(buffer_size - map.offset, map.mapped_span.size())
                             : 0;
    const u32 source_bytes =
        static_cast<u32>(std::min<u64>(readable, std::numeric_limits<u32>::max()));
    if (!logged_gpu_unswizzle) {
        logged_gpu_unswizzle = true;
        LOG_INFO(Render, "D3D12: first texture unswizzled on the GPU ({} {}x{}, {} layers, {} "
                 "levels, {} KiB)", image.info.format, image.info.size.width,
                 image.info.size.height, image.info.resources.layers,
                 image.info.resources.levels, image.guest_size_bytes / 1024);
    }

    // One scratch footprint for every band: the widest level's rows, bounded by the budget.
    u64 scratch_size = 0;
    for (const VideoCommon::SwizzleParameters& swizzle : swizzles) {
        const u64 row_pitch = AlignPitch(swizzle.num_tiles.width * bytes_per_block);
        const u64 band_rows = std::clamp<u64>(GPU_UNSWIZZLE_SCRATCH_BUDGET / row_pitch, 1,
                                              swizzle.num_tiles.height);
        scratch_size = std::max(scratch_size, row_pitch * band_rows);
    }
    if (scratch_size == 0) {
        return;
    }
    ComPtr<ID3D12Resource> scratch = transfer_buffers.Acquire(scratch_size, true, "GPU unswizzle");
    D3D12_RESOURCE_STATES scratch_state = D3D12_RESOURCE_STATE_COMMON;
    image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);

    const u32 layers = static_cast<u32>(image.info.resources.layers);
    const u32 bpp_log2 = static_cast<u32>(std::countr_zero(bytes_per_block));
    const u32 layer_stride = VideoCommon::CalculateLayerStride(image.info);
    // GOB of the level 0 block height, as the CPU path uses for every level.
    const u32 gob_width_log2 = GOB_X_SHIFT - bpp_log2 + image.info.tile_width_spacing;
    const u32 gob_height_log2 = GOB_Y_SHIFT + image.info.block.height;
    for (const VideoCommon::SwizzleParameters& swizzle : swizzles) {
        if (swizzle.level < static_cast<s32>(image.LevelBias())) {
            continue;
        }
        const VideoCommon::Extent3D num_tiles = swizzle.num_tiles;
        const bool smaller_than_gob = num_tiles.width <= (1U << gob_width_log2) ||
                                      num_tiles.height <= (1U << gob_height_log2) ||
                                      num_tiles.depth < (1U << image.info.block.depth);
        const u32 stride_alignment = smaller_than_gob ? GOB_X_SHIFT - bpp_log2 : gob_width_log2;
        const u32 stride = Common::AlignUpLog2(num_tiles.width, stride_alignment) * bytes_per_block;
        const u32 x_shift = GOB_SHIFT + swizzle.block.height + swizzle.block.depth;
        const u32 block_size = Common::DivCeilLog2(stride, GOB_X_SHIFT) << x_shift;
        const u32 mip_width = std::max(1U, image.info.size.width >> swizzle.level);
        const u32 row_bytes = swizzle.num_tiles.width * bytes_per_block;
        const u32 row_pitch = AlignPitch(row_bytes);
        const u32 block_rows = swizzle.num_tiles.height;
        const u32 band_rows = static_cast<u32>(std::clamp<u64>(
            GPU_UNSWIZZLE_SCRATCH_BUDGET / row_pitch, 1, block_rows));
        for (u32 layer = 0; layer < layers; ++layer) {
            const u64 level_offset = static_cast<u64>(swizzle.buffer_offset) +
                                     static_cast<u64>(layer_stride) * layer;
            if (level_offset % 4 != 0 || level_offset >= source_bytes) {
                continue; // nothing readable (the CPU path would read zeros too)
            }
            for (u32 first_row = 0; first_row < block_rows; first_row += band_rows) {
                const u32 rows = std::min(band_rows, block_rows - first_row);
                auto* const commands = scheduler.CommandList();
                TransitionBuffer(commands, scratch.Get(), scratch_state,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                scratch_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                blit_helper->UnswizzleBlockLinear({
                    .source = source,
                    .destination = scratch->GetGPUVirtualAddress(),
                    .source_offset = static_cast<u32>(level_offset),
                    .source_bytes = source_bytes,
                    .block_size = block_size,
                    .x_shift = x_shift,
                    .block_height = swizzle.block.height,
                    .block_height_mask = (1U << swizzle.block.height) - 1,
                    .row_words = Common::DivCeil(row_bytes, 4U),
                    .rows = rows,
                    .first_row = first_row,
                    .row_pitch_words = row_pitch / 4,
                });
                TransitionBuffer(commands, scratch.Get(), scratch_state,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
                scratch_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                const D3D12_TEXTURE_COPY_LOCATION src{
                    .pResource = scratch.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    .PlacedFootprint = {
                        .Offset = 0,
                        .Footprint = {.Format = image.FootprintFormat(),
                                      .Width = Common::AlignUp(mip_width, tile_width),
                                      .Height = rows * tile_height,
                                      .Depth = 1,
                                      .RowPitch = row_pitch},
                    },
                };
                const D3D12_TEXTURE_COPY_LOCATION dst{
                    .pResource = image.Handle(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = image.Subresource(swizzle.level, layer),
                };
                commands->CopyTextureRegion(&dst, 0, first_row * tile_height, 0, &src, nullptr);
            }
        }
    }
    transfer_buffers.Release(std::move(scratch));
}

} // namespace D3D12
