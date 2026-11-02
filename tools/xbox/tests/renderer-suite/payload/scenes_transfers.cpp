// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Blits, copies between images and buffers, and what the GPU reports back into memory; then the two
// scenes on the OpenGL conventions, which need their own device.

#include "harness.h"
#include "scene_util.h"

namespace {

// A 4x4 image where each texel says where it is.
Image& Pattern(Gpu& gpu, uint32_t flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine) {
    Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 4, 4, 1, 1, DkImageType_2D, flags);
    uint8_t texels[4 * 4 * 4];
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint8_t* texel = &texels[(y * 4 + x) * 4];
            texel[0] = static_cast<uint8_t>((x + 1) * 50);
            texel[1] = static_cast<uint8_t>((y + 1) * 50);
            texel[2] = 0;
            texel[3] = 255;
        }
    }
    gpu.Upload(image, texels, sizeof(texels), 4, 4);
    return image;
}

Color PatternColor(uint32_t x, uint32_t y) {
    return {(x + 1) * 50 / 255.0, (y + 1) * 50 / 255.0, 0, 1};
}

Image& Destination(Gpu& gpu, DkImageFormat format = DkImageFormat_RGBA8_Unorm, uint32_t size = TargetSize) {
    Image& image = gpu.Begin(format, size);
    gpu.ClearColor(Black);
    gpu.Barrier();
    return image;
}

} // namespace

