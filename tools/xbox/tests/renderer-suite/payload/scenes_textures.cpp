// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Sampling: every compressed family decoded from hand-built blocks, image types, swizzles, wrap
// modes, filters, mips, views, multisampling and rendering into parts of an image.

#include "harness.h"
#include "scene_util.h"

#include <cmath>

namespace {

constexpr uint32_t Sampled = 0; // Block linear, not renderable: what compressed formats need.

// Little-endian bit writer for the formats whose fields don't fall on bytes.
struct Block {
    uint8_t bytes[16]{};
    void Set(uint32_t bit, uint32_t count, uint32_t value) {
        for (uint32_t i = 0; i < count; ++i) {
            if ((value >> i) & 1) {
                bytes[(bit + i) / 8] |= static_cast<uint8_t>(1u << ((bit + i) % 8));
            }
        }
    }
};

dk::Sampler Nearest() {
    dk::Sampler sampler;
    sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_None);
    sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
    return sampler;
}

dk::Sampler Linear() {
    dk::Sampler sampler;
    sampler.setFilter(DkFilter_Linear, DkFilter_Linear, DkMipFilter_None);
    sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
    return sampler;
}

// Draws `image` over the whole of a new target, unfiltered.
void Show(Gpu& gpu, const dk::ImageView& view, DkImageFormat target = DkImageFormat_RGBA16_Float,
          const char* shader = "texture_fsh") {
    Image& result = gpu.Begin(target);
    gpu.Shaders("basic_vsh", shader);
    gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(view, Nearest()));
    gpu.Full(White);
    gpu.Finish(&result);
}

void Show(Gpu& gpu, const Image& image, DkImageFormat target = DkImageFormat_RGBA16_Float) {
    dk::ImageView view{image.image};
    Show(gpu, view, target);
}

// A 4x4 compressed image of one block, with each of its rows probed.
template <size_t Bytes>
void BlockRows(Gpu& gpu, DkImageFormat format, const uint8_t (&block)[Bytes], const Color (&rows)[4],
               double tolerance = 0.02) {
    Image& image = gpu.MakeImage(format, 4, 4, 1, 1, DkImageType_2D, Sampled);
    gpu.Upload(image, block, Bytes, 4, 4);
    Show(gpu, image);
    for (uint32_t y = 0; y < 4; ++y) {
        gpu.Expect(16, y * 8 + 4, rows[y], tolerance);
    }
}

void FillAlphaIndices(uint8_t* block) {
    // Rows of 3-bit indices 0, 1, 2 and 7.
    Block bits;
    constexpr uint32_t Rows[4] = {0, 1, 2, 7};
    for (uint32_t i = 0; i < 16; ++i) {
        bits.Set(i * 3, 3, Rows[i / 4]);
    }
    std::memcpy(block, bits.bytes, 6);
}

struct AstcSize {
    const char* name;
    DkImageFormat format;
    DkImageFormat srgb;
    uint32_t w, h;
};

#define ASTC(W, H)                                                                                                     \
    AstcSize {                                                                                                         \
        #W "x" #H, DkImageFormat_RGBA_ASTC_##W##x##H, DkImageFormat_RGBA_ASTC_##W##x##H##_sRGB, W, H                   \
    }

const AstcSize AstcSizes[] = {ASTC(4, 4),  ASTC(5, 4),  ASTC(5, 5),  ASTC(6, 5),   ASTC(6, 6),
                              ASTC(8, 5),  ASTC(8, 6),  ASTC(8, 8),  ASTC(10, 5),  ASTC(10, 6),
                              ASTC(10, 8), ASTC(10, 10), ASTC(12, 10), ASTC(12, 12)};

// A constant-color image in void-extent blocks: the one ASTC encoding that fits a hand-built table.
void AstcConstant(Gpu& gpu, DkImageFormat format, uint32_t block_w, uint32_t block_h, const Color& expected) {
    constexpr uint32_t Size = 24;
    Image& image = gpu.MakeImage(format, Size, Size, 1, 1, DkImageType_2D, Sampled);
    const uint32_t blocks = ((Size + block_w - 1) / block_w) * ((Size + block_h - 1) / block_h);
    constexpr uint8_t Void[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF};
    std::vector<uint8_t> data(blocks * 16);
    for (uint32_t i = 0; i < blocks; ++i) {
        std::memcpy(&data[i * 16], Void, 16);
    }
    gpu.Upload(image, data.data(), static_cast<uint32_t>(data.size()), Size, Size);
    Show(gpu, image);
    gpu.Expect(4, 4, expected, 0.02);
    gpu.Expect(28, 28, expected, 0.02);
}

// 4x4, where each texel says where it is, for the probes that care about texel placement.
Image& Pattern(Gpu& gpu) {
    Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 4, 4);
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

// Three levels, red, green and blue, each as big as a mip chain from `size` makes it.
Image& MipChain(Gpu& gpu, uint32_t size) {
    Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, size, size, 1, 3);
    constexpr uint8_t Colors[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    for (uint32_t level = 0; level < 3; ++level) {
        const uint32_t s = size >> level;
        std::vector<uint8_t> texels(s * s * 4);
        for (uint32_t i = 0; i < s * s; ++i) {
            std::memcpy(&texels[i * 4], Colors[level], 4);
        }
        gpu.Upload(image, texels.data(), static_cast<uint32_t>(texels.size()), s, s, level);
    }
    return image;
}

// Two texels, left red and right green, with u running -1..2 over the target.
struct WrapCase {
    const char* name;
    DkWrapMode mode;
};

constexpr WrapCase WrapModes[] = {
    {"repeat", DkWrapMode_Repeat},
    {"mirrored_repeat", DkWrapMode_MirroredRepeat},
    {"clamp_to_edge", DkWrapMode_ClampToEdge},
    {"clamp_to_border", DkWrapMode_ClampToBorder},
    {"mirror_clamp_to_edge", DkWrapMode_MirrorClampToEdge},
    {"mirror_clamp_to_border", DkWrapMode_MirrorClampToBorder},
};

