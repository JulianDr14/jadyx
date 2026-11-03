// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_render_corrections.h"

#include <atomic>

#include "video_core/gpu.h"
#include "video_core/texture_cache/decode_bc.h"

namespace D3D12 {

namespace {
std::atomic<BorderColorMode> border_color_mode{BorderColorMode::Exact};
std::atomic<bool> depth_bias_rounding{false};
std::atomic<bool> unorm_half_down{false};
std::atomic<bool> cube_direction_sanitize{false};
std::atomic<bool> half_float_flush{false};
std::atomic<bool> shader_alpha_to_coverage{false};
std::atomic<bool> small_bc_guest_palette{false};
std::atomic<bool> shader_depth_bias{false};
} // Anonymous namespace

void SetBorderColorMode(BorderColorMode mode) {
    border_color_mode.store(mode, std::memory_order_relaxed);
}

BorderColorMode GetBorderColorMode() {
    return border_color_mode.load(std::memory_order_relaxed);
}

void SetDepthBiasRounding(bool enabled) {
    depth_bias_rounding.store(enabled, std::memory_order_relaxed);
}

bool GetDepthBiasRounding() {
    return depth_bias_rounding.load(std::memory_order_relaxed);
}

void SetUnormHalfDownRounding(bool enabled) {
    unorm_half_down.store(enabled, std::memory_order_relaxed);
}

bool GetUnormHalfDownRounding() {
    return unorm_half_down.load(std::memory_order_relaxed);
}

void SetCubeDirectionSanitize(bool enabled) {
    cube_direction_sanitize.store(enabled, std::memory_order_relaxed);
}

bool GetCubeDirectionSanitize() {
    return cube_direction_sanitize.load(std::memory_order_relaxed);
}

void SetHalfFloatFlush(bool enabled) {
    half_float_flush.store(enabled, std::memory_order_relaxed);
}

bool GetHalfFloatFlush() {
    return half_float_flush.load(std::memory_order_relaxed);
}

void SetShaderAlphaToCoverage(bool enabled) {
    shader_alpha_to_coverage.store(enabled, std::memory_order_relaxed);
}

bool GetShaderAlphaToCoverage() {
    return shader_alpha_to_coverage.load(std::memory_order_relaxed);
}

void SetSmallBcGuestPalette(bool enabled) {
    small_bc_guest_palette.store(enabled, std::memory_order_relaxed);
    VideoCommon::SetBcGuestPalette(enabled);
}

bool GetSmallBcGuestPalette() {
    return small_bc_guest_palette.load(std::memory_order_relaxed);
}

void SetShaderDepthBias(bool enabled) {
    shader_depth_bias.store(enabled, std::memory_order_relaxed);
}

bool GetShaderDepthBias() {
    return shader_depth_bias.load(std::memory_order_relaxed);
}

std::array<u8, 4> LogicOpChannelBits(u32 format) noexcept {
    using Tegra::RenderTargetFormat;
    switch (static_cast<RenderTargetFormat>(format)) {
    case RenderTargetFormat::A8R8G8B8_UNORM:
    case RenderTargetFormat::A8B8G8R8_UNORM:
    case RenderTargetFormat::X8R8G8B8_UNORM:
    case RenderTargetFormat::X8B8G8R8_UNORM:
    case RenderTargetFormat::R8G8_UNORM:
    case RenderTargetFormat::R8_UNORM:
        return {8, 8, 8, 8};
    case RenderTargetFormat::A2B10G10R10_UNORM:
    case RenderTargetFormat::A2R10G10B10_UNORM:
        return {10, 10, 10, 2};
    case RenderTargetFormat::R16G16B16A16_UNORM:
    case RenderTargetFormat::R16G16_UNORM:
    case RenderTargetFormat::R16_UNORM:
        return {16, 16, 16, 16};
    case RenderTargetFormat::R5G6B5_UNORM:
        return {5, 6, 5, 8};
    case RenderTargetFormat::A1R5G5B5_UNORM:
    case RenderTargetFormat::X1R5G5B5_UNORM:
        return {5, 5, 5, 1};
    default:
        return {};
    }
}

bool IsUnorm8RenderTarget(u32 format) noexcept {
    using Tegra::RenderTargetFormat;
    switch (static_cast<RenderTargetFormat>(format)) {
    case RenderTargetFormat::A8R8G8B8_UNORM:
    case RenderTargetFormat::A8B8G8R8_UNORM:
    case RenderTargetFormat::X8R8G8B8_UNORM:
    case RenderTargetFormat::R8G8_UNORM:
    case RenderTargetFormat::R8_UNORM:
    case RenderTargetFormat::X8B8G8R8_UNORM:
        return true;
    default:
        return false;
    }
}

} // namespace D3D12
