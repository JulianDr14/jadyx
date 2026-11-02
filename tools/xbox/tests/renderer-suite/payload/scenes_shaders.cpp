// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The shader recompiler and its bindings: compute, math against the values C++ computes in the guest,
// control flow, uniforms, storage buffers, images and fragment-stage builtins.

#include "harness.h"
#include "scene_util.h"

#include <cmath>

namespace {

Slice Storage(Gpu& gpu, uint32_t size, uint8_t fill = 0) {
    Slice slice = gpu.Alloc(size, DK_UNIFORM_BUF_ALIGNMENT);
    std::memset(slice.cpu, fill, size);
    return slice;
}

void Compute(Gpu& gpu, const char* shader) {
    // The descriptor sets are bound with the 3D state; a compute scene run alone has no earlier
    // scene to have bound them.
    gpu.DefaultState();
    gpu.cmd.bindShaders(DkStageFlag_Compute, {gpu.Shader(shader)});
}

void EndCompute(Gpu& gpu) {
    gpu.cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
    gpu.Submit();
}

uint32_t ReadUint(const Slice& slice, uint32_t index) {
    return static_cast<volatile uint32_t*>(slice.cpu)[index];
}

float ReadFloat(const Slice& slice, uint32_t index) {
    return static_cast<volatile float*>(slice.cpu)[index];
}

constexpr uint32_t Flags = DkImageFlags_UsageRender | DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine;

uint32_t BitReverse(uint32_t value) {
    uint32_t result = 0;
    for (int i = 0; i < 32; ++i) {
        result = (result << 1) | ((value >> i) & 1);
    }
    return result;
}

} // namespace