void RegisterTransfers(std::vector<SceneEntry>& scenes) {
    scenes.push_back({"blit_stretch_nearest", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 4, 4, 1}, to, {0, 0, 0, 32, 32, 1}, DkBlitFlag_FilterNearest);
        gpu.Finish();
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                gpu.Expect(x * 8 + 4, y * 8 + 4, PatternColor(x, y));
            }
        }
    }});
    scenes.push_back({"blit_stretch_linear", [](Gpu& gpu) {
        Image& source = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 2, 1);
        const uint8_t texels[8] = {0, 0, 0, 255, 255, 255, 255, 255};
        gpu.Upload(source, texels, sizeof(texels), 2, 1);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 2, 1, 1}, to, {0, 0, 0, 32, 32, 1}, DkBlitFlag_FilterLinear);
        gpu.Finish();
        // Black at the first texel's center, white at the second's and a ramp between them. deko3d
        // gives the engine a surface as wide as the GOB (16 texels), so past the second's center
        // the filter blends in texels of the surface that the image does not have.
        gpu.Check("left", gpu.Read(8, 16).r, 0.0, 0.05);
        gpu.Check("middle", gpu.Read(16, 16).r, 0.5, 0.1);
        gpu.Check("right", gpu.Read(24, 16).r, 1.0, 0.05);
    }});
    scenes.push_back({"blit_flip_x", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 4, 4, 1}, to, {0, 0, 0, 32, 32, 1},
                          DkBlitFlag_FilterNearest | DkBlitFlag_FlipX);
        gpu.Finish();
        gpu.Expect(4, 4, PatternColor(3, 0));
        gpu.Expect(28, 4, PatternColor(0, 0));
    }});
    scenes.push_back({"blit_flip_y", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 4, 4, 1}, to, {0, 0, 0, 32, 32, 1},
                          DkBlitFlag_FilterNearest | DkBlitFlag_FlipY);
        gpu.Finish();
        gpu.Expect(4, 4, PatternColor(0, 3));
        gpu.Expect(4, 28, PatternColor(0, 0));
    }});
    scenes.push_back({"blit_subrect", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {2, 2, 0, 2, 2, 1}, to, {8, 8, 0, 16, 16, 1}, DkBlitFlag_FilterNearest);
        gpu.Finish();
        gpu.Expect(4, 4, Black);
        gpu.Expect(10, 10, PatternColor(2, 2));
        gpu.Expect(22, 22, PatternColor(3, 3));
        gpu.Expect(26, 26, Black);
    }});
    scenes.push_back({"blit_format_conversion", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu, DkImageFormat_RGBA16_Float);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 4, 4, 1}, to, {0, 0, 0, 32, 32, 1}, DkBlitFlag_FilterNearest);
        gpu.Finish();
        gpu.Expect(12, 20, PatternColor(1, 2), 0.01);
    }});
    // Both directions of the conversion games use around their display buffers.
    scenes.push_back({"blit_srgb_to_linear", [](Gpu& gpu) {
        Image& source = gpu.MakeImage(DkImageFormat_RGBA8_Unorm_sRGB, 1, 1);
        const uint8_t texel[4] = {128, 128, 128, 255};
        gpu.Upload(source, texel, 4, 1, 1);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 1, 1, 1}, to, {0, 0, 0, 32, 32, 1}, DkBlitFlag_FilterNearest);
        gpu.Finish();
        const double linear = SrgbToLinear(128.0 / 255.0);
        gpu.Expect(16, 16, {linear, linear, linear, 1}, 0.02);
    }});
    scenes.push_back({"blit_bgra_to_rgba", [](Gpu& gpu) {
        Image& source = gpu.MakeImage(DkImageFormat_BGRA8_Unorm, 1, 1);
        const uint8_t texel[4] = {255, 0, 0, 255}; // Blue, as BGRA.
        gpu.Upload(source, texel, 4, 1, 1);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 1, 1, 1}, to, {0, 0, 0, 32, 32, 1}, DkBlitFlag_FilterNearest);
        gpu.Finish();
        gpu.Expect(16, 16, Blue);
    }});

    scenes.push_back({"copy_image_same_size", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu, DkImageFormat_RGBA8_Unorm, 4);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.copyImage(from, {0, 0, 0, 4, 4, 1}, to, {0, 0, 0, 4, 4, 1});
        gpu.Finish();
        gpu.Expect(1, 2, PatternColor(1, 2));
        gpu.Expect(3, 3, PatternColor(3, 3));
    }});
    scenes.push_back({"copy_image_subrect", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Image& target = Destination(gpu);
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.copyImage(from, {1, 1, 0, 2, 2, 1}, to, {10, 20, 0, 2, 2, 1});
        gpu.Finish();
        gpu.Expect(10, 20, PatternColor(1, 1));
        gpu.Expect(11, 21, PatternColor(2, 2));
        gpu.Expect(9, 20, Black);
        gpu.Expect(12, 21, Black);
    }});
    scenes.push_back({"copy_buffer", [](Gpu& gpu) {
        Slice source = gpu.Alloc(64), target = gpu.Alloc(64);
        for (uint32_t i = 0; i < 16; ++i) {
            static_cast<uint32_t*>(source.cpu)[i] = i * 3 + 1;
            static_cast<uint32_t*>(target.cpu)[i] = 0;
        }
        gpu.cmd.copyBuffer(source.gpu, target.gpu, 64);
        gpu.Submit();
        for (uint32_t i = 0; i < 16; ++i) {
            gpu.Check("word", static_cast<volatile uint32_t*>(target.cpu)[i], i * 3 + 1);
        }
    }});
    scenes.push_back({"copy_buffer_to_image_row_length", [](Gpu& gpu) {
        Image& target = Destination(gpu, DkImageFormat_RGBA8_Unorm, 4);
        // Rows 32 bytes apart, of which each one's texels are the first 16.
        Slice staging = gpu.Alloc(32 * 4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        std::memset(staging.cpu, 0, staging.size);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint8_t texel[4] = {static_cast<uint8_t>((x + 1) * 50), static_cast<uint8_t>((y + 1) * 50), 0, 255};
                std::memcpy(static_cast<uint8_t*>(staging.cpu) + y * 32 + x * 4, texel, 4);
            }
        }
        dk::ImageView view{target.image};
        gpu.cmd.copyBufferToImage({staging.gpu, 32, 0}, view, {0, 0, 0, 4, 4, 1});
        gpu.Finish();
        gpu.Expect(0, 0, PatternColor(0, 0));
        gpu.Expect(3, 2, PatternColor(3, 2));
    }});
    scenes.push_back({"copy_image_to_buffer_subrect", [](Gpu& gpu) {
        Image& source = Pattern(gpu);
        Slice out = gpu.Alloc(16, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        std::memset(out.cpu, 0, out.size);
        dk::ImageView view{source.image};
        gpu.cmd.copyImageToBuffer(view, {2, 1, 0, 2, 2, 1}, {out.gpu, 0, 0});
        gpu.cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        gpu.Submit();
        const auto* bytes = static_cast<const volatile uint8_t*>(out.cpu);
        gpu.Check("first red", bytes[0], 150);
        gpu.Check("first green", bytes[1], 100);
        gpu.Check("last red", bytes[12], 200);
        gpu.Check("last green", bytes[13], 150);
    }});
    // Whatever the GPU wrote to memory reaches the CPU, and what the CPU writes reaches a later texture.
    scenes.push_back({"copy_image_to_buffer_after_draw", [](Gpu& gpu) {
        Image& target = gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 16, 32, Green);
        gpu.Barrier();
        Slice out = gpu.Alloc(32 * 32 * 4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        dk::ImageView view{target.image};
        gpu.cmd.copyImageToBuffer(view, {0, 0, 0, 32, 32, 1}, {out.gpu, 0, 0});
        gpu.cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        gpu.Submit();
        const auto* bytes = static_cast<const volatile uint8_t*>(out.cpu);
        gpu.Check("drawn", bytes[(16 * 32 + 8) * 4 + 1], 255);
        gpu.Check("cleared", bytes[(16 * 32 + 24) * 4 + 1], 0);
    }});
    scenes.push_back({"texture_replaced_by_upload", [](Gpu& gpu) {
        Image& source = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1);
        const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
        gpu.Upload(source, red, 4, 1, 1);
        gpu.Begin();
        dk::Sampler sampler;
        gpu.Shaders("basic_vsh", "texture_fsh");
        const DkResHandle handle = gpu.Texture(source, sampler);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, handle);
        gpu.Rect(0, 0, 16, 32, White);
        gpu.Submit();
        gpu.Upload(source, green, 4, 1, 1);
        gpu.Rect(16, 0, 32, 32, White);
        gpu.Finish();
        gpu.Expect(8, 16, Red);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"push_data", [](Gpu& gpu) {
        Slice target = gpu.Alloc(16);
        std::memset(target.cpu, 0, 16);
        const uint32_t words[4] = {11, 22, 33, 44};
        gpu.cmd.pushData(target.gpu, words, sizeof(words));
        gpu.cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        gpu.Submit();
        for (uint32_t i = 0; i < 4; ++i) {
            gpu.Check("word", static_cast<volatile uint32_t*>(target.cpu)[i], words[i]);
        }
    }});
    scenes.push_back({"report_value", [](Gpu& gpu) {
        Slice target = gpu.Alloc(16);
        std::memset(target.cpu, 0, 16);
        gpu.cmd.reportValue(0x12345678, target.gpu);
        gpu.Submit();
        gpu.Check("value", *static_cast<volatile uint32_t*>(target.cpu), 0x12345678);
    }});
    scenes.push_back({"report_samples_passed", [](Gpu& gpu) {
        Slice counters = gpu.Alloc(32, 16);
        std::memset(counters.cpu, 0, 32);
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.resetCounter(DkCounter_SamplesPassed);
        gpu.Rect(0, 0, 8, 8, Green);
        gpu.cmd.reportCounter(DkCounter_SamplesPassed, counters.gpu);
        gpu.Rect(8, 8, 16, 12, Green);
        gpu.cmd.reportCounter(DkCounter_SamplesPassed, counters.gpu + 16);
        gpu.Submit();
        const auto* values = static_cast<const volatile uint64_t*>(counters.cpu);
        gpu.Check("after the first draw", static_cast<double>(values[0]), 64);
        gpu.Check("after the second draw", static_cast<double>(values[2]), 64 + 32);
    }});
    scenes.push_back({"report_primitives", [](Gpu& gpu) {
        Slice counters = gpu.Alloc(16, 16);
        std::memset(counters.cpu, 0, 16);
        gpu.Begin();
        gpu.cmd.resetCounter(DkCounter_InputPrimitives);
        gpu.Full(Green);
        gpu.cmd.reportCounter(DkCounter_InputPrimitives, counters.gpu);
        gpu.Submit();
        gpu.Check("primitives", static_cast<double>(*static_cast<const volatile uint64_t*>(counters.cpu)), 2);
    }});
    scenes.push_back({"report_timestamp_advances", [](Gpu& gpu) {
        Slice counters = gpu.Alloc(32, 16);
        std::memset(counters.cpu, 0, 32);
        gpu.cmd.reportCounter(DkCounter_Timestamp, counters.gpu);
        gpu.Submit();
        svcSleepThread(2'000'000);
        gpu.cmd.reportCounter(DkCounter_Timestamp, counters.gpu + 16);
        gpu.Submit();
        // A report is {value, timestamp}: the timestamp is the second word.
        const auto* values = static_cast<const volatile uint64_t*>(counters.cpu);
        if (values[3] <= values[1]) {
            gpu.Fail("timestamps %llu then %llu", static_cast<unsigned long long>(values[1]),
                     static_cast<unsigned long long>(values[3]));
        }
    }});
}