Color WrapExpected(DkWrapMode mode, double u) {
    double at = u;
    switch (mode) {
    case DkWrapMode_Repeat:
        at = u - std::floor(u);
        break;
    case DkWrapMode_MirroredRepeat: {
        const double m = u - 2.0 * std::floor(u / 2.0);
        at = m > 1.0 ? 2.0 - m : m;
        break;
    }
    case DkWrapMode_ClampToEdge:
        at = std::fmin(std::fmax(u, 0.0), 1.0);
        break;
    case DkWrapMode_ClampToBorder:
        if (u < 0.0 || u > 1.0) return Blue;
        break;
    case DkWrapMode_MirrorClampToEdge:
        at = std::fmin(std::fabs(u), 1.0);
        break;
    default:
        at = std::fabs(u);
        if (at > 1.0) return Blue;
        break;
    }
    return at < 0.5 ? Red : Green;
}

Image& TwoTexels(Gpu& gpu) {
    Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 2, 1);
    const uint8_t texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    gpu.Upload(image, texels, sizeof(texels), 2, 1);
    return image;
}

// Whole-target multisampled image bound with its state on.
Image& MsTarget(Gpu& gpu, DkImageFormat format = DkImageFormat_RGBA8_Unorm) {
    Image& target = gpu.MakeImage(format, TargetSize, TargetSize, 1, 1, DkImageType_2DMS,
                                  DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, DkMsMode_4x);
    gpu.UseTarget(target);
    dk::MultisampleState state;
    state.setMode(DkMsMode_4x);
    gpu.cmd.bindMultisampleState(state);
    return target;
}

// Down to one sample per pixel and read.
void Resolve(Gpu& gpu, Image& target) {
    gpu.Barrier();
    Image& result = gpu.MakeImage(target.format, TargetSize, TargetSize);
    dk::ImageView from{target.image}, to{result.image};
    gpu.cmd.resolveImage(from, to);
    gpu.Finish(&result);
}

} // namespace

