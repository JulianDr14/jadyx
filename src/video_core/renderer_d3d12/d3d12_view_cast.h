// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include <dxgiformat.h>

// How a view reads or renders an image whose resource is in another typeless family. The guest
// views an image in any format of the same texel size (Vulkan's mutable format); D3D12 casts only
// inside a typeless family, and a view outside it removes the device on the Series. Pure: no
// device, no image; the texture cache applies the plan (ImageView, Image::Reinterpreted).
namespace D3D12::ViewCast {

/// A typed 8-bit RGBA format and its typeless family.
struct Typed {
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT family{DXGI_FORMAT_UNKNOWN};
};

/// The same texels with R and B swapped in the other 8-bit RGBA family, sRGB kept: a BGRA
/// resource reads as RGBA (and back) by swapping the two in the SRV's component mapping, with
/// no copy. Unknown for every other format (no BGRA integer or signed formats exist).
[[nodiscard]] constexpr Typed RedBlueTwin(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS};
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS};
    default:
        return {};
    }
}

/// What a view's descriptor is created on.
enum class Source : std::uint8_t {
    Image, ///< the image's own resource
    Copy,  ///< the image's copy in copy_family (created if missing)
    Own,   ///< not castable nor copyable: the image in its own format (wrong colors, device lives)
};

struct Plan {
    Source source{Source::Own};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}; ///< the descriptor's format; unused for Own
    DXGI_FORMAT copy_family{DXGI_FORMAT_UNKNOWN};
    bool swap_red_blue{}; ///< the SRV mapping swaps R and B (RedBlueTwin)

    friend constexpr bool operator==(const Plan&, const Plan&) = default;
};

/// What the caller knows about the view and the image.
struct Request {
    DXGI_FORMAT format;          ///< the view's typed format
    DXGI_FORMAT family;          ///< its typeless family
    DXGI_FORMAT resource_family; ///< the image resource's typeless family
    /// A copy can hold the view's texels: same texel size, 1x1 blocks, color, single-sampled.
    bool copyable;
    /// Families the image already has copies in.
    std::span<const DXGI_FORMAT> copies;
};

[[nodiscard]] constexpr bool HasCopy(const Request& request, DXGI_FORMAT family) noexcept {
    return std::ranges::find(request.copies, family) != request.copies.end();
}

/// Shader reads, cheapest first: the image itself, the image with R and B swapped, a copy the
/// image already keeps (in the view's family or its twin), a new copy in the view's family.
/// Each copy costs memory and a buffer round trip whenever the image changes, so an existing
/// twin copy wins over a new one.
[[nodiscard]] constexpr Plan PlanSrv(const Request& request) noexcept {
    if (request.family == request.resource_family) {
        return {Source::Image, request.format};
    }
    const Typed twin = RedBlueTwin(request.format);
    if (twin.format != DXGI_FORMAT_UNKNOWN && twin.family == request.resource_family) {
        return {Source::Image, twin.format, DXGI_FORMAT_UNKNOWN, true};
    }
    if (!request.copyable) {
        return {};
    }
    if (HasCopy(request, request.family)) {
        return {Source::Copy, request.format, request.family};
    }
    if (twin.format != DXGI_FORMAT_UNKNOWN && HasCopy(request, twin.family)) {
        return {Source::Copy, twin.format, twin.family, true};
    }
    return {Source::Copy, request.format, request.family};
}

/// Render targets take no swizzle: the image itself or a copy in exactly the view's family.
[[nodiscard]] constexpr Plan PlanRtv(const Request& request) noexcept {
    if (request.family == request.resource_family) {
        return {Source::Image, request.format};
    }
    if (!request.copyable) {
        return {};
    }
    return {Source::Copy, request.format, request.family};
}

} // namespace D3D12::ViewCast