// The conventions OpenGL titles use: the bottom row of the image is y = 0 and clip z runs -1..1. The
// window y is the row in memory, so clip-space top lands on the last row while scissors and
// gl_FragCoord count rows from the first.
void RegisterGlConventions(std::vector<SceneEntry>& scenes) {
    scenes.push_back({"gl_clip_top_is_last_row", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 32, 8, Green); // The clip-space top.
        gpu.Finish();
        gpu.Expect(16, 28, Green);
        gpu.Expect(16, 4, Black);
    }});
    scenes.push_back({"gl_scissor_counts_from_bottom", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setScissors(0, {{0, 0, 32, 8}});
        gpu.Full(Green);
        gpu.Finish();
        gpu.Expect(16, 4, Green);
        gpu.Expect(16, 28, Black);
    }});
    scenes.push_back({"gl_depth_minus_one_is_zero", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_ZF32);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.25f, 0, 0);
        dk::DepthStencilState depth;
        depth.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthCompareOp(DkCompareOp_Less);
        gpu.cmd.bindDepthStencilState(depth);
        gpu.Rect(0, 0, 16, 32, Green, -1.0f); // Depth 0: under the 0.25.
        gpu.Rect(16, 0, 32, 32, Red, 0.0f);   // Depth 0.5: over it.
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"gl_front_face_is_mirrored", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Shaders("basic_vsh", "front_facing_fsh");
        gpu.Rect(0, 0, 32, 32, White);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"gl_frag_coord_y", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA16_Unorm);
        gpu.Shaders("basic_vsh", "frag_coord_fsh");
        gpu.Full(White, 0.0f);
        gpu.Finish();
        // Clip z 0 lands on depth 0.5; row 4 in memory is window y 4.5.
        gpu.Expect(4, 4, {4.5 / 32.0, 4.5 / 32.0, 0.5, 1.0}, 0.002);
    }});
}
