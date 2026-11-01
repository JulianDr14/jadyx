// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Depth and stencil: every comparison and operation, each depth format, and the less common switches.

#include "harness.h"
#include "scene_util.h"

namespace {

constexpr DkCompareOp CompareOps[] = {DkCompareOp_Never,   DkCompareOp_Less,     DkCompareOp_Equal,  DkCompareOp_Lequal,
                                      DkCompareOp_Greater, DkCompareOp_NotEqual, DkCompareOp_Gequal, DkCompareOp_Always};
constexpr const char* CompareNames[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};

bool Compare(DkCompareOp op, int a, int b) {
    switch (op) {
    case DkCompareOp_Never:
        return false;
    case DkCompareOp_Less:
        return a < b;
    case DkCompareOp_Equal:
        return a == b;
    case DkCompareOp_Lequal:
        return a <= b;
    case DkCompareOp_Greater:
        return a > b;
    case DkCompareOp_NotEqual:
        return a != b;
    case DkCompareOp_Gequal:
        return a >= b;
    default:
        return true;
    }
}

constexpr DkImageFormat DepthFormats[] = {DkImageFormat_Z16, DkImageFormat_Z24X8, DkImageFormat_Z24S8,
                                          DkImageFormat_ZF32, DkImageFormat_ZF32_X24S8};
constexpr const char* DepthNames[] = {"Z16", "Z24X8", "Z24S8", "ZF32", "ZF32_X24S8"};

dk::DepthStencilState DepthTest(DkCompareOp op, bool write = true) {
    dk::DepthStencilState state;
    state.setDepthTestEnable(true).setDepthWriteEnable(write).setDepthCompareOp(op);
    return state;
}

// Stencil is written with the first state and then read back as color: a second pass over the whole
// cell draws green only where the stored value equals `expected`.
dk::DepthStencilState StencilState(DkCompareOp compare, DkStencilOp fail, DkStencilOp pass, DkStencilOp depth_fail) {
    dk::DepthStencilState state;
    state.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilTestEnable(true);
    state.setStencilFrontCompareOp(compare).setStencilFrontFailOp(fail).setStencilFrontPassOp(pass);
    state.setStencilFrontDepthFailOp(depth_fail);
    state.setStencilBackCompareOp(compare).setStencilBackFailOp(fail).setStencilBackPassOp(pass);
    state.setStencilBackDepthFailOp(depth_fail);
    return state;
}

void ProbeStencil(Gpu& gpu, const Cell& cell, uint8_t expected) {
    gpu.cmd.bindDepthStencilState(StencilState(DkCompareOp_Equal, DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep));
    gpu.cmd.setStencil(DkFace_FrontAndBack, 0, expected, 0xFF);
    gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, Green);
}

struct StencilCase {
    const char* name;
    DkStencilOp op;
    uint8_t start;
    uint8_t ref;
    uint8_t expected;
};

constexpr StencilCase StencilOps[] = {
    {"keep", DkStencilOp_Keep, 7, 3, 7},
    {"zero", DkStencilOp_Zero, 7, 3, 0},
    {"replace", DkStencilOp_Replace, 7, 3, 3},
    {"incr", DkStencilOp_Incr, 7, 3, 8},
    {"incr_saturates", DkStencilOp_Incr, 255, 3, 255},
    {"decr", DkStencilOp_Decr, 7, 3, 6},
    {"decr_saturates", DkStencilOp_Decr, 0, 3, 0},
    {"invert", DkStencilOp_Invert, 0x0F, 3, 0xF0},
    {"incr_wraps", DkStencilOp_IncrWrap, 255, 3, 0},
    {"decr_wraps", DkStencilOp_DecrWrap, 0, 3, 255},
};

} // namespace

