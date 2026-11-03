// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>

#include "common/common_types.h"
#include "video_core/surface.h"

/// Render corrections set from boot.cfg: each changes what the host draws to match the guest GPU
/// more closely (or to share more pipelines). boot.cfg turns them on by default (BootConfig), and
/// each can be turned off there; they apply to objects created after they are set.
namespace D3D12 {

/// Sampler border colors as the guest GPU stores them.
enum class BorderColorMode : u8 {
    Exact,      ///< The guest's floats as written.
    Steps8,     ///< Each channel in [0, 1] rounded to an 8-bit step.
    BlackWhite, ///< Only transparent black, opaque black or opaque white.
};

/// boot.cfg "border_color=exact|8bit|bw".
void SetBorderColorMode(BorderColorMode mode);
[[nodiscard]] BorderColorMode GetBorderColorMode();

/// boot.cfg "depth_bias_round=1": pipeline keys keep only the constant depth bias D3D12 applies
/// and the slope and clamp rounded to 1/16, so near-identical biases share one pipeline.
void SetDepthBiasRounding(bool enabled);
[[nodiscard]] bool GetDepthBiasRounding();

/// boot.cfg "unorm_round=1": fragment outputs to 8-bit UNORM targets without blending round
/// ties down like the guest (0.5 -> 127) instead of to nearest (128).
void SetUnormHalfDownRounding(bool enabled);
[[nodiscard]] bool GetUnormHalfDownRounding();

/// boot.cfg "cube_sanitize=1": Shader::Profile::sanitize_cube_directions.
void SetCubeDirectionSanitize(bool enabled);
[[nodiscard]] bool GetCubeDirectionSanitize();

/// boot.cfg "half_ftz=1": Shader::Profile::flush_lowered_fp16_denorms.
void SetHalfFloatFlush(bool enabled);
[[nodiscard]] bool GetHalfFloatFlush();

/// boot.cfg "a2c_shader=1": single-sample alpha-to-coverage runs in the fragment shader with the
/// guest's dither footprint instead of the host's own pattern.
void SetShaderAlphaToCoverage(bool enabled);
[[nodiscard]] bool GetShaderAlphaToCoverage();

/// boot.cfg "small_bc_palette=1": BC1-3 textures of at most SMALL_BC_GUEST_PALETTE_BYTES decode
/// on the CPU, and every CPU decode of BC1-3 uses the guest's color interpolation
/// (VideoCommon::SetBcGuestPalette).
void SetSmallBcGuestPalette(bool enabled);
[[nodiscard]] bool GetSmallBcGuestPalette();

inline constexpr u32 SMALL_BC_GUEST_PALETTE_BYTES = 32 * 1024;

/// A texture small_bc_palette decodes on the CPU (when it is on).
[[nodiscard]] inline bool IsSmallGuestPaletteBc(VideoCore::Surface::PixelFormat format,
                                                u32 guest_bytes) noexcept {
    using VideoCore::Surface::PixelFormat;
    switch (format) {
    case PixelFormat::BC1_RGBA_UNORM:
    case PixelFormat::BC1_RGBA_SRGB:
    case PixelFormat::BC2_UNORM:
    case PixelFormat::BC2_SRGB:
    case PixelFormat::BC3_UNORM:
    case PixelFormat::BC3_SRGB:
        return guest_bytes != 0 && guest_bytes <= SMALL_BC_GUEST_PALETTE_BYTES;
    default:
        return false;
    }
}

/// boot.cfg "depth_bias_shader=1": single-sample draws with a depth bias and a fragment shader
/// add the bias in the shader (Shader::RuntimeInfo::depth_bias_in_shader), fractional constant
/// biases included, and their pipeline has none. Costs early depth tests on those draws.
void SetShaderDepthBias(bool enabled);
[[nodiscard]] bool GetShaderDepthBias();

/// The resolution of the guest depth format (Tegra::DepthFormat) the constant bias is counted in;
/// 0 for float formats, whose unit follows the depth's exponent.
[[nodiscard]] inline f32 DepthBiasUnit(u32 depth_format) noexcept {
    switch (depth_format) {
    case 0x0A: // Z32_FLOAT
    case 0x19: // Z32_FLOAT_X24S8_UINT
        return 0.0f;
    case 0x13: // Z16_UNORM
        return 0x1p-16f;
    default: // The Z24 formats
        return 0x1p-24f;
    }
}

/// Bits of each channel when the render target format (Tegra::RenderTargetFormat) is one whose
/// logic op the fragment shader can compute: all zeros for sRGB, signed, integer and float targets.
[[nodiscard]] std::array<u8, 4> LogicOpChannelBits(u32 format) noexcept;

/// Whether the packed logic op (GL value minus 0x1500) depends on the destination at all.
[[nodiscard]] constexpr bool LogicOpReadsDestination(u32 op) noexcept {
    return ((op & 1) != 0) != ((op & 2) != 0) || ((op & 4) != 0) != ((op & 8) != 0);
}

/// The render target format (Tegra::RenderTargetFormat) stores 8-bit UNORM channels.
[[nodiscard]] bool IsUnorm8RenderTarget(u32 format) noexcept;

/// What unorm_round makes the shader write: x in [0, 1] on the 8-bit step, ties down.
[[nodiscard]] inline f32 RoundUnorm8HalfDown(f32 value) noexcept {
    const f32 unorm = std::fmin(std::fmax(value, 0.0f), 1.0f);
    return std::ceil(unorm * 255.0f - 0.5f) / 255.0f;
}

/// The alpha a pixel needs to be covered under a2c_shader (footprint as in
/// Shader::RuntimeInfo::alpha_to_coverage_footprint).
[[nodiscard]] inline f32 AlphaToCoverageThreshold(u32 footprint, u32 x, u32 y) noexcept {
    if (footprint != 2) {
        return 0.5f;
    }
    constexpr f32 dither[2][2]{{0.125f, 0.625f}, {0.875f, 0.375f}};
    return dither[y & 1][x & 1];
}

/// The border color float samples read under mode. Integer views read the guest's raw bits
/// instead and never go through this.
[[nodiscard]] inline std::array<f32, 4> CorrectBorderColor(std::array<f32, 4> color,
                                                          BorderColorMode mode) noexcept {
    switch (mode) {
    case BorderColorMode::Exact:
        break;
    case BorderColorMode::Steps8:
        // Out-of-range values (float targets) and NaN stay as they are.
        for (f32& channel : color) {
            if (channel >= 0.0f && channel <= 1.0f) {
                channel = std::round(channel * 255.0f) / 255.0f;
            }
        }
        break;
    case BorderColorMode::BlackWhite: {
        if (!(color[3] >= 0.5f)) {
            return {0.0f, 0.0f, 0.0f, 0.0f};
        }
        const f32 luma = 0.299f * color[0] + 0.587f * color[1] + 0.114f * color[2];
        const f32 value = luma >= 0.5f ? 1.0f : 0.0f;
        return {value, value, value, 1.0f};
    }
    }
    return color;
}

/// Constant depth bias in Maxwell units (twice D3D12's) reduced to what the rasterizer state
/// keeps: D3D12's DepthBias is an integer, truncated toward zero.
[[nodiscard]] inline f32 EffectiveDepthBias(f32 maxwell_bias) noexcept {
    if (!std::isfinite(maxwell_bias)) {
        return maxwell_bias;
    }
    return std::trunc(maxwell_bias / 2.0f) * 2.0f;
}

/// A slope or clamp depth bias rounded to 1/16.
[[nodiscard]] inline f32 RoundDepthBiasStep(f32 value) noexcept {
    if (!std::isfinite(value)) {
        return value;
    }
    return std::round(value * 16.0f) / 16.0f;
}

} // namespace D3D12