void RegisterShaders(std::vector<SceneEntry>& scenes) {
    scenes.push_back({"compute_fill", [](Gpu& gpu) {
        Slice values = Storage(gpu, 16 * 4);
        Compute(gpu, "fill_cs");
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(2, 1, 1);
        EndCompute(gpu);
        for (uint32_t i = 0; i < 16; ++i) {
            gpu.Check("value", ReadUint(values, i), i * 2 + 1);
        }
    }});
    // compute_fill read back through a copy: tells a lost shader write from a lost download.
    scenes.push_back({"compute_fill_copied", [](Gpu& gpu) {
        Slice values = Storage(gpu, 16 * 4);
        Slice copy = Storage(gpu, 16 * 4);
        Compute(gpu, "fill_cs");
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(2, 1, 1);
        gpu.cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        gpu.cmd.copyBuffer(values.gpu, copy.gpu, 16 * 4);
        EndCompute(gpu);
        for (uint32_t i = 0; i < 16; ++i) {
            gpu.Check("value", ReadUint(copy, i), i * 2 + 1);
        }
    }});
    scenes.push_back({"compute_indirect", [](Gpu& gpu) {
        Slice values = Storage(gpu, 16 * 4);
        Slice args = gpu.Alloc(sizeof(DkDispatchIndirectData), 4);
        const DkDispatchIndirectData data{2, 1, 1};
        std::memcpy(args.cpu, &data, sizeof(data));
        Compute(gpu, "fill_cs");
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchComputeIndirect(args.gpu);
        EndCompute(gpu);
        for (uint32_t i = 0; i < 16; ++i) {
            gpu.Check("value", ReadUint(values, i), i * 2 + 1);
        }
    }});
    scenes.push_back({"compute_atomics", [](Gpu& gpu) {
        Slice values = Storage(gpu, 5 * 4);
        const uint32_t start[5] = {0, 0xFFFFFFFF, 0, 0, 7};
        std::memcpy(values.cpu, start, sizeof(start));
        Compute(gpu, "atomic_cs");
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        gpu.Check("sum", ReadUint(values, 0), 2080);
        gpu.Check("low", ReadUint(values, 1), 1);
        gpu.Check("high", ReadUint(values, 2), 64);
        gpu.Check("ored", ReadUint(values, 3), 0xFFFFFFFFu);
        gpu.Check("exchanged", ReadUint(values, 4), 9);
    }});
    scenes.push_back({"compute_shared_memory", [](Gpu& gpu) {
        Slice values = Storage(gpu, 8 * 4);
        Compute(gpu, "shared_cs");
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        for (uint32_t i = 0; i < 8; ++i) {
            gpu.Check("value", ReadUint(values, i), (7 - i) + 10);
        }
    }});
    scenes.push_back({"compute_ids", [](Gpu& gpu) {
        Slice values = Storage(gpu, 12 * 16);
        Compute(gpu, "ids_cs");
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(2, 1, 1);
        EndCompute(gpu);
        for (uint32_t group = 0; group < 2; ++group) {
            for (uint32_t index = 0; index < 6; ++index) {
                const uint32_t slot = (group * 6 + index) * 4;
                const uint32_t local_x = index % 2;
                gpu.Check("group", ReadUint(values, slot), group);
                gpu.Check("local x", ReadUint(values, slot + 1), local_x);
                gpu.Check("local y", ReadUint(values, slot + 2), index / 2);
                gpu.Check("global x", ReadUint(values, slot + 3), group * 2 + local_x);
            }
        }
    }});
    scenes.push_back({"compute_uniforms", [](Gpu& gpu) {
        Slice values = Storage(gpu, 32);
        const uint32_t input[8] = {0x3F800000, 0x40000000, 0x40400000, 0x40800000, 5, 6, 7, 8};
        Compute(gpu, "uniform_cs");
        gpu.Uniform(DkStage_Compute, 0, input, sizeof(input));
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        for (uint32_t i = 0; i < 8; ++i) {
            gpu.Check("copied word", ReadUint(values, i), input[i]);
        }
    }});
    scenes.push_back({"compute_samples_texture", [](Gpu& gpu) {
        Image& texture = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 4, 4, 1, 3);
        uint8_t texels[4 * 4 * 4]{};
        texels[(1 * 4 + 2) * 4 + 0] = 255;
        texels[(1 * 4 + 2) * 4 + 3] = 255;
        gpu.Upload(texture, texels, sizeof(texels), 4, 4);
        Slice values = Storage(gpu, 32);
        dk::Sampler sampler;
        Compute(gpu, "texture_cs");
        gpu.cmd.bindTextures(DkStage_Compute, 0, gpu.Texture(texture, sampler));
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, values.gpu, values.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        gpu.Check("red", ReadFloat(values, 0), 1.0, 0.01);
        gpu.Check("alpha", ReadFloat(values, 3), 1.0, 0.01);
        gpu.Check("width", ReadUint(values, 4), 4);
        gpu.Check("height", ReadUint(values, 5), 4);
        gpu.Check("levels", ReadUint(values, 6), 3);
    }});
    scenes.push_back({"compute_image_store", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32, 1, 1, DkImageType_2D, Flags);
        dk::ImageView view{image.image};
        Compute(gpu, "image_cs");
        gpu.cmd.bindImages(DkStage_Compute, 0, dkMakeImageHandle(gpu.SetImage(view, true)));
        gpu.cmd.dispatchCompute(8, 8, 1);
        gpu.Finish(&image);
        gpu.Expect(0, 0, {0, 0, 0, 1});
        gpu.Expect(31, 0, {1, 0, 0, 1});
        gpu.Expect(0, 31, {0, 1, 0, 1});
        gpu.Expect(10, 20, {10.0 / 31.0, 20.0 / 31.0, 0, 1});
    }});
    scenes.push_back({"compute_image_atomic", [](Gpu& gpu) {
        Image& image = gpu.MakeImage(DkImageFormat_R32_Uint, 4, 4, 1, 1, DkImageType_2D, Flags);
        dk::ImageView view{image.image};
        const uint32_t zeros[16]{};
        gpu.Upload(image, zeros, sizeof(zeros), 4, 4);
        Compute(gpu, "image_atomic_cs");
        gpu.cmd.bindImages(DkStage_Compute, 0, dkMakeImageHandle(gpu.SetImage(view, true)));
        gpu.cmd.dispatchCompute(1, 1, 1);
        gpu.Finish(&image);
        gpu.Check("count", gpu.Read(0, 0).r, 16);
        gpu.Check("other", gpu.Read(1, 1).r, 0);
    }});
    scenes.push_back({"math_integer", [](Gpu& gpu) {
        const uint32_t x = 0x92345678, y = 0x00F000F0, z = 7;
        const int32_t bx = -1000, by = 7;
        const uint32_t input[8] = {x, y, z, 1, static_cast<uint32_t>(bx), static_cast<uint32_t>(by), 0, 0};
        Slice results = Storage(gpu, 32 * 4);
        Compute(gpu, "int_math_cs");
        gpu.Uniform(DkStage_Compute, 0, input, sizeof(input));
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, results.gpu, results.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        const uint64_t product = static_cast<uint64_t>(x) * y;
        const uint32_t expected[] = {
            static_cast<uint32_t>(product >> 32),
            static_cast<uint32_t>(product),
            x + x,
            1,
            y - x,
            1,
            (x >> 8) & 0xFFF,
            static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((bx >> 4) & 0xFF))),
            (x & ~(0xFFu << 4)) | ((y & 0xFF) << 4),
            BitReverse(x),
            static_cast<uint32_t>(__builtin_popcount(x)),
            static_cast<uint32_t>(__builtin_ctz(y)),
            static_cast<uint32_t>(31 - __builtin_clz(x)),
            static_cast<uint32_t>(31 - __builtin_clz(~static_cast<uint32_t>(bx))),
            y / z,
            y % z,
            static_cast<uint32_t>(bx / by),
            static_cast<uint32_t>(bx >> 3),
            x >> 3,
            x << 5,
            static_cast<uint32_t>(bx),
            static_cast<uint32_t>(by),
            1000,
            0xFFFFFFFF,
            ~x,
            x ^ y,
            static_cast<uint32_t>(-5),
        };
        static constexpr const char* Names[] = {
            "umul high", "umul low", "uadd", "uadd carry", "usub", "usub borrow", "extract u", "extract s",
            "insert", "reverse", "bit count", "lsb", "msb u", "msb s", "div", "mod", "signed div",
            "signed shift", "shift right", "shift left", "min", "max", "abs", "sign", "not", "xor", "clamp"};
        for (uint32_t i = 0; i < std::size(expected); ++i) {
            gpu.Check(Names[i], ReadUint(results, i), expected[i]);
        }
        // Rounding of exact halves is up to the implementation: one step either way.
        const uint32_t unorm = ReadUint(results, 27);
        gpu.Check("unorm byte 0", unorm & 0xFF, 0);
        gpu.Check("unorm byte 1", (unorm >> 8) & 0xFF, 64, 1);
        gpu.Check("unorm byte 2", (unorm >> 16) & 0xFF, 128, 1);
        gpu.Check("unorm byte 3", unorm >> 24, 255);
        const uint32_t snorm = ReadUint(results, 28);
        gpu.Check("snorm low", snorm & 0xFFFF, 0x8001);
        gpu.Check("snorm high", snorm >> 16, 16384, 1);
        gpu.Check("half", ReadUint(results, 29), 0xC0003E00u);
        gpu.Check("float to int", ReadUint(results, 30), static_cast<uint32_t>(-3));
        gpu.Check("float to uint", ReadUint(results, 31), 3);
    }});
    scenes.push_back({"math_float", [](Gpu& gpu) {
        const float a[4] = {2.75f, 2.5f, 1.5f, -0.5f};
        Slice results = Storage(gpu, 32 * 4);
        Compute(gpu, "float_math_cs");
        gpu.Uniform(DkStage_Compute, 0, a, sizeof(a));
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, results.gpu, results.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        struct Case {
            const char* name;
            double expected;
            double tolerance;
        };
        const Case cases[] = {
            {"floor", 2, 0},
            {"ceil", 3, 0},
            {"trunc", 2, 0},
            {"round", 2.5, 0.5}, // Exact halves may go either way.
            {"round even", 2, 0},
            {"fract", 0.75, 1e-5},
            {"mod", 0.25, 1e-5},
            {"sqrt", std::sqrt(2.5), 1e-4},
            {"inversesqrt", 1.0 / std::sqrt(2.5), 1e-4},
            {"pow", std::pow(2.5, 2.5), 1e-2},
            {"exp2", std::pow(2.0, 2.5), 1e-3},
            {"log2", std::log2(2.5), 1e-4},
            {"sin", std::sin(2.5), 1e-3},
            {"cos", std::cos(2.5), 1e-3},
            {"atan", std::atan2(2.5, 1.5), 1e-3},
            {"mix", 2.6875, 1e-4},
            {"smoothstep", 0.68359375, 1e-4},
            {"step", 1, 0},
            {"clamp", 1, 0},
            {"min", 2.5, 0},
            {"max", 2.75, 0},
            {"fma", 8.375, 1e-4},
            {"frexp", 0.625, 1e-5},
            {"frexp exponent", 2, 0},
            {"ldexp", 20, 1e-4},
            {"dot", 2.875, 1e-4},
            {"length", std::sqrt(8.5), 1e-4},
            {"cross", -3.5, 1e-4},
            {"normalize", 2.5 / std::sqrt(8.5), 1e-4},
            {"abs", 2.75, 0},
            {"sign", 1, 0},
            {"bits to float", 1.5, 0},
        };
        for (uint32_t i = 0; i < std::size(cases); ++i) {
            gpu.Check(cases[i].name, ReadFloat(results, i), cases[i].expected, cases[i].tolerance);
        }
    }});
    scenes.push_back({"control_flow", [](Gpu& gpu) {
        const int32_t limits[4] = {10, 20, 5, 1};
        Slice results = Storage(gpu, 8 * 4);
        Compute(gpu, "control_flow_cs");
        gpu.Uniform(DkStage_Compute, 0, limits, sizeof(limits));
        gpu.cmd.bindStorageBuffer(DkStage_Compute, 0, results.gpu, results.size);
        gpu.cmd.dispatchCompute(1, 1, 1);
        EndCompute(gpu);
        gpu.Check("fibonacci", ReadUint(results, 0), 55);
        gpu.Check("continue and break", ReadUint(results, 1), 16);
        gpu.Check("do while", ReadUint(results, 2), 5);
        gpu.Check("switch fallthrough", ReadUint(results, 3), 25);
        gpu.Check("nested", ReadUint(results, 4), 6);
    }});

    scenes.push_back({"uniform_fragment", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "ubo_fsh");
        const float value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        gpu.Uniform(DkStage_Fragment, 0, value, sizeof(value));
        gpu.Full(White);
        gpu.Finish();
        gpu.Expect(16, 16, {0.25, 0.5, 0.75, 1});
    }});
    scenes.push_back({"uniform_array_indexed", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "uniform_array_fsh");
        const float values[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
        gpu.Uniform(DkStage_Fragment, 0, values, sizeof(values));
        gpu.Full(White);
        gpu.Finish();
        gpu.Expect(4, 16, Red);
        gpu.Expect(12, 16, Green);
        gpu.Expect(20, 16, Blue);
        gpu.Expect(28, 16, White);
    }});
    // The same uniform buffer memory read by two draws, rewritten between them from the command list:
    // the path every game's per-draw constants take.
    scenes.push_back({"uniform_rewritten_between_draws", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "ubo_fsh");
        // A pushed-to buffer's size must be a whole number of uniform buffer alignments.
        Slice block = gpu.Alloc(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
        const float zero[4] = {0, 0, 0, 1};
        std::memcpy(block.cpu, zero, sizeof(zero));
        gpu.cmd.bindUniformBuffer(DkStage_Fragment, 0, block.gpu, block.size);
        for (uint32_t i = 0; i < 4; ++i) {
            const float value[4] = {(i + 1) / 4.0f, 0, 0, 1};
            gpu.cmd.pushConstants(block.gpu, block.size, 0, sizeof(value), value);
            gpu.Rect(i * 8.0f, 0, i * 8.0f + 8.0f, 32, White);
        }
        gpu.Finish();
        for (uint32_t i = 0; i < 4; ++i) {
            gpu.Expect(i * 8 + 4, 16, {(i + 1) / 4.0, 0, 0, 1});
        }
    }});
    scenes.push_back({"uniform_cpu_overwrite", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "ubo_fsh");
        Slice block = gpu.Alloc(16, DK_UNIFORM_BUF_ALIGNMENT);
        const float first[4] = {1, 0, 0, 1};
        std::memcpy(block.cpu, first, sizeof(first));
        gpu.cmd.bindUniformBuffer(DkStage_Fragment, 0, block.gpu, block.size);
        gpu.Rect(0, 0, 16, 32, White);
        gpu.Submit();
        const float second[4] = {0, 1, 0, 1};
        std::memcpy(block.cpu, second, sizeof(second));
        gpu.cmd.bindUniformBuffer(DkStage_Fragment, 0, block.gpu, block.size);
        gpu.Rect(16, 0, 32, 32, White);
        gpu.Finish();
        gpu.Expect(8, 16, Red);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"storage_read_fragment", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "ssbo_fsh");
        const float values[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
        Slice block = gpu.Alloc(sizeof(values), DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(block.cpu, values, sizeof(values));
        gpu.cmd.bindStorageBuffer(DkStage_Fragment, 0, block.gpu, block.size);
        gpu.Full(White);
        gpu.Finish();
        gpu.Expect(4, 16, Red);
        gpu.Expect(28, 16, White);
    }});
    scenes.push_back({"storage_write_fragment", [](Gpu& gpu) {
        gpu.Begin();
        Slice counter = Storage(gpu, 16);
        gpu.Shaders("basic_vsh", "ssbo_write_fsh");
        gpu.cmd.bindStorageBuffer(DkStage_Fragment, 0, counter.gpu, counter.size);
        gpu.Full(White);
        gpu.Finish();
        gpu.Check("fragments", ReadUint(counter, 0), 32 * 32);
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"image_read_fragment", [](Gpu& gpu) {
        Image& source = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 32, 32, 1, 1, DkImageType_2D, Flags);
        gpu.UseTarget(source);
        gpu.ClearColor({0.25, 0.5, 0.75, 1});
        gpu.Barrier();
        Image& result = gpu.Begin();
        dk::ImageView view{source.image};
        gpu.Shaders("basic_vsh", "image_read_fsh");
        gpu.cmd.bindImages(DkStage_Fragment, 0, dkMakeImageHandle(gpu.SetImage(view, true)));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Expect(16, 16, {0.25, 0.5, 0.75, 1});
    }});
    scenes.push_back({"two_textures", [](Gpu& gpu) {
        Image& first = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1);
        Image& second = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, 1, 1);
        const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
        gpu.Upload(first, red, 4, 1, 1);
        gpu.Upload(second, green, 4, 1, 1);
        gpu.Begin();
        dk::Sampler sampler;
        gpu.Shaders("basic_vsh", "two_textures_fsh");
        const DkResHandle handles[2] = {gpu.Texture(first, sampler), gpu.Texture(second, sampler)};
        gpu.cmd.bindTextures(DkStage_Fragment, 0, {handles[0], handles[1]});
        gpu.Full(White);
        gpu.Finish();
        gpu.Expect(8, 16, Red);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"fragment_discard", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Shaders("basic_vsh", "discard_fsh");
        gpu.Full(Green);
        gpu.Finish();
        gpu.Expect(8, 16, Black);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"fragment_derivatives", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "derivative_fsh");
        gpu.Full(White);
        gpu.Finish();
        // 1/32 per pixel, times 8. uam multiplies what dFdy reads by a Y factor from the shader's own
        // constants (c[1][0]) and leaves it 0 in the dksh, so both dFdy terms are 0: green is 0 and
        // blue only its 0.5 offset.
        gpu.Expect(16, 16, {0.25, 0, 0.5, 1}, 0.02);
    }});
    scenes.push_back({"interpolation", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA16_Unorm);
        gpu.Shaders("interp_vsh", "interp_fsh");
        // Value 0 with w 1 on the left, value 1 with w 3 on the right.
        const Vertex vertices[4] = {{{-1, 1, 0}, {0, 0, 0, 0}, {0, 1}},
                                    {{-1, -1, 0}, {0, 0, 0, 0}, {0, 1}},
                                    {{1, 1, 0}, {0, 0, 0, 0}, {1, 3}},
                                    {{1, -1, 0}, {0, 0, 0, 0}, {1, 3}}};
        gpu.BindVertices(vertices, 4);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Finish();
        for (uint32_t x : {4u, 16u, 28u}) {
            const double t = (x + 0.5) / 32.0;
            const double perspective = (t / 3.0) / ((1.0 - t) + t / 3.0);
            gpu.Check("noperspective", gpu.Read(x, 16).r, t, 0.005);
            gpu.Check("perspective", gpu.Read(x, 16).g, perspective, 0.005);
        }
    }});
    scenes.push_back({"provoking_vertex_last", [](Gpu& gpu) {
        gpu.Begin();
        gpu.Shaders("basic_vsh", "provoking_fsh");
        const Vertex vertices[3] = {{{-1, 1, 0}, {1, 0, 0, 1}, {0, 0}},
                                    {{-1, -3, 0}, {0, 1, 0, 1}, {0, 0}},
                                    {{3, 1, 0}, {0, 0, 1, 1}, {0, 0}}};
        gpu.BindVertices(vertices, 3);
        gpu.cmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(16, 16, Blue);
    }});
    scenes.push_back({"provoking_vertex_first", [](Gpu& gpu) {
        gpu.Begin();
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None).setProvokingVertex(DkProvokingVertex_First);
        gpu.cmd.bindRasterizerState(rasterizer);
        gpu.Shaders("basic_vsh", "provoking_fsh");
        const Vertex vertices[3] = {{{-1, 1, 0}, {1, 0, 0, 1}, {0, 0}},
                                    {{-1, -3, 0}, {0, 1, 0, 1}, {0, 0}},
                                    {{3, 1, 0}, {0, 0, 1, 1}, {0, 0}}};
        gpu.BindVertices(vertices, 3);
        gpu.cmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(16, 16, Red);
    }});
}