void RegisterTextures(std::vector<SceneEntry>& scenes) {
    // BC: rows of indices 0, 1, 2 and 3 over two endpoints.
    scenes.push_back({"bc1_rgb_four_colors", [](Gpu& gpu) {
        constexpr uint8_t Data[8] = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0xAA, 0xFF};
        constexpr Color Rows[4] = {Red, Blue, {2.0 / 3.0, 0, 1.0 / 3.0, 1}, {1.0 / 3.0, 0, 2.0 / 3.0, 1}};
        BlockRows(gpu, DkImageFormat_RGB_BC1, Data, Rows);
    }});
    scenes.push_back({"bc1_rgba_punch_through", [](Gpu& gpu) {
        constexpr uint8_t Data[8] = {0x1F, 0x00, 0x00, 0xF8, 0x00, 0x55, 0xAA, 0xFF};
        constexpr Color Rows[4] = {Blue, Red, {0.5, 0, 0.5, 1}, {0, 0, 0, 0}};
        BlockRows(gpu, DkImageFormat_RGBA_BC1, Data, Rows);
    }});
    scenes.push_back({"bc2_explicit_alpha", [](Gpu& gpu) {
        constexpr uint8_t Data[16] = {0x00, 0x00, 0xFF, 0xFF, 0x88, 0x88, 0x33, 0x33,
                                      0xE0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        constexpr Color Rows[4] = {{0, 1, 0, 0}, {0, 1, 0, 1}, {0, 1, 0, 8.0 / 15.0}, {0, 1, 0, 3.0 / 15.0}};
        BlockRows(gpu, DkImageFormat_RGBA_BC2, Data, Rows);
    }});
    scenes.push_back({"bc3_interpolated_alpha", [](Gpu& gpu) {
        uint8_t data[16] = {0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        FillAlphaIndices(&data[2]);
        constexpr Color Rows[4] = {{0, 1, 0, 1}, {0, 1, 0, 0}, {0, 1, 0, 6.0 / 7.0}, {0, 1, 0, 1.0 / 7.0}};
        BlockRows(gpu, DkImageFormat_RGBA_BC3, data, Rows);
    }});
    scenes.push_back({"bc4_unorm", [](Gpu& gpu) {
        uint8_t data[8] = {0xFF, 0x00};
        FillAlphaIndices(&data[2]);
        constexpr Color Rows[4] = {{1, 0, 0, 1}, {0, 0, 0, 1}, {6.0 / 7.0, 0, 0, 1}, {1.0 / 7.0, 0, 0, 1}};
        BlockRows(gpu, DkImageFormat_R_BC4_Unorm, data, Rows);
    }});
    scenes.push_back({"bc4_snorm", [](Gpu& gpu) {
        uint8_t data[8] = {0x7F, 0x81};
        FillAlphaIndices(&data[2]);
        constexpr Color Rows[4] = {{1, 0, 0, 1}, {-1, 0, 0, 1}, {5.0 / 7.0, 0, 0, 1}, {-5.0 / 7.0, 0, 0, 1}};
        BlockRows(gpu, DkImageFormat_R_BC4_Snorm, data, Rows);
    }});
    scenes.push_back({"bc5_unorm", [](Gpu& gpu) {
        uint8_t data[16] = {0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0x00, 0xFF};
        FillAlphaIndices(&data[2]);
        FillAlphaIndices(&data[10]);
        // Green's endpoints rise, so its block takes the six-value mode: index 2 is 1/5, index 7 is 1.
        constexpr Color Rows[4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {6.0 / 7.0, 1.0 / 5.0, 0, 1}, {1.0 / 7.0, 1, 0, 1}};
        BlockRows(gpu, DkImageFormat_RG_BC5_Unorm, data, Rows);
    }});
    scenes.push_back({"bc5_snorm", [](Gpu& gpu) {
        uint8_t data[16] = {0x7F, 0x81, 0, 0, 0, 0, 0, 0, 0x81, 0x7F};
        FillAlphaIndices(&data[2]);
        FillAlphaIndices(&data[10]);
        // Green's endpoints rise, so its block takes the six-value mode: index 2 is -76/127, index 7 is 1.
        constexpr Color Rows[4] = {{1, -1, 0, 1}, {-1, 1, 0, 1}, {5.0 / 7.0, -76.0 / 127.0, 0, 1}, {-5.0 / 7.0, 1, 0, 1}};
        BlockRows(gpu, DkImageFormat_RG_BC5_Snorm, data, Rows);
    }});
    scenes.push_back({"bc7_mode6", [](Gpu& gpu) {
        Block block;
        block.Set(0, 7, 0x40);
        // Endpoints: 7 bits each plus the shared low bit set, so red and green come out at 255 and 1.
        block.Set(7, 7, 127);  // R0
        block.Set(14, 7, 0);   // R1
        block.Set(21, 7, 0);   // G0
        block.Set(28, 7, 127); // G1
        block.Set(35, 7, 0);   // B0
        block.Set(42, 7, 0);   // B1
        block.Set(49, 7, 127); // A0
        block.Set(56, 7, 127); // A1
        block.Set(63, 1, 1);   // P0
        block.Set(64, 1, 1);   // P1
        // Rows 0 and 1 index 0 and rows 2 and 3 index 15; the first index has no top bit.
        for (uint32_t i = 8; i < 16; ++i) {
            block.Set(68 + (i - 1) * 4, 4, 15);
        }
        constexpr Color Rows[4] = {{1, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}};
        BlockRows(gpu, DkImageFormat_RGBA_BC7_Unorm, block.bytes, Rows);
    }});
    scenes.push_back({"bc7_mode6_srgb", [](Gpu& gpu) {
        Block block;
        block.Set(0, 7, 0x40);
        for (uint32_t bit : {7u, 21u, 35u, 14u, 28u, 42u}) {
            block.Set(bit, 7, 64); // Gray: (64 << 1) | 0 = 128 for every color endpoint.
        }
        block.Set(49, 7, 127);
        block.Set(56, 7, 127);
        const double gray = SrgbToLinear(128.0 / 255.0);
        const Color rows[4] = {{gray, gray, gray, 1}, {gray, gray, gray, 1}, {gray, gray, gray, 1}, {gray, gray, gray, 1}};
        BlockRows(gpu, DkImageFormat_RGBA_BC7_Unorm_sRGB, block.bytes, rows);
    }});
    scenes.push_back({"bc6h_unsigned", [](Gpu& gpu) {
        Block block;
        block.Set(0, 5, 3); // Mode 11: 10-bit endpoints with no deltas.
        for (uint32_t i = 0; i < 6; ++i) {
            block.Set(5 + i * 10, 10, 512);
        }
        constexpr double Expected = 1.0 + 527.0 / 1024.0;
        constexpr Color Rows[4] = {{Expected, Expected, Expected, 1}, {Expected, Expected, Expected, 1},
                                   {Expected, Expected, Expected, 1}, {Expected, Expected, Expected, 1}};
        BlockRows(gpu, DkImageFormat_RGBA_BC6H_UF16_Float, block.bytes, Rows);
    }});
    scenes.push_back({"bc6h_signed", [](Gpu& gpu) {
        Block block;
        block.Set(0, 5, 3);
        for (uint32_t i = 0; i < 6; ++i) {
            block.Set(5 + i * 10, 10, 256);
        }
        constexpr double Expected = 1.0 + 543.0 / 1024.0;
        constexpr Color Rows[4] = {{Expected, Expected, Expected, 1}, {Expected, Expected, Expected, 1},
                                   {Expected, Expected, Expected, 1}, {Expected, Expected, Expected, 1}};
        BlockRows(gpu, DkImageFormat_RGBA_BC6H_SF16_Float, block.bytes, Rows);
    }});
    scenes.push_back({"bc_block_larger_image", [](Gpu& gpu) {
        // Two blocks across: the second one must land at x 4..7 of an NPOT image.
        constexpr uint32_t Width = 6, Height = 4;
        Image& image = gpu.MakeImage(DkImageFormat_RGB_BC1, Width, Height, 1, 1, DkImageType_2D, Sampled);
        const uint8_t blocks[16] = {0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0, 0xE0, 0x07, 0x00, 0x00, 0, 0, 0, 0};
        gpu.Upload(image, blocks, sizeof(blocks), Width, Height);
        Show(gpu, image);
        gpu.Expect(2, 16, Red);
        gpu.Expect(29, 16, Green);
    }});

    for (const AstcSize& size : AstcSizes) {
        scenes.push_back({Named("astc_", size.name), [&size](Gpu& gpu) {
            AstcConstant(gpu, size.format, size.w, size.h, {1, 0.5, 0, 1});
        }});
        scenes.push_back({Named("astc_srgb_", size.name), [&size](Gpu& gpu) {
            AstcConstant(gpu, size.srgb, size.w, size.h, {1, SrgbToLinear(128.0 / 255.0), 0, 1});
        }});
    }

    scenes.push_back({"etc2_rgb", [](Gpu& gpu) {
        constexpr uint8_t Data[8] = {0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0};
        constexpr double Low = 2.0 / 255.0;
        constexpr Color Rows[4] = {{1, Low, Low, 1}, {1, Low, Low, 1}, {1, Low, Low, 1}, {1, Low, Low, 1}};
        BlockRows(gpu, DkImageFormat_RGB_ETC2, Data, Rows);
    }});
    scenes.push_back({"etc2_rgba", [](Gpu& gpu) {
        constexpr uint8_t Data[16] = {0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0};
        constexpr double Low = 2.0 / 255.0;
        constexpr Color Rows[4] = {{1, Low, Low, 1}, {1, Low, Low, 1}, {1, Low, Low, 1}, {1, Low, Low, 1}};
        BlockRows(gpu, DkImageFormat_RGBA_ETC2, Data, Rows, 0.03);
    }});
    scenes.push_back({"eac_r11", [](Gpu& gpu) {
        constexpr uint8_t Data[8] = {0xFF, 0x00, 0, 0, 0, 0, 0, 0};
        constexpr Color Rows[4] = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
        BlockRows(gpu, DkImageFormat_R_ETC2_Unorm, Data, Rows);
    }});
    scenes.push_back({"eac_rg11", [](Gpu& gpu) {
        constexpr uint8_t Data[16] = {0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 0, 0, 0, 0, 0, 0};
        constexpr Color Rows[4] = {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}};
        BlockRows(gpu, DkImageFormat_RG_ETC2_Unorm, Data, Rows);
    }});

    // Image types.
    scenes.push_back({"type_2d_placement", [](Gpu& gpu) {
        Image& image = Pattern(gpu);
        Show(gpu, image, DkImageFormat_RGBA8_Unorm);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                gpu.Expect(x * 8 + 4, y * 8 + 4, PatternColor(x, y));
            }
        }
    }});
    scenes.push_back({"type_2d_not_power_of_two", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 5, 3);
        uint8_t texels[5 * 3 * 4];
        for (uint32_t y = 0; y < 3; ++y) {
            for (uint32_t x = 0; x < 5; ++x) {
                uint8_t* texel = &texels[(y * 5 + x) * 4];
                texel[0] = static_cast<uint8_t>((x + 1) * 50);
                texel[1] = static_cast<uint8_t>((y + 1) * 80);
                texel[2] = 0;
                texel[3] = 255;
            }
        }
        gpu.Upload(image, texels, sizeof(texels), 5, 3);
        Show(gpu, image, DkImageFormat_RGBA8_Unorm);
        gpu.Expect(3, 5, {50 / 255.0, 80 / 255.0, 0, 1});
        gpu.Expect(29, 27, {250 / 255.0, 240 / 255.0, 0, 1});
    }});
    // No 1D scenes: deko3d refuses every copy into a 1D image, so there is no way to fill one.
    scenes.push_back({"type_3d", [](Gpu& gpu) {
        // 3D images cannot be used with the 2D engine; the upload goes through the copy engine.
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 2, 2, 4, 1, DkImageType_3D, 0);
        uint8_t texels[2 * 2 * 4 * 4];
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t slice = i / 4;
            texels[i * 4 + 0] = static_cast<uint8_t>((slice + 1) * 50);
            texels[i * 4 + 1] = 0;
            texels[i * 4 + 2] = 0;
            texels[i * 4 + 3] = 255;
        }
        gpu.Upload(image, texels, sizeof(texels), 2, 2, 0, 0, 4);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "tex3d_fsh");
        const float slice[4] = {2.5f / 4.0f, 0, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, slice, sizeof(slice));
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {150 / 255.0, 0, 0, 1});
    }});
    scenes.push_back({"type_2d_array", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1, 3, 1, DkImageType_2DArray);
        constexpr uint8_t Layers[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
        for (uint32_t layer = 0; layer < 3; ++layer) {
            gpu.Upload(image, Layers[layer], 4, 1, 1, 0, layer);
        }
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "array_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        for (uint32_t layer = 0; layer < 3; ++layer) {
            const float value[4] = {static_cast<float>(layer), 0, 0, 0};
            gpu.Uniform(DkStage_Fragment, 0, value, sizeof(value));
            gpu.Rect(layer * 10.0f, 0, layer * 10.0f + 10.0f, 32, White);
        }
        gpu.Finish(&result);
        gpu.Expect(5, 16, Red);
        gpu.Expect(15, 16, Green);
        gpu.Expect(25, 16, Blue);
    }});
    scenes.push_back({"type_cubemap", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, DkImageType_Cubemap);
        constexpr uint8_t Faces[6] = {40, 80, 120, 160, 200, 240};
        for (uint32_t face = 0; face < 6; ++face) {
            const uint8_t texel[4] = {Faces[face], 0, 0, 255};
            gpu.Upload(image, texel, 4, 1, 1, 0, face);
        }
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "cube_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        constexpr float Directions[6][4] = {{1, 0, 0, 0},  {-1, 0, 0, 0}, {0, 1, 0, 0},
                                            {0, -1, 0, 0}, {0, 0, 1, 0},  {0, 0, -1, 0}};
        for (uint32_t face = 0; face < 6; ++face) {
            gpu.Uniform(DkStage_Fragment, 0, Directions[face], sizeof(Directions[face]));
            gpu.Rect(face * 5.0f, 0, face * 5.0f + 5.0f, 32, White);
        }
        gpu.Finish(&result);
        for (uint32_t face = 0; face < 6; ++face) {
            gpu.Expect(face * 5 + 2, 16, {Faces[face] / 255.0, 0, 0, 1});
        }
    }});

    // Views.
    static constexpr struct {
        const char* name;
        DkImageSwizzle x, y, z, w;
        Color expected;
    } Swizzles[] = {
        {"identity", DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue, DkImageSwizzle_Alpha,
         {0.2, 0.4, 0.6, 0.8}},
        {"reversed", DkImageSwizzle_Alpha, DkImageSwizzle_Blue, DkImageSwizzle_Green, DkImageSwizzle_Red,
         {0.8, 0.6, 0.4, 0.2}},
        {"red_everywhere", DkImageSwizzle_Red, DkImageSwizzle_Red, DkImageSwizzle_Red, DkImageSwizzle_Red,
         {0.2, 0.2, 0.2, 0.2}},
        {"zero_and_one", DkImageSwizzle_Zero, DkImageSwizzle_One, DkImageSwizzle_Green, DkImageSwizzle_One,
         {0, 1, 0.4, 1}},
    };
    for (const auto& swizzle : Swizzles) {
        scenes.push_back({Named("swizzle_", swizzle.name), [&swizzle](Gpu& gpu) {
            Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1);
            const uint8_t texel[4] = {51, 102, 153, 204};
            gpu.Upload(image, texel, 4, 1, 1);
            dk::ImageView view{image.image};
            view.setSwizzle(swizzle.x, swizzle.y, swizzle.z, swizzle.w);
            Show(gpu, view, DkImageFormat_RGBA8_Unorm);
            gpu.Expect(16, 16, swizzle.expected);
        }});
    }
    scenes.push_back({"view_missing_channels", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_R8_Unorm, 1, 1);
        const uint8_t texel = 128;
        gpu.Upload(image, &texel, 1, 1, 1);
        Show(gpu, image, DkImageFormat_RGBA8_Unorm);
        // Channels the format lacks read as 0, 0, 1.
        gpu.Expect(16, 16, {128 / 255.0, 0, 0, 1});
    }});
    scenes.push_back({"view_reinterpret_uint", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32);
        std::vector<uint8_t> texels(32 * 32 * 4, 0);
        for (uint32_t i = 0; i < 32 * 32; ++i) {
            texels[i * 4] = 200;
        }
        gpu.Upload(image, texels.data(), static_cast<uint32_t>(texels.size()), 32, 32);
        dk::ImageView view{image.image};
        view.setFormat(DkImageFormat_R32_Uint);
        Show(gpu, view, DkImageFormat_RGBA8_Unorm, "utex_fsh");
        // R32 has no alpha: it reads as integer 1, which the shader divides by 255 like the rest.
        gpu.Expect(16, 16, {200 / 255.0, 0, 0, 1 / 255.0});
    }});
    scenes.push_back({"view_reinterpret_srgb", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1);
        const uint8_t texel[4] = {128, 128, 128, 255};
        gpu.Upload(image, texel, 4, 1, 1);
        dk::ImageView view{image.image};
        view.setFormat(DkImageFormat_RGBA8_Unorm_sRGB);
        Show(gpu, view);
        const double linear = SrgbToLinear(128.0 / 255.0);
        gpu.Expect(16, 16, {linear, linear, linear, 1}, 0.01);
    }});
    scenes.push_back({"view_one_layer_of_array", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1, 3, 1, DkImageType_2DArray);
        constexpr uint8_t Layers[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
        for (uint32_t layer = 0; layer < 3; ++layer) {
            gpu.Upload(image, Layers[layer], 4, 1, 1, 0, layer);
        }
        dk::ImageView view{image.image};
        view.setType(DkImageType_2D).setLayers(2, 1);
        Show(gpu, view, DkImageFormat_RGBA8_Unorm);
        gpu.Expect(16, 16, Blue);
    }});
    scenes.push_back({"view_one_mip", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 4);
        dk::ImageView view{image.image};
        view.setMipLevels(1, 1);
        Show(gpu, view, DkImageFormat_RGBA8_Unorm);
        gpu.Expect(16, 16, Green);
    }});

    // Samplers.
    for (const WrapCase& wrap : WrapModes) {
        scenes.push_back({Named("wrap_", wrap.name), [&wrap](Gpu& gpu) {
            Image& image = TwoTexels(gpu);
            Image& result = gpu.Begin();
            gpu.Shaders("basic_vsh", "texture_fsh");
            dk::Sampler sampler;
            sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_None);
            sampler.setWrapMode(wrap.mode, wrap.mode, wrap.mode);
            sampler.setBorderColor(0.0f, 0.0f, 1.0f, 1.0f);
            gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
            const Vertex vertices[4] = {{{-1, 1, 0}, {1, 1, 1, 1}, {-1, 0}},
                                        {{-1, -1, 0}, {1, 1, 1, 1}, {-1, 1}},
                                        {{1, 1, 0}, {1, 1, 1, 1}, {2, 0}},
                                        {{1, -1, 0}, {1, 1, 1, 1}, {2, 1}}};
            gpu.BindVertices(vertices, 4);
            gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
            gpu.Finish(&result);
            for (uint32_t x = 1; x < 32; x += 3) {
                const double u = -1.0 + 3.0 * (x + 0.5) / 32.0;
                const double distance_to_edge = std::fabs(u * 2.0 - std::round(u * 2.0));
                if (distance_to_edge < 0.2) continue; // Next to a texel edge either answer is right.
                gpu.Expect(x, 16, WrapExpected(wrap.mode, u));
            }
        }});
    }
    scenes.push_back({"border_color_integer_format", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1);
        const uint8_t texel[4] = {0, 0, 0, 255};
        gpu.Upload(image, texel, 4, 1, 1);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_None);
        sampler.setWrapMode(DkWrapMode_ClampToBorder, DkWrapMode_ClampToBorder, DkWrapMode_ClampToBorder);
        // Not one of the three colors static D3D12 samplers can do.
        sampler.setBorderColor(0.25f, 0.5f, 0.75f, 1.0f);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        const Vertex vertices[4] = {{{-1, 1, 0}, {1, 1, 1, 1}, {2, 2}},
                                    {{-1, -1, 0}, {1, 1, 1, 1}, {2, 3}},
                                    {{1, 1, 0}, {1, 1, 1, 1}, {3, 2}},
                                    {{1, -1, 0}, {1, 1, 1, 1}, {3, 3}}};
        gpu.BindVertices(vertices, 4);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {0.25, 0.5, 0.75, 1});
    }});
    scenes.push_back({"filter_linear", [](Gpu& gpu) {
        Image& image = TwoTexels(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Linear()));
        gpu.Full(White);
        gpu.Finish(&result);
        for (uint32_t x : {10u, 16u, 22u}) {
            const double green = (x + 0.5) / 32.0 * 2.0 - 0.5;
            gpu.Expect(x, 16, {1.0 - green, green, 0, 1}, 0.02);
        }
    }});
    scenes.push_back({"filter_reduction_min", [](Gpu& gpu) {
        Image& image = TwoTexels(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler = Linear();
        sampler.setReductionMode(DkSamplerReduction_Min);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {0, 0, 0, 1}, 0.02);
    }});
    scenes.push_back({"filter_reduction_max", [](Gpu& gpu) {
        Image& image = TwoTexels(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler = Linear();
        sampler.setReductionMode(DkSamplerReduction_Max);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {1, 1, 0, 1}, 0.02);
    }});
    scenes.push_back({"filter_anisotropic_on_solid", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 32);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Linear, DkFilter_Linear, DkMipFilter_Linear);
        sampler.setMaxAnisotropy(16.0f);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        // One texel per pixel: anisotropy has nothing to add and the first level shows.
        gpu.Expect(16, 16, Red, 0.05);
    }});
    scenes.push_back({"mip_selected_by_footprint", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 64); // Two texels a pixel: level 1.
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_Nearest);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"mip_explicit_lod", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 8);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "tex_lod_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_Nearest);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        for (uint32_t level = 0; level < 3; ++level) {
            const float lod[4] = {static_cast<float>(level), 0, 0, 0};
            gpu.Uniform(DkStage_Fragment, 0, lod, sizeof(lod));
            gpu.Rect(level * 10.0f, 0, level * 10.0f + 10.0f, 32, White);
        }
        gpu.Finish(&result);
        gpu.Expect(5, 16, Red);
        gpu.Expect(15, 16, Green);
        gpu.Expect(25, 16, Blue);
    }});
    scenes.push_back({"mip_trilinear", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 8);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "tex_lod_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Linear, DkFilter_Linear, DkMipFilter_Linear);
        const float lod[4] = {0.5f, 0, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, lod, sizeof(lod));
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {0.5, 0.5, 0, 1}, 0.03);
    }});
    scenes.push_back({"mip_no_mip_filter_uses_base", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 64);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, Red);
    }});
    scenes.push_back({"mip_lod_clamp", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 32);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_Nearest);
        sampler.setLodClamp(2.0f, 3.0f);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, Blue);
    }});
    scenes.push_back({"mip_lod_bias", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 32);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_Nearest);
        sampler.setLodBias(1.0f);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"texel_fetch", [](Gpu& gpu) {
        Image& image = Pattern(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "fetch_fsh");
        const int32_t place[4] = {2, 1, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, place, sizeof(place));
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, PatternColor(2, 1));
    }});
    scenes.push_back({"texel_fetch_level", [](Gpu& gpu) {
        Image& image = MipChain(gpu, 8);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "fetch_fsh");
        const int32_t place[4] = {1, 0, 2, 0};
        gpu.Uniform(DkStage_Fragment, 0, place, sizeof(place));
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, Blue);
    }});
    scenes.push_back({"texture_offset", [](Gpu& gpu) {
        Image& image = Pattern(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "offset_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, PatternColor(1, 2));
    }});
    scenes.push_back({"texture_gather", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 2, 2);
        const uint8_t texels[16] = {51, 0, 0, 255, 102, 0, 0, 255, 153, 0, 0, 255, 204, 0, 0, 255};
        gpu.Upload(image, texels, sizeof(texels), 2, 2);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "gather_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Linear()));
        gpu.Full(White);
        gpu.Finish(&result);
        // In gather order: (0,1), (1,1), (1,0), (0,0).
        gpu.Expect(16, 16, {0.6, 0.8, 0.4, 0.2});
    }});
    // Gathers of the green and blue channels with texel offsets, which the console's compiler
    // faults on as instructions: they move the coordinates instead. Texel (x, y) holds green
    // (x + 4y) * 16 + 8 and blue 255 minus that; the footprint at the centre starts at (1, 1).
    const auto offset_texels = [](Gpu& gpu) -> Image& {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 4, 4);
        uint8_t texels[64];
        for (uint32_t i = 0; i < 16; ++i) {
            const uint8_t green = static_cast<uint8_t>(i * 16 + 8);
            texels[i * 4 + 0] = 0;
            texels[i * 4 + 1] = green;
            texels[i * 4 + 2] = static_cast<uint8_t>(255 - green);
            texels[i * 4 + 3] = 255;
        }
        gpu.Upload(image, texels, sizeof(texels), 4, 4);
        return image;
    };
    scenes.push_back({"texture_gather_offset_green", [offset_texels](Gpu& gpu) {
        Image& image = offset_texels(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "gather_offset_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Linear()));
        gpu.Full(White);
        gpu.Finish(&result);
        // Offset (-1, 1): texels (0,3), (1,3), (1,2), (0,2).
        gpu.Expect(8, 16, {200 / 255.0, 216 / 255.0, 152 / 255.0, 136 / 255.0});
        // Offset (2, -1): x 3..4 clamps to 3, texels (3,1), (3,1), (3,0), (3,0).
        gpu.Expect(24, 16, {120 / 255.0, 120 / 255.0, 56 / 255.0, 56 / 255.0});
    }});
    scenes.push_back({"texture_gather_offsets_blue", [offset_texels](Gpu& gpu) {
        Image& image = offset_texels(gpu);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "gather_offsets_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Linear()));
        gpu.Full(White);
        gpu.Finish(&result);
        // Blue of texels (1,1), (2,1), (0,2), (2,2).
        gpu.Expect(16, 16, {167 / 255.0, 151 / 255.0, 119 / 255.0, 87 / 255.0});
    }});

    // Multisampling.
    scenes.push_back({"ms_resolve_solid", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 16, 32, Green);
        Resolve(gpu, target);
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"ms_resolve_clear", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor({0.25, 0.5, 0.75, 1});
        Resolve(gpu, target);
        gpu.Expect(16, 16, {0.25, 0.5, 0.75, 1});
    }});
    scenes.push_back({"ms_resolve_edge_mixes", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        const Vertex triangle[3] = {{{-1, 1, 0}, {0, 1, 0, 1}, {0, 0}},
                                    {{-1, -1, 0}, {0, 1, 0, 1}, {0, 0}},
                                    {{1, 1, 0}, {0, 1, 0, 1}, {0, 0}}};
        gpu.BindVertices(triangle, 3);
        gpu.cmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
        Resolve(gpu, target);
        gpu.Expect(4, 4, Green);
        gpu.Expect(28, 28, Black);
        // A pixel the diagonal crosses gets some of each, whichever samples it covers.
        const double edge = gpu.Read(16, 15).g;
        if (edge <= 0.1 || edge >= 0.9) gpu.Fail("the edge pixel reads %.3f", edge);
    }});
    scenes.push_back({"ms_sample_mask", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        gpu.cmd.setSampleMask(0x3);
        gpu.Full(Green);
        Resolve(gpu, target);
        gpu.Expect(16, 16, {0, 0.5, 0, 1}, 0.02);
    }});
    scenes.push_back({"ms_alpha_to_coverage", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        dk::MultisampleState state;
        state.setMode(DkMsMode_4x).setAlphaToCoverageEnable(true);
        gpu.cmd.bindMultisampleState(state);
        gpu.Rect(0, 0, 16, 32, {0, 1, 0, 0.5});
        gpu.Rect(16, 0, 32, 32, {0, 1, 0, 0});
        Resolve(gpu, target);
        // About half of the samples; the exact pattern is up to the hardware.
        const double half = gpu.Read(8, 16).g;
        if (half < 0.2 || half > 0.8) gpu.Fail("alpha 0.5 covers %.3f", half);
        gpu.Check("alpha 0", gpu.Read(24, 16).g, 0.0, 0.01);
    }});
    scenes.push_back({"ms_per_sample_shading", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        gpu.Shaders("basic_vsh", "sample_id_fsh");
        gpu.Full(White);
        Resolve(gpu, target);
        gpu.Expect(16, 16, {0.25, 0.75, 0, 1}, 0.02);
    }});
    scenes.push_back({"ms_texel_fetch", [](Gpu& gpu) {
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        gpu.cmd.setSampleMask(0x1);
        gpu.Full(Green);
        gpu.Barrier();
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "ms_fetch_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(target, Nearest()));
        int32_t index[4] = {0, 0, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, index, sizeof(index));
        gpu.Rect(0, 0, 16, 32, White);
        index[0] = 1;
        gpu.Uniform(DkStage_Fragment, 0, index, sizeof(index));
        gpu.Rect(16, 0, 32, 32, White);
        gpu.Finish(&result);
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});

    // deko3d writes one sample mask to the four registers of the 2x2 footprint: the scene gives
    // each its own value in the recorded command.
    scenes.push_back({"ms_sample_mask_per_position", [](Gpu& gpu) {
        constexpr uint32_t Written = 0xA5A5;
        const uint32_t from[4] = {Written, Written, Written, Written};
        const uint32_t to[4] = {0xF, 0x3, 0x1, 0xE};
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Black);
        gpu.cmd.setSampleMask(Written);
        if (!gpu.PatchCommands(from, to, 4)) {
            gpu.Fail("the sample mask registers are not in the recorded command");
            return;
        }
        gpu.Full(Green);
        Resolve(gpu, target);
        gpu.Check("even x, even y", gpu.Read(16, 16).g, 1.0, 0.02);
        gpu.Check("odd x, even y", gpu.Read(17, 16).g, 0.5, 0.02);
        gpu.Check("even x, odd y", gpu.Read(16, 17).g, 0.25, 0.02);
        gpu.Check("odd x, odd y", gpu.Read(17, 17).g, 0.75, 0.02);
    }});

    // Resolves that are not a whole color image of one format. The sources are one color, so the
    // scenes only say where the result lands.
    scenes.push_back({"ms_resolve_part", [](Gpu& gpu) {
        Image& source = MsTarget(gpu);
        gpu.ClearColor(Green);
        gpu.Barrier();
        Image& result = gpu.Begin();
        gpu.ClearColor(Blue);
        gpu.Barrier();
        dk::ImageView from{source.image}, to{result.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 16, 16, 1}, to, {0, 0, 0, 16, 16, 1}, DkBlitFlag_FilterNearest);
        gpu.Finish(&result);
        gpu.Expect(8, 8, Green);
        gpu.Expect(24, 24, Blue);
    }});
    scenes.push_back({"ms_resolve_scaled", [](Gpu& gpu) {
        Image& source = MsTarget(gpu);
        gpu.ClearColor(Green);
        gpu.Barrier();
        Image& result = gpu.Begin();
        gpu.ClearColor(Blue);
        gpu.Barrier();
        dk::ImageView from{source.image}, to{result.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 16, 16, 1}, to, {0, 0, 0, 32, 16, 1}, DkBlitFlag_FilterLinear);
        gpu.Finish(&result);
        gpu.Expect(24, 8, Green);
        gpu.Expect(8, 24, Blue);
    }});
    scenes.push_back({"ms_resolve_uint", [](Gpu& gpu) {
        Image& target = MsTarget(gpu, DkImageFormat_RGBA8_Uint);
        gpu.cmd.clearColor<uint32_t>(0, DkColorMask_RGBA, 12, 34, 56, 78);
        Resolve(gpu, target);
        gpu.ExpectUint(16, 16, 12, 34, 56, 78);
    }});
    scenes.push_back({"ms_resolve_sint", [](Gpu& gpu) {
        Image& target = MsTarget(gpu, DkImageFormat_RGBA16_Sint);
        gpu.cmd.clearColor<int32_t>(0, DkColorMask_RGBA, -12, 34, -56, 78);
        Resolve(gpu, target);
        gpu.Expect(16, 16, {-12.0, 34.0, -56.0, 78.0}, 0.0);
    }});
    scenes.push_back({"ms_blit_into", [](Gpu& gpu) {
        Image& source = gpu.Begin();
        gpu.ClearColor(Green);
        gpu.Barrier();
        Image& target = MsTarget(gpu);
        gpu.ClearColor(Blue);
        gpu.Barrier();
        dk::ImageView from{source.image}, to{target.image};
        gpu.cmd.blitImage(from, {0, 0, 0, 32, 32, 1}, to, {0, 0, 0, 32, 32, 1}, DkBlitFlag_FilterNearest);
        Resolve(gpu, target);
        gpu.Expect(16, 16, Green);
    }});

    // Rendering into parts of an image, then sampling it.
    scenes.push_back({"render_then_sample", [](Gpu& gpu) {
        Image& first = gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 16, 32, Green);
        gpu.Barrier();
        Show(gpu, dk::ImageView{first.image}, DkImageFormat_RGBA8_Unorm);
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"render_then_sample_twice", [](Gpu& gpu) {
        Image& first = gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 16, 32, Green);
        gpu.Barrier();
        Image& second = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(first, Nearest()));
        gpu.Full(White);
        gpu.Barrier();
        gpu.UseTarget(first);
        gpu.Rect(16, 0, 32, 32, Blue);
        gpu.Barrier();
        Show(gpu, dk::ImageView{first.image}, DkImageFormat_RGBA8_Unorm);
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Blue);
        gpu.Finish(&second);
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"render_to_mip", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32, 1, 2);
        dk::ImageView base{image.image}, small{image.image};
        base.setMipLevels(0, 1);
        small.setMipLevels(1, 1);
        Image& result = gpu.Begin(); // Provides the state and later the target to show it.
        gpu.cmd.bindRenderTargets(&base);
        gpu.ClearColor(Red);
        gpu.cmd.bindRenderTargets(&small);
        gpu.cmd.setViewports(0, {{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f}});
        gpu.cmd.setScissors(0, {{0, 0, 16, 16}});
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 16, 32, Green); // The left half in clip space.
        gpu.Barrier();
        gpu.UseTarget(result);
        gpu.Shaders("basic_vsh", "tex_lod_fsh");
        const float lod[4] = {1, 0, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, lod, sizeof(lod));
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest, DkMipFilter_Nearest);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, sampler));
        gpu.Full(White);
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"render_to_array_layer_by_view", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32, 3, 1, DkImageType_2DArray);
        Image& result = gpu.Begin();
        for (uint32_t layer = 0; layer < 3; ++layer) {
            dk::ImageView view{image.image};
            view.setType(DkImageType_2D).setLayers(layer, 1);
            gpu.cmd.bindRenderTargets(&view);
            gpu.ClearColor(layer == 1 ? Green : Red);
        }
        gpu.Barrier();
        gpu.UseTarget(result);
        gpu.Shaders("basic_vsh", "array_fsh");
        const float layer[4] = {1, 0, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, layer, sizeof(layer));
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        gpu.Full(White);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"render_to_layer_from_geometry", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32, 3, 1, DkImageType_2DArray);
        Image& result = gpu.Begin();
        dk::ImageView all{image.image};
        all.setLayers(0, 3);
        gpu.cmd.bindRenderTargets(&all);
        gpu.ClearColor(Black);
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask,
                            {gpu.Shader("basic_vsh"), gpu.Shader("layer_gsh"), gpu.Shader("color_fsh")});
        const int32_t layer_number[4] = {2, 0, 0, 0};
        gpu.Uniform(DkStage_Geometry, 0, layer_number, sizeof(layer_number));
        gpu.Full(Green);
        gpu.Barrier();
        gpu.UseTarget(result);
        gpu.Shaders("basic_vsh", "array_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(image, Nearest()));
        float layer[4] = {2, 0, 0, 0};
        gpu.Uniform(DkStage_Fragment, 0, layer, sizeof(layer));
        gpu.Rect(0, 0, 16, 32, White);
        layer[0] = 1;
        gpu.Uniform(DkStage_Fragment, 0, layer, sizeof(layer));
        gpu.Rect(16, 0, 32, 32, White);
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    // Formats deko3d does not name, with the guest's texture descriptor written by hand: format
    // in bits 0-6, UNORM (2) in each component type, then the x/y/z/w sources.
    scenes.push_back({"format_a5b5g5r1_unorm", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_R16_Unorm, 1, 1);
        const uint16_t texel = 0xF821; // A 31, B 0, G 16, R 1
        gpu.Upload(image, &texel, sizeof(texel), 1, 1);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        const uint32_t unorm = (2u << 7) | (2u << 10) | (2u << 13) | (2u << 16);
        const uint32_t word0 = 0x13u | unorm | (2u << 19) | (3u << 22) | (4u << 25) | (5u << 28);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.TextureAs(image, Nearest(), word0));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {1, 16.0 / 31.0, 0, 1}, 0.02);
    }});
    scenes.push_back({"format_g4r4_unorm", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_R8_Unorm, 1, 1);
        const uint8_t texel = 0xA5; // G 10, R 5
        gpu.Upload(image, &texel, sizeof(texel), 1, 1);
        Image& result = gpu.Begin();
        gpu.Shaders("basic_vsh", "texture_fsh");
        const uint32_t unorm = (2u << 7) | (2u << 10) | (2u << 13) | (2u << 16);
        const uint32_t word0 = 0x1Eu | unorm | (2u << 19) | (3u << 22) | (0u << 25) | (7u << 28);
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.TextureAs(image, Nearest(), word0));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {5.0 / 15.0, 10.0 / 15.0, 0, 1}, 0.02);
    }});
    scenes.push_back({"render_to_pitch_linear", [](Gpu& gpu) {
        Image& target = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32, 1, 1, DkImageType_2D,
                                      DkImageFlags_PitchLinear | DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine,
                                      DkMsMode_1x, 32 * 4);
        gpu.UseTarget(target);
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 16, 32, Green);
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
}
