// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Clears and render target formats: each format the guest can draw into, by clear and by draw,
// readable afterwards both as bytes and through a texture.

#include "harness.h"
#include "scene_util.h"

#include <cmath>

namespace {

constexpr DkImageFormat DirectFormats[] = {
    DkImageFormat_R8_Unorm,     DkImageFormat_R8_Snorm,      DkImageFormat_R8_Uint,      DkImageFormat_R8_Sint,
    DkImageFormat_R16_Float,    DkImageFormat_R16_Unorm,     DkImageFormat_R16_Snorm,    DkImageFormat_R16_Uint,
    DkImageFormat_R16_Sint,     DkImageFormat_R32_Float,     DkImageFormat_R32_Uint,     DkImageFormat_R32_Sint,
    DkImageFormat_RG8_Unorm,    DkImageFormat_RG8_Snorm,     DkImageFormat_RG8_Uint,     DkImageFormat_RG8_Sint,
    DkImageFormat_RG16_Float,   DkImageFormat_RG16_Unorm,    DkImageFormat_RG16_Snorm,   DkImageFormat_RG16_Uint,
    DkImageFormat_RG16_Sint,    DkImageFormat_RG32_Float,    DkImageFormat_RG32_Uint,    DkImageFormat_RG32_Sint,
    DkImageFormat_RGBA8_Unorm,  DkImageFormat_RGBA8_Snorm,   DkImageFormat_RGBA8_Uint,   DkImageFormat_RGBA8_Sint,
    DkImageFormat_RGBA16_Float, DkImageFormat_RGBA16_Unorm,  DkImageFormat_RGBA16_Snorm, DkImageFormat_RGBA16_Uint,
    DkImageFormat_RGBA16_Sint,  DkImageFormat_RGBA32_Float,  DkImageFormat_RGBA32_Uint,  DkImageFormat_RGBA32_Sint,
};

// Only the packed formats deko3d can render to: RGB565, RGB5A1, RGBA4 and E5BGR9 cannot be targets.
constexpr DkImageFormat PackedFormats[] = {
    DkImageFormat_BGRA8_Unorm,    DkImageFormat_BGRA8_Unorm_sRGB, DkImageFormat_RGB10A2_Unorm,
    DkImageFormat_RG11B10_Float,  DkImageFormat_BGR565_Unorm,     DkImageFormat_BGR5A1_Unorm,
    DkImageFormat_RGBX8_Unorm,    DkImageFormat_BGRX8_Unorm,
    DkImageFormat_RGBA8_Unorm_sRGB,
};

// The missing channels of a smaller format read as 0, 0, 1.
Color Keep(const FormatInfo& info, Color color) {
    if (info.channels < 2) color.g = 0;
    if (info.channels < 3) color.b = 0;
    if (info.channels < 4) color.a = 1;
    return color;
}

double ChannelTolerance(const FormatInfo& info) {
    if (info.kind == Kind::Float) return 0.002;
    if (info.kind == Kind::Uint || info.kind == Kind::Sint) return 0.0;
    return info.bits == 8 ? 1.5 / 255.0 : 0.001;
}

Color FloatValue(const FormatInfo& info) {
    switch (info.kind) {
    case Kind::Unorm:
        return {0.25, 0.5, 0.75, 1.0};
    case Kind::Snorm:
        return {-0.5, 0.25, 0.75, 1.0};
    default:
        return {0.25, -0.5, 2.5, 1.0};
    }
}

void ProbeAll(Gpu& gpu, const Color& color, double tolerance) {
    for (uint32_t y : {1u, 16u, 30u}) {
        for (uint32_t x : {1u, 16u, 30u}) {
            gpu.Expect(x, y, color, tolerance);
        }
    }
}

void ClearFormat(Gpu& gpu, DkImageFormat format) {
    const FormatInfo& info = InfoOf(format);
    gpu.Begin(format);
    if (info.kind == Kind::Uint) {
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_RGBA, 12, 34, 56, 78);
        gpu.Finish();
        ProbeAll(gpu, Keep(info, {12, 34, 56, 78}), 0.0);
    } else if (info.kind == Kind::Sint) {
        gpu.cmd.clearColor<int32_t>(0, DkColorMask_RGBA, -12, 34, -56, 78);
        gpu.Finish();
        ProbeAll(gpu, Keep(info, {-12, 34, -56, 78}), 0.0);
    } else {
        const Color color = FloatValue(info);
        gpu.ClearColor(color);
        gpu.Finish();
        ProbeAll(gpu, Keep(info, color), ChannelTolerance(info));
    }
}