void RegisterDepthStencil(std::vector<SceneEntry>& scenes) {
    for (size_t i = 0; i < std::size(CompareOps); ++i) {
        const DkCompareOp op = CompareOps[i];
        scenes.push_back({Named("depth_compare_", CompareNames[i]), [op](Gpu& gpu) {
            gpu.BeginDepth(DkImageFormat_Z24S8);
            gpu.ClearColor(Black);
            gpu.cmd.clearDepthStencil(true, 0.5f, 0, 0);
            gpu.cmd.bindDepthStencilState(DepthTest(op, false));
            gpu.Rect(0, 0, 10, 32, Green, 0.25f);
            gpu.Rect(10, 0, 20, 32, Green, 0.5f);
            gpu.Rect(20, 0, 32, 32, Green, 0.75f);
            gpu.Finish();
            gpu.Expect(5, 16, Compare(op, 25, 50) ? Green : Black);
            gpu.Expect(15, 16, Compare(op, 50, 50) ? Green : Black);
            gpu.Expect(25, 16, Compare(op, 75, 50) ? Green : Black);
        }});
    }
    for (size_t i = 0; i < std::size(DepthFormats); ++i) {
        const DkImageFormat format = DepthFormats[i];
        scenes.push_back({Named("depth_format_", DepthNames[i]), [format](Gpu& gpu) {
            gpu.BeginDepth(format);
            gpu.ClearColor(Black);
            gpu.cmd.clearDepthStencil(true, 1.0f, 0, 0);
            gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Less));
            gpu.Rect(0, 0, 16, 32, Red, 0.25f);
            gpu.Rect(0, 0, 32, 32, Green, 0.5f);
            gpu.Rect(8, 0, 32, 32, Blue, 0.75f);
            gpu.Finish();
            gpu.Expect(4, 16, Red);
            gpu.Expect(12, 16, Red);
            gpu.Expect(24, 16, Green);
        }});
        scenes.push_back({Named("depth_sampled_", DepthNames[i]), [format](Gpu& gpu) {
            gpu.BeginDepth(format);
            Image& depth = *gpu.current_depth;
            gpu.cmd.clearDepthStencil(true, 1.0f, 0, 0);
            gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Always));
            gpu.Rect(0, 0, 16, 32, White, 0.25f);
            gpu.Rect(16, 0, 32, 32, White, 0.75f);
            gpu.Barrier();
            Image& result = gpu.Begin();
            dk::Sampler sampler;
            gpu.Shaders("basic_vsh", "texture_fsh");
            gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(depth, sampler));
            gpu.Full(White);
            gpu.Finish(&result);
            gpu.Check("left depth", gpu.Read(8, 16).r, 0.25, 0.005);
            gpu.Check("right depth", gpu.Read(24, 16).r, 0.75, 0.005);
        }});
        scenes.push_back({Named("shadow_sampler_", DepthNames[i]), [format](Gpu& gpu) {
            gpu.BeginDepth(format);
            Image& depth = *gpu.current_depth;
            gpu.cmd.clearDepthStencil(true, 1.0f, 0, 0);
            gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Always));
            gpu.Rect(0, 0, 16, 32, White, 0.25f);
            gpu.Rect(16, 0, 32, 32, White, 0.75f);
            gpu.Barrier();
            Image& result = gpu.Begin();
            dk::Sampler sampler;
            sampler.setDepthCompare(true, DkCompareOp_Less);
            gpu.Shaders("basic_vsh", "shadow_fsh");
            gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(depth, sampler));
            const float reference[4] = {0.5f, 0, 0, 0};
            gpu.Uniform(DkStage_Fragment, 0, reference, sizeof(reference));
            gpu.Full(White);
            gpu.Finish(&result);
            // Less: passes where the reference is under what is stored.
            gpu.Expect(8, 16, Black);
            gpu.Expect(24, 16, White);
        }});
    }
    scenes.push_back({"depth_write_off", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 1.0f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Less, false));
        gpu.Full(Red, 0.25f);
        gpu.Full(Green, 0.75f);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"depth_clamp", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 1.0f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Lequal));
        // Past the far plane: clipped without clamp and drawn at 1 with it.
        gpu.Rect(0, 0, 16, 32, Red, 1.5f);
        dk::RasterizerState clamped;
        clamped.setCullMode(DkFace_None).setDepthClampEnable(true);
        gpu.cmd.bindRasterizerState(clamped);
        gpu.Rect(16, 0, 32, 32, Green, 1.5f);
        gpu.Finish();
        gpu.Expect(8, 16, Black);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"depth_bias", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.5f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Less, false));
        gpu.Rect(0, 0, 16, 32, Red, 0.5f);
        dk::RasterizerState biased;
        biased.setCullMode(DkFace_None).setDepthBiasEnable(true);
        gpu.cmd.bindRasterizerState(biased);
        gpu.cmd.setDepthBias(-1000.0f, 0.0f, 0.0f);
        gpu.Rect(16, 0, 32, 32, Green, 0.5f);
        gpu.Finish();
        gpu.Expect(8, 16, Black);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"depth_bounds", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.1f, 0, 0);
        gpu.cmd.setDepthBounds(true, 0.25f, 0.75f);
        gpu.Rect(0, 0, 16, 32, Red);
        gpu.cmd.setDepthBounds(false, 0.0f, 1.0f);
        gpu.Rect(16, 0, 32, 32, Green);
        gpu.Finish();
        gpu.Expect(8, 16, Black);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"depth_from_shader", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.5f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Less, false));
        gpu.Shaders("basic_vsh", "depth_out_fsh");
        gpu.Full(Green, 1.0f);
        gpu.Finish();
        // The shader writes uv.x as depth: only the left half passes.
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"depth_viewport_range", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.75f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Less, false));
        gpu.cmd.setViewports(0, {{0.0f, 0.0f, 32.0f, 32.0f, 0.5f, 1.0f}});
        // z 0 lands on depth 0.5 and z 1 on depth 1.
        gpu.Rect(0, 0, 16, 32, Green, 0.0f);
        gpu.Rect(16, 0, 32, 32, Red, 1.0f);
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"depth_viewport_reversed", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_ZF32);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.5f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Greater, false));
        // The reversed depth games use: z 0 lands on depth 1.
        gpu.cmd.setViewports(0, {{0.0f, 0.0f, 32.0f, 32.0f, 1.0f, 0.0f}});
        gpu.Rect(0, 0, 16, 32, Green, 0.0f);
        gpu.Rect(16, 0, 32, 32, Red, 1.0f);
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"depth_early_test_counts", [](Gpu& gpu) {
        Slice counter = gpu.Alloc(16);
        std::memset(counter.cpu, 0, 16);
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.5f, 0, 0);
        gpu.cmd.bindDepthStencilState(DepthTest(DkCompareOp_Less, false));
        gpu.cmd.bindStorageBuffer(DkStage_Fragment, 0, counter.gpu, 16);
        gpu.Shaders("basic_vsh", "early_z_fsh");
        gpu.Rect(0, 0, 8, 8, Blue, 0.25f);    // Passes: 64 fragments.
        gpu.Rect(16, 16, 32, 32, Blue, 0.75f); // Fails, so it must not run.
        gpu.Finish();
        gpu.Check("fragments that ran", *static_cast<volatile uint32_t*>(counter.cpu), 64.0);
        gpu.Expect(4, 4, Blue);
        gpu.Expect(24, 24, Black);
    }});

    for (const StencilCase& test : StencilOps) {
        scenes.push_back({Named("stencil_op_", test.name), [test](Gpu& gpu) {
            gpu.BeginDepth(DkImageFormat_Z24S8);
            gpu.ClearColor(Black);
            gpu.cmd.clearDepthStencil(false, 0.0f, 0xFF, test.start);
            gpu.cmd.bindDepthStencilState(StencilState(DkCompareOp_Always, DkStencilOp_Keep, test.op, DkStencilOp_Keep));
            gpu.cmd.setStencil(DkFace_FrontAndBack, 0xFF, test.ref, 0xFF);
            gpu.Rect(0, 0, 32, 32, White);
            gpu.ClearColor(Black);
            ProbeStencil(gpu, {0, 0, 32}, test.expected);
            gpu.Finish();
            gpu.Expect(16, 16, Green);
        }});
    }
    for (size_t i = 0; i < std::size(CompareOps); ++i) {
        const DkCompareOp op = CompareOps[i];
        scenes.push_back({Named("stencil_compare_", CompareNames[i]), [op](Gpu& gpu) {
            gpu.BeginDepth(DkImageFormat_Z24S8);
            gpu.ClearColor(Black);
            gpu.cmd.clearDepthStencil(false, 0.0f, 0xFF, 5);
            gpu.cmd.bindDepthStencilState(StencilState(op, DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep));
            constexpr int Refs[] = {3, 5, 7};
            for (int r = 0; r < 3; ++r) {
                gpu.cmd.setStencil(DkFace_FrontAndBack, 0, static_cast<uint8_t>(Refs[r]), 0xFF);
                gpu.Rect(r * 10.0f, 0, r * 10.0f + 10.0f, 32, Green);
            }
            gpu.Finish();
            for (int r = 0; r < 3; ++r) {
                gpu.Expect(r * 10 + 5, 16, Compare(op, Refs[r], 5) ? Green : Black);
            }
        }});
    }
    scenes.push_back({"stencil_masks", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(false, 0.0f, 0xFF, 0xAA);
        // Only the low nibble may change, and only the low nibble is compared.
        gpu.cmd.bindDepthStencilState(StencilState(DkCompareOp_Equal, DkStencilOp_Keep, DkStencilOp_Replace, DkStencilOp_Keep));
        gpu.cmd.setStencil(DkFace_FrontAndBack, 0x0F, 0x5A, 0x0F);
        gpu.Rect(0, 0, 32, 32, Red);
        gpu.ClearColor(Black);
        ProbeStencil(gpu, {0, 0, 32}, 0xAA);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
    }});
    scenes.push_back({"stencil_separate_faces", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(false, 0.0f, 0xFF, 0);
        dk::DepthStencilState state;
        state.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilTestEnable(true);
        state.setStencilFrontCompareOp(DkCompareOp_Always).setStencilFrontPassOp(DkStencilOp_Replace);
        state.setStencilBackCompareOp(DkCompareOp_Always).setStencilBackPassOp(DkStencilOp_Replace);
        gpu.cmd.bindDepthStencilState(state);
        gpu.cmd.setStencil(DkFace_Front, 0xFF, 1, 0xFF);
        gpu.cmd.setStencil(DkFace_Back, 0xFF, 2, 0xFF);
        // Rect winds counterclockwise; the right half is drawn wound the other way.
        gpu.Rect(0, 0, 16, 32, White);
        gpu.Rect(32, 0, 16, 32, White);
        gpu.ClearColor(Black);
        ProbeStencil(gpu, {0, 0, 16}, 1);
        ProbeStencil(gpu, {16, 0, 16}, 2);
        gpu.Finish();
        gpu.Expect(8, 8, Green);
        gpu.Expect(24, 8, Green);
    }});
    scenes.push_back({"stencil_depth_fail", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.5f, 0xFF, 0);
        dk::DepthStencilState state =
            StencilState(DkCompareOp_Always, DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Replace);
        state.setDepthTestEnable(true).setDepthCompareOp(DkCompareOp_Less);
        gpu.cmd.bindDepthStencilState(state);
        gpu.cmd.setStencil(DkFace_FrontAndBack, 0xFF, 9, 0xFF);
        gpu.Rect(0, 0, 16, 32, White, 0.75f); // Fails depth: stencil becomes 9.
        gpu.Rect(16, 0, 32, 32, White, 0.25f); // Passes depth: stencil stays.
        gpu.ClearColor(Black);
        ProbeStencil(gpu, {0, 0, 16}, 9);
        ProbeStencil(gpu, {16, 0, 16}, 0);
        gpu.Finish();
        gpu.Expect(8, 8, Green);
        gpu.Expect(24, 8, Green);
    }});
    scenes.push_back({"stencil_sampled", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        Image& depth = *gpu.current_depth;
        gpu.cmd.clearDepthStencil(false, 0.0f, 0xFF, 0x40);
        gpu.cmd.bindDepthStencilState(StencilState(DkCompareOp_Always, DkStencilOp_Keep, DkStencilOp_Replace, DkStencilOp_Keep));
        gpu.cmd.setStencil(DkFace_FrontAndBack, 0xFF, 0x80, 0xFF);
        gpu.Rect(16, 0, 32, 32, White);
        gpu.Barrier();
        Image& result = gpu.Begin();
        dk::ImageView view{depth.image};
        view.setDsSource(DkDsSource_Stencil);
        dk::Sampler sampler;
        gpu.Shaders("basic_vsh", "utex_fsh");
        gpu.cmd.bindTextures(DkStage_Fragment, 0, gpu.Texture(view, sampler));
        gpu.Full(White);
        gpu.Finish(&result);
        gpu.Check("left stencil", gpu.Read(8, 16).r * 255.0, 64.0, 0.6);
        gpu.Check("right stencil", gpu.Read(24, 16).r * 255.0, 128.0, 0.6);
    }});
    scenes.push_back({"depth_stencil_clear_parts", [](Gpu& gpu) {
        gpu.BeginDepth(DkImageFormat_Z24S8);
        gpu.ClearColor(Black);
        gpu.cmd.clearDepthStencil(true, 0.25f, 0xFF, 3);
        // Depth only: stencil keeps its 3.
        gpu.cmd.clearDepthStencil(true, 0.75f, 0, 0);
        dk::DepthStencilState state = StencilState(DkCompareOp_Equal, DkStencilOp_Keep, DkStencilOp_Keep, DkStencilOp_Keep);
        state.setDepthTestEnable(true).setDepthCompareOp(DkCompareOp_Less);
        gpu.cmd.bindDepthStencilState(state);
        gpu.cmd.setStencil(DkFace_FrontAndBack, 0, 3, 0xFF);
        gpu.Rect(0, 0, 16, 32, Green, 0.5f);  // Under 0.75 and stencil 3: drawn.
        gpu.Rect(16, 0, 32, 32, Red, 0.9f);   // Over 0.75: not drawn.
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
}
