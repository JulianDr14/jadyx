// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/texture_cache/util.h"

// The ASTC array layer probe. On the Series the texture unit reads some layers of
// block-compressed 2D arrays from another place than the one CopyTextureRegion wrote (0.2.44: a
// 128x128x105 BC4 array read layers 84 and 100 32 rows off; an ASTC lightmap array recompressed
// to BC3 went black the same way). Before an ASTC array recompressed to BC gets its texels, a
// marker block goes into every layer and level through the copy engine, a compute shader reads
// them back through the texture unit, and the shapes with misread layers become RGBA8, which the
// ASTC decoder writes through a UAV: no copy, no misread. Arrays that read right stay BC.

namespace D3D12 {

using namespace TextureDetail;

namespace {

/// Each marker is a lone 4x4 block at its own 512-byte placement in the upload buffer.
constexpr u64 MARKER_STRIDE = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
/// Above this many markers (layers x (levels + 2)) the array goes unprobed: 4 MiB of markers.
constexpr u32 MAX_MARKERS = 8192;
/// Per channel; the BC endpoint expansion rounds differently between decoders.
constexpr int TOLERANCE = 3;

enum ProbeKind : u8 { ProbeBc1, ProbeBc3, ProbeBc4 };

/// The UNORM view of the resource's BC format, or nothing for the formats the probe does not
/// know (sRGB views would bend the expected values).
std::optional<std::pair<ProbeKind, DXGI_FORMAT>> ProbeFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
        return std::pair{ProbeBc1, DXGI_FORMAT_BC1_UNORM};
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
        return std::pair{ProbeBc3, DXGI_FORMAT_BC3_UNORM};
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
        return std::pair{ProbeBc4, DXGI_FORMAT_BC4_UNORM};
    default:
        return std::nullopt;
    }
}

u32 Hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct Marker {
    std::array<u8, 16> block{};
    u32 expected{}; ///< packUnorm4x8 of the texel: red in the low byte
};

/// A one-color block, distinct per marker, never near black (fresh memory reads zero).
Marker MakeMarker(ProbeKind kind, u32 salt, u32 index) {
    const u32 hash = Hash(index * 0x9E3779B9U + salt);
    const u16 color = static_cast<u16>(hash | 0x8000);
    const u8 alpha = static_cast<u8>((hash >> 16) | 0x80);
    const u8 red8 = static_cast<u8>((hash >> 24) | 0x80);
    const u32 r = ((color >> 11) & 31) << 3 | ((color >> 11) & 31) >> 2;
    const u32 g = ((color >> 5) & 63) << 2 | ((color >> 5) & 63) >> 4;
    const u32 b = (color & 31) << 3 | (color & 31) >> 2;
    Marker marker;
    // Both endpoints equal and every index 0: the texel is endpoint 0 in either BC1 mode.
    const auto put_color = [&](u8* block) {
        block[0] = block[2] = static_cast<u8>(color);
        block[1] = block[3] = static_cast<u8>(color >> 8);
    };
    switch (kind) {
    case ProbeBc1:
        put_color(marker.block.data());
        marker.expected = r | g << 8 | b << 16 | 0xFFU << 24;
        break;
    case ProbeBc3:
        marker.block[0] = marker.block[1] = alpha;
        put_color(marker.block.data() + 8);
        marker.expected = r | g << 8 | b << 16 | static_cast<u32>(alpha) << 24;
        break;
    case ProbeBc4:
        marker.block[0] = marker.block[1] = red8;
        marker.expected = red8 | 0xFFU << 24;
        break;
    }
    return marker;
}

bool Matches(u32 read, u32 expected) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        const int a = static_cast<int>((read >> shift) & 0xFF);
        const int b = static_cast<int>((expected >> shift) & 0xFF);
        if (std::abs(a - b) > TOLERANCE) {
            return false;
        }
    }
    return true;
}

/// Where marker `slot` of a layer sits, as d3d12_array_layer_probe.comp reads it.
struct MarkerPlace {
    u32 level;
    u32 x;
    u32 y;
};
MarkerPlace PlaceOf(u32 slot, u32 width, u32 height) {
    const u32 level = slot < 3 ? 0 : slot - 2;
    const u32 blocks_x = Common::DivCeil(std::max(width >> level, 1U), 4U);
    const u32 blocks_y = Common::DivCeil(std::max(height >> level, 1U), 4U);
    u32 bx = 0;
    u32 by = 0;
    if (slot == 1) {
        bx = blocks_x / 2;
        by = blocks_y / 2;
    } else if (slot == 2) {
        bx = blocks_x - 1;
        by = blocks_y - 1;
    }
    return {level, bx * 4, by * 4};
}

constexpr const char* KIND_NAMES[] = {"BC1", "BC3", "BC4"};

} // Anonymous namespace