void DrawFormat(Gpu& gpu, DkImageFormat format) {
    const FormatInfo& info = InfoOf(format);
    gpu.Begin(format);
    if (info.kind == Kind::Uint) {
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_RGBA, 0, 0, 0, 0);
        const uint32_t value[4] = {21, 43, 65, 87};
        gpu.Shaders("basic_vsh", "uint_fsh");
        gpu.Uniform(DkStage_Fragment, 0, value, sizeof(value));
        gpu.Rect(8, 8, 24, 24, White);
        gpu.Finish();
        gpu.Expect(16, 16, Keep(info, {21, 43, 65, 87}), 0.0);
        gpu.Expect(2, 2, Keep(info, {0, 0, 0, 0}), 0.0);
    } else if (info.kind == Kind::Sint) {
        gpu.cmd.clearColor<int32_t>(0, DkColorMask_RGBA, 0, 0, 0, 0);
        const int32_t value[4] = {-21, 43, -65, 87};
        gpu.Shaders("basic_vsh", "sint_fsh");
        gpu.Uniform(DkStage_Fragment, 0, value, sizeof(value));
        gpu.Rect(8, 8, 24, 24, White);
        gpu.Finish();
        gpu.Expect(16, 16, Keep(info, {-21, 43, -65, 87}), 0.0);
        gpu.Expect(2, 2, Keep(info, {0, 0, 0, 0}), 0.0);
    } else {
        gpu.ClearColor({0, 0, 0, 0});
        const Color color = FloatValue(info);
        gpu.Rect(8, 8, 24, 24, color);
        gpu.Finish();
        gpu.Expect(16, 16, Keep(info, color), ChannelTolerance(info));
        gpu.Expect(2, 2, Keep(info, {0, 0, 0, 0}), ChannelTolerance(info));
    }
}

// The format draws and samples back; the format's own byte layout stays out of the scene's way.
void RoundTrip(Gpu& gpu, DkImageFormat format, double tolerance) {
    Image& target = gpu.Begin(format);
    gpu.ClearColor({0, 0, 0, 1});
    gpu.Rect(0, 0, 16, 32, {0.5, 0.25, 0.75, 1.0});
    gpu.Rect(16, 0, 32, 32, Red);
    gpu.Barrier();
    Image& result = gpu.Begin();
    dk::Sampler sampler;
    gpu.Shaders("basic_vsh", "texture_fsh");
    gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(target, sampler));
    gpu.Full(White);
    gpu.Finish(&result);
    gpu.Expect(8, 16, {0.5, 0.25, 0.75, 1.0}, tolerance);
    gpu.Expect(24, 16, Red, tolerance);
}

} // namespace

