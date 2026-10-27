// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>

#include <d3d12.h>

#include "common/common_types.h"

namespace D3D12 {

/// Channels (bit 0 red .. bit 3 alpha) a view format stores; the sampler fills the others with
/// 0 (color) and 1 (alpha), border texels included.
[[nodiscard]] constexpr u32 StoredChannels(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_A8_UNORM:
        return 0b1000;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_R1_UNORM:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 0b0001;
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 0b0010;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return 0b0011;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
        return 0b0111;
    default:
        return 0b1111;
    }
}

[[nodiscard]] constexpr bool IsIntegerView(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return true;
    default:
        return false;
    }
}

/// The border color as a sample through the view returns it: the sampler replaces a border texel
/// before format expansion, so channels the format lacks read 0 (alpha 1), then the view's
/// Shader4ComponentMapping applies. raw: the sampler's BorderColor bits; integer views read them
/// as integers, so their forced 1 is the integer 1.
[[nodiscard]] constexpr std::array<u32, 4> SampledBorderColor(const std::array<u32, 4>& raw,
                                                              DXGI_FORMAT format,
                                                              UINT mapping) noexcept {
    const u32 one = IsIntegerView(format) ? 1u : std::bit_cast<u32>(1.0f);
    const u32 stored = StoredChannels(format);
    std::array<u32, 4> expanded{};
    for (u32 i = 0; i < 4; ++i) {
        expanded[i] = (stored >> i) & 1 ? raw[i] : (i == 3 ? one : 0u);
    }
    std::array<u32, 4> result{};
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (mapping >> (3 * i)) & D3D12_SHADER_COMPONENT_MAPPING_MASK;
        if (source <= D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3) {
            result[i] = expanded[source];
        } else {
            result[i] = source == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 ? one : 0u;
        }
    }
    return result;
}

} // namespace D3D12
