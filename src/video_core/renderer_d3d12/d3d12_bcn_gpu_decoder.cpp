// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <limits>

#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/decode_bc.h"

// BC images kept as plain texels (DecodedBcFormat: arrays, unaligned extents, the guest palette)
// decoded on the GPU on upload, instead of ConvertImage on the GPU thread.

namespace D3D12 {

using namespace TextureDetail;

namespace {

// d3d12_bcn_decoder.comp's mode bits.
constexpr u32 BCN_BC1 = 1;
constexpr u32 BCN_BC2 = 2;
constexpr u32 BCN_BC3 = 3;
constexpr u32 BCN_BC4 = 4;
constexpr u32 BCN_BC5 = 5;
constexpr u32 BCN_BC7 = 7;
constexpr u32 BCN_SIGNED = 16;
constexpr u32 BCN_GUEST_PALETTE = 32;

/// The shader's mode for a guest format, 0 when it does not decode it (BC6H).
u32 BcnMode(PixelFormat format) {
    switch (format) {
    case PixelFormat::BC1_RGBA_UNORM:
    case PixelFormat::BC1_RGBA_SRGB:
        return BCN_BC1;
    case PixelFormat::BC2_UNORM:
    case PixelFormat::BC2_SRGB:
        return BCN_BC2;
    case PixelFormat::BC3_UNORM:
    case PixelFormat::BC3_SRGB:
        return BCN_BC3;
    case PixelFormat::BC4_UNORM:
        return BCN_BC4;
    case PixelFormat::BC4_SNORM:
        return BCN_BC4 | BCN_SIGNED;
    case PixelFormat::BC5_UNORM:
        return BCN_BC5;
    case PixelFormat::BC5_SNORM:
        return BCN_BC5 | BCN_SIGNED;
    case PixelFormat::BC7_UNORM:
    case PixelFormat::BC7_SRGB:
        return BCN_BC7;
    default:
        return 0;
    }
}

} // Anonymous namespace

bool TextureDetail::CanDecodeBcnOnGpu(PixelFormat format) {
    return BcnMode(format) != 0;
}

void TextureCacheRuntime::BcnImageUpload(Image& image, const StagingBufferRef& map,
                                         std::span<const VideoCommon::SwizzleParameters> swizzles) {
    u32 mode = BcnMode(image.info.format);
    if (mode == 0) {
        return;
    }
    // ConvertImage's palette (VideoCommon::DecompressBCn) for the BC1-3 color blocks.
    if ((mode & 15) <= BCN_BC3 && VideoCommon::GetBcGuestPalette()) {
        mode |= BCN_GUEST_PALETTE;
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploads, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploadBytes, image.guest_size_bytes);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureGpuDecodes, 1);
    if (!logged_gpu_bcn) {
        LOG_INFO(Render, "D3D12: first BC image decoded on the GPU ({} {}x{}, {} layers, {} "
                 "levels)", image.info.format, image.info.size.width, image.info.size.height,
                 image.info.resources.layers, image.info.resources.levels);
        logged_gpu_bcn = true;
    }
    // sRGB views cannot be UAVs; the texels are the same bytes through the UNORM one.
    const DXGI_FORMAT uav_format = image.format.view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                       ? DXGI_FORMAT_R8G8B8A8_UNORM
                                       : image.format.view;
    image.Transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const u64 base = map.offset;
    const u64 buffer_size = map.buffer->GetDesc().Width;
    const u32 layers = static_cast<u32>(image.info.resources.layers);
    for (const VideoCommon::SwizzleParameters& swizzle : swizzles) {
        const auto params = VideoCommon::Accelerated::MakeBlockLinearSwizzle2DParams(swizzle,
                                                                                     image.info);
        const u64 source_offset = base + swizzle.buffer_offset;
        const u64 wanted_bytes = static_cast<u64>(params.layer_stride) * layers;
        const u64 source_bytes = source_offset < buffer_size
                                     ? std::min(wanted_bytes, buffer_size - source_offset)
                                     : 0;
        const u32 input_words = static_cast<u32>(std::min<u64>(source_bytes / 4,
                                                               std::numeric_limits<u32>::max()));
        const D3D12_CPU_DESCRIPTOR_HANDLE source_srv = view_descriptors.Allocate();
        const D3D12_SHADER_RESOURCE_VIEW_DESC source_desc{
            .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {.FirstElement = input_words ? source_offset / 4 : 0,
                       .NumElements = input_words,
                       .StructureByteStride = 0,
                       .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
        };
        device.Get()->CreateShaderResourceView(input_words ? map.buffer : nullptr, &source_desc,
                                               source_srv);
        // Each level is its own subresource: no UAV barrier between the level dispatches.
        const D3D12_CPU_DESCRIPTOR_HANDLE uav = view_descriptors.Allocate();
        const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
            .Format = uav_format,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY,
            .Texture2DArray = {.MipSlice = static_cast<UINT>(swizzle.level),
                               .FirstArraySlice = 0,
                               .ArraySize = layers,
                               .PlaneSlice = 0},
        };
        device.Get()->CreateUnorderedAccessView(image.Handle(), nullptr, &desc, uav);
        blit_helper->DecodeBcn({
            .source = source_srv,
            .destination = uav,
            .mode = mode,
            .layer_stride = params.layer_stride,
            .block_size = params.block_size,
            .x_shift = params.x_shift,
            .gob_block_height = params.block_height,
            .gob_block_height_mask = params.block_height_mask,
            .blocks_x = swizzle.num_tiles.width,
            .blocks_y = swizzle.num_tiles.height,
            .layers = layers,
            .input_words = input_words,
        });
        view_descriptors.Free(uav);
        view_descriptors.Free(source_srv);
    }
    // The native blocks for exact copies between BC images (PreserveGuestBlocks), unswizzled from
    // the same staging. They match the contents the decode above wrote.
    if (blit_helper->CanUnswizzle()) {
        if (Image* const blocks = image.EnsureGuestBlocks()) {
            UnswizzleImageUpload(*blocks, map, swizzles);
            image.guest_blocks_coverage.Fill();
            image.guest_blocks_version = image.write_version;
        }
    }
}

} // namespace D3D12