void RegisterClears(std::vector<SceneEntry>& scenes) {
    for (DkImageFormat format : DirectFormats) {
        const char* name = InfoOf(format).name;
        scenes.push_back({Named("clear_", name), [format](Gpu& gpu) { ClearFormat(gpu, format); }});
        scenes.push_back({Named("draw_", name), [format](Gpu& gpu) { DrawFormat(gpu, format); }});
    }
    for (DkImageFormat format : PackedFormats) {
        double tolerance = 2.5 / 255.0;
        switch (format) {
        case DkImageFormat_RGBA4_Unorm:
            tolerance = 0.5 / 15.0 + 0.002;
            break;
        case DkImageFormat_RGB565_Unorm:
        case DkImageFormat_BGR565_Unorm:
        case DkImageFormat_RGB5A1_Unorm:
        case DkImageFormat_BGR5A1_Unorm:
            tolerance = 0.5 / 31.0 + 0.002;
            break;
        case DkImageFormat_RG11B10_Float:
        case DkImageFormat_E5BGR9_Float:
            tolerance = 0.02;
            break;
        default:
            break;
        }
        scenes.push_back({Named("roundtrip_", InfoOf(format).name),
                          [format, tolerance](Gpu& gpu) { RoundTrip(gpu, format, tolerance); }});
    }

    scenes.push_back({"clear_mask", [](Gpu& gpu) {
        for (uint32_t mask = 0; mask < 16; ++mask) {
            gpu.Begin();
            gpu.ClearColor({0.25, 0.25, 0.25, 0.25});
            gpu.cmd.clearColor<float>(0, mask, 0.75f, 0.75f, 0.75f, 0.75f);
            gpu.Finish();
            const auto pick = [&](uint32_t bit) { return (mask & bit) ? 0.75 : 0.25; };
            gpu.Expect(16, 16, {pick(DkColorMask_R), pick(DkColorMask_G), pick(DkColorMask_B), pick(DkColorMask_A)});
        }
    }});
    // Values a 32-bit float cannot hold, which D3D12's own clear goes through.
    scenes.push_back({"clear_uint32_large", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA32_Uint);
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_RGBA, 0xFFFFFFFF, 16777217, 0x80000001, 5);
        gpu.Finish();
        gpu.ExpectUint(16, 16, 0xFFFFFFFF, 16777217, 0x80000001, 5);
    }});
    scenes.push_back({"clear_sint32_large", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA32_Sint);
        gpu.cmd.clearColor<int32_t>(0, DkColorMask_RGBA, -2147483647, 16777217, -16777217, 7);
        gpu.Finish();
        gpu.Expect(16, 16, {-2147483647.0, 16777217.0, -16777217.0, 7.0}, 0.0);
    }});
    scenes.push_back({"clear_uint_masked", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA8_Uint);
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_RGBA, 1, 2, 3, 4);
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_R | DkColorMask_B, 200, 201, 250, 251);
        gpu.Finish();
        gpu.ExpectUint(16, 16, 200, 2, 250, 4);
    }});
    scenes.push_back({"clear_uint32_large_masked", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA32_Uint);
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_RGBA, 1, 2, 3, 4);
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_G | DkColorMask_A, 0, 0xFFFFFFFF, 0, 16777217);
        gpu.Finish();
        gpu.ExpectUint(16, 16, 1, 0xFFFFFFFF, 3, 16777217);
    }});
    scenes.push_back({"clear_sint_masked", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA16_Sint);
        gpu.cmd.clearColor<int32_t>(0, DkColorMask_RGBA, -1, -2, -3, -4);
        gpu.cmd.clearColor<int32_t>(0, DkColorMask_G, 0, -3000, 0, 0);
        gpu.Finish();
        gpu.Expect(16, 16, {-1.0, -3000.0, -3.0, -4.0}, 0.0);
    }});
    scenes.push_back({"clear_scissored", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Blue);
        gpu.cmd.setScissors(0, {{8, 8, 16, 16}});
        gpu.ClearColor(Green);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
        gpu.Expect(4, 4, Blue);
        gpu.Expect(25, 25, Blue);
    }});
    scenes.push_back({"clear_then_draw_then_clear", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Red);
        gpu.Rect(0, 0, 32, 32, Green);
        gpu.ClearColor(Blue);
        gpu.Rect(8, 8, 16, 16, White);
        gpu.Finish();
        gpu.Expect(12, 12, White);
        gpu.Expect(24, 24, Blue);
    }});
    scenes.push_back({"clear_two_targets", [](Gpu& gpu) {
        Image& first = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        Image& second = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        gpu.UseTarget(first);
        dk::ImageView view_first{first.image}, view_second{second.image};
        gpu.cmd.bindRenderTargets({&view_first, &view_second});
        gpu.ClearColor(Red, 0);
        gpu.ClearColor(Green, 1);
        gpu.Finish(&first);
        gpu.Expect(16, 16, Red);
        gpu.Finish(&second);
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"size_not_power_of_two", [](Gpu& gpu) {
        Image& target = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 33, 17);
        gpu.UseTarget(target);
        gpu.ClearColor(Blue);
        gpu.Rect(11, 5, 33, 17, Green);
        gpu.Finish();
        gpu.Expect(32, 16, Green);
        gpu.Expect(10, 16, Blue);
        gpu.Expect(32, 4, Blue);
    }});
    scenes.push_back({"size_large_target", [](Gpu& gpu) {
        Image& target = gpu.Begin(DkImageFormat_RGBA8_Unorm, 512);
        (void)target;
        gpu.ClearColor(Blue);
        gpu.Rect(256, 256, 512, 512, Green);
        gpu.Finish();
        gpu.Expect(511, 511, Green);
        gpu.Expect(255, 255, Blue);
    }});
    // The same memory first as a float target, then as RGBA8: the second read must be RGBA8 bytes,
    // not what the texture cache still holds for the first image there.
    scenes.push_back({"format_alias_reuse", [](Gpu& gpu) {
        Image& first = gpu.Begin(DkImageFormat_R32_Float);
        gpu.ClearColor({0.25, 0, 0, 1});
        gpu.Finish();
        gpu.Expect(16, 16, {0.25, 0, 0, 1}, 0.0);
        Image& second = gpu.Alias(first, DkImageFormat_RGBA8_Unorm);
        gpu.UseTarget(second);
        gpu.ClearColor({0.25, 0.5, 0.75, 1.0});
        gpu.Rect(8, 8, 24, 24, Red);
        gpu.Finish();
        gpu.Expect(4, 4, {0.25, 0.5, 0.75, 1.0});
        gpu.Expect(16, 16, Red);
    }});
    scenes.push_back({"target_survives_rebind", [](Gpu& gpu) {
        Image& first = gpu.Begin();
        gpu.ClearColor(Red);
        Image& second = gpu.Begin();
        gpu.ClearColor(Blue);
        gpu.UseTarget(first);
        gpu.Rect(0, 0, 16, 32, Green);
        gpu.Finish(&first);
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Red);
        gpu.Finish(&second);
        gpu.Expect(16, 16, Blue);
    }});
}