void TextureCacheRuntime::ProbeAstcArrayLayers(Image& image) {
    image.layers_probed = true;
    if (!blit_helper || !blit_helper->CanProbeArrayLayers() ||
        !VideoCommon::WantsAstcArrayProbe(image.info)) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
    const auto format = ProbeFormat(desc.Format);
    const u32 layers = desc.DepthOrArraySize;
    const u32 levels = desc.MipLevels;
    const u32 width = static_cast<u32>(desc.Width);
    const u32 height = desc.Height;
    const u32 per_layer = levels + 2;
    const u32 total = per_layer * layers;
    if (!format || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || layers < 2 ||
        total > MAX_MARKERS) {
        LOG_INFO(Render, "D3D12: ASTC array {}x{}x{} levels {} (DXGI {}) at 0x{:X} not probed",
                 width, height, layers, levels, static_cast<u32>(desc.Format), image.gpu_addr);
        return;
    }
    const auto [kind, view_format] = *format;
    const u32 salt = Hash(++astc_array_probe_count * 0x85EBCA6BU);
    ID3D12Device* const dev = device.Get();

    AstcArrayProbe probe{
        .tick = 0,
        .markers = CreateCommittedBuffer(dev, MARKER_STRIDE * total, D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_STATE_GENERIC_READ,
                                         D3D12_RESOURCE_FLAG_NONE, "ASTC array probe markers"),
        .results = CreateTransferBuffer(dev, Common::AlignUp(total * 4ULL, 256ULL), true),
        .readback = CreateCommittedBuffer(dev, Common::AlignUp(total * 4ULL, 256ULL),
                                          D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_FLAG_NONE, "ASTC array probe readback"),
        .info = image.info,
        .gpu_addr = image.gpu_addr,
        .kind = kind,
        .salt = salt,
        .width = width,
        .height = height,
        .layers = layers,
        .levels = levels,
    };
    u8* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    ThrowIfFailed(probe.markers->Map(0, &no_read, reinterpret_cast<void**>(&mapped)),
                  "Map ASTC array probe markers");
    for (u32 index = 0; index < total; ++index) {
        const Marker marker = MakeMarker(kind, salt, index);
        std::memcpy(mapped + MARKER_STRIDE * index, marker.block.data(), marker.block.size());
    }
    probe.markers->Unmap(0, nullptr);

    auto* const commands = scheduler.CommandList();
    image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    for (u32 index = 0; index < total; ++index) {
        const MarkerPlace place = PlaceOf(index % per_layer, width, height);
        const D3D12_TEXTURE_COPY_LOCATION src{
            .pResource = probe.markers.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = {.Offset = MARKER_STRIDE * index,
                                .Footprint = {.Format = image.FootprintFormat(),
                                              .Width = 4,
                                              .Height = 4,
                                              .Depth = 1,
                                              .RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}},
        };
        const D3D12_TEXTURE_COPY_LOCATION dst{
            .pResource = image.Handle(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = place.level + (index / per_layer) * levels,
        };
        commands->CopyTextureRegion(&dst, place.x, place.y, 0, &src, nullptr);
    }
    image.Transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionBuffer(commands, probe.results.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv = view_descriptors.Allocate();
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = view_format,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2DArray = {.MostDetailedMip = 0, .MipLevels = levels, .FirstArraySlice = 0,
                           .ArraySize = layers, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f},
    };
    dev->CreateShaderResourceView(image.Handle(), &srv_desc, srv);
    blit_helper->ProbeArrayLayers({.source = srv,
                                   .destination = probe.results->GetGPUVirtualAddress(),
                                   .width = width,
                                   .height = height,
                                   .layers = layers,
                                   .levels = levels});
    view_descriptors.Free(srv);
    TransitionBuffer(commands, probe.results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(probe.readback.Get(), 0, probe.results.Get(), 0, total * 4ULL);
    probe.tick = scheduler.CurrentTick();
    astc_array_probes.push_back(std::move(probe));
}

void TextureCacheRuntime::PollAstcArrayProbes() {
    std::erase_if(astc_array_probes, [&](AstcArrayProbe& probe) {
        if (!scheduler.IsFree(probe.tick)) {
            return false;
        }
        const u32 per_layer = probe.levels + 2;
        const u32 total = per_layer * probe.layers;
        const u32* words = nullptr;
        const D3D12_RANGE read_range{0, total * 4ULL};
        if (FAILED(probe.readback->Map(0, &read_range, reinterpret_cast<void**>(
                                                          const_cast<u32**>(&words))))) {
            LOG_WARNING(Render, "D3D12: ASTC array probe readback failed to map");
            return true;
        }
        std::vector<u32> bad_layers;
        u32 first_read = 0;
        u32 first_expected = 0;
        for (u32 layer = 0; layer < probe.layers; ++layer) {
            for (u32 slot = 0; slot < per_layer; ++slot) {
                const u32 index = layer * per_layer + slot;
                const u32 expected = MakeMarker(static_cast<ProbeKind>(probe.kind), probe.salt,
                                                index).expected;
                if (!Matches(words[index], expected)) {
                    if (bad_layers.empty()) {
                        first_read = words[index];
                        first_expected = expected;
                    }
                    bad_layers.push_back(layer);
                    break;
                }
            }
        }
        const D3D12_RANGE no_write{0, 0};
        probe.readback->Unmap(0, &no_write);

        if (bad_layers.empty()) {
            LOG_INFO(Render, "D3D12: ASTC array probe {}x{}x{} levels {} {} at 0x{:X}: all "
                     "layers read right, stays BC", probe.width, probe.height, probe.layers,
                     probe.levels, KIND_NAMES[probe.kind], probe.gpu_addr);
            return true;
        }
        std::string list;
        for (size_t i = 0; i < bad_layers.size() && i < 24; ++i) {
            list += fmt::format("{}{}", i ? "," : "", bad_layers[i]);
        }
        if (bad_layers.size() > 24) {
            list += ",...";
        }
        const bool rgba = VideoCommon::ReportAstcArrayMisread(probe.info);
        LOG_WARNING(Render, "D3D12: ASTC array probe {}x{}x{} levels {} {} at 0x{:X}: {} of {} "
                    "layers misread [{}] (first read 0x{:08X}, wanted 0x{:08X}), {}",
                    probe.width, probe.height, probe.layers, probe.levels,
                    KIND_NAMES[probe.kind], probe.gpu_addr, bad_layers.size(), probe.layers,
                    list, first_read, first_expected,
                    rgba ? "this shape goes RGBA8" : "too large for RGBA8, stays BC");
        return true;
    });
}

} // namespace D3D12
