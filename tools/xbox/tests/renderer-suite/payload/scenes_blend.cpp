// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Blending, logic operations, write masks and the alpha test. What a pixel must read is computed with
// the blend equation itself, so every factor and operation pair is checked against the formula.

#include "harness.h"
#include "scene_util.h"

#include <algorithm>
#include <cmath>

double LinearToSrgb(double value) {
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

double SrgbToLinear(double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

namespace {

constexpr Color Src{0.6, 0.4, 0.2, 0.5};
constexpr Color Dst{0.2, 0.8, 0.5, 0.75};
constexpr Color Const{0.3, 0.6, 0.9, 0.7};
constexpr Color Src1{0.25, 0.5, 0.75, 0.5}; // What dual_fsh writes as its second output.

struct Factor {
    DkBlendFactor value;
    const char* name;
};

constexpr Factor Factors[] = {
    {DkBlendFactor_Zero, "Zero"},
    {DkBlendFactor_One, "One"},
    {DkBlendFactor_SrcColor, "SrcColor"},
    {DkBlendFactor_InvSrcColor, "InvSrcColor"},
    {DkBlendFactor_SrcAlpha, "SrcAlpha"},
    {DkBlendFactor_InvSrcAlpha, "InvSrcAlpha"},
    {DkBlendFactor_DstAlpha, "DstAlpha"},
    {DkBlendFactor_InvDstAlpha, "InvDstAlpha"},
    {DkBlendFactor_DstColor, "DstColor"},
    {DkBlendFactor_InvDstColor, "InvDstColor"},
    {DkBlendFactor_SrcAlphaSaturate, "SrcAlphaSaturate"},
    {DkBlendFactor_ConstColor, "ConstColor"},
    {DkBlendFactor_InvConstColor, "InvConstColor"},
    {DkBlendFactor_ConstAlpha, "ConstAlpha"},
    {DkBlendFactor_InvConstAlpha, "InvConstAlpha"},
};

double Channel(const Color& color, int channel) {
    return channel == 0 ? color.r : channel == 1 ? color.g : channel == 2 ? color.b : color.a;
}

double FactorValue(DkBlendFactor factor, int channel) {
    switch (factor) {
    case DkBlendFactor_Zero:
        return 0.0;
    case DkBlendFactor_One:
        return 1.0;
    case DkBlendFactor_SrcColor:
        return Channel(Src, channel);
    case DkBlendFactor_InvSrcColor:
        return 1.0 - Channel(Src, channel);
    case DkBlendFactor_SrcAlpha:
        return Src.a;
    case DkBlendFactor_InvSrcAlpha:
        return 1.0 - Src.a;
    case DkBlendFactor_DstAlpha:
        return Dst.a;
    case DkBlendFactor_InvDstAlpha:
        return 1.0 - Dst.a;
    case DkBlendFactor_DstColor:
        return Channel(Dst, channel);
    case DkBlendFactor_InvDstColor:
        return 1.0 - Channel(Dst, channel);
    case DkBlendFactor_SrcAlphaSaturate:
        return channel == 3 ? 1.0 : std::min(Src.a, 1.0 - Dst.a);
    case DkBlendFactor_ConstColor:
        return Channel(Const, channel);
    case DkBlendFactor_InvConstColor:
        return 1.0 - Channel(Const, channel);
    case DkBlendFactor_ConstAlpha:
        return Const.a;
    case DkBlendFactor_InvConstAlpha:
        return 1.0 - Const.a;
    case DkBlendFactor_Src1Color:
        return Channel(Src1, channel);
    case DkBlendFactor_InvSrc1Color:
        return 1.0 - Channel(Src1, channel);
    case DkBlendFactor_Src1Alpha:
        return Src1.a;
    case DkBlendFactor_InvSrc1Alpha:
        return 1.0 - Src1.a;
    default:
        return 0.0;
    }
}

double Equation(DkBlendOp op, double src, double dst, double src_factor, double dst_factor) {
    double value = 0.0;
    switch (op) {
    case DkBlendOp_Add:
        value = src * src_factor + dst * dst_factor;
        break;
    case DkBlendOp_Sub:
        value = src * src_factor - dst * dst_factor;
        break;
    case DkBlendOp_RevSub:
        value = dst * dst_factor - src * src_factor;
        break;
    case DkBlendOp_Min:
        value = std::min(src, dst);
        break;
    case DkBlendOp_Max:
        value = std::max(src, dst);
        break;
    }
    return std::clamp(value, 0.0, 1.0);
}

Color Blended(DkBlendOp color_op, DkBlendOp alpha_op, DkBlendFactor src_color, DkBlendFactor dst_color,
              DkBlendFactor src_alpha, DkBlendFactor dst_alpha) {
    Color out;
    out.r = Equation(color_op, Src.r, Dst.r, FactorValue(src_color, 0), FactorValue(dst_color, 0));
    out.g = Equation(color_op, Src.g, Dst.g, FactorValue(src_color, 1), FactorValue(dst_color, 1));
    out.b = Equation(color_op, Src.b, Dst.b, FactorValue(src_color, 2), FactorValue(dst_color, 2));
    out.a = Equation(alpha_op, Src.a, Dst.a, FactorValue(src_alpha, 3), FactorValue(dst_alpha, 3));
    return out;
}

void DrawBlended(Gpu& gpu, const Cell& cell, DkBlendOp color_op, DkBlendOp alpha_op, DkBlendFactor src_color,
                 DkBlendFactor dst_color, DkBlendFactor src_alpha, DkBlendFactor dst_alpha) {
    dk::ColorState color_state;
    gpu.cmd.bindColorState(color_state);
    gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, Dst);
    color_state.setBlendEnable(0, true);
    gpu.cmd.bindColorState(color_state);
    dk::BlendState blend;
    blend.setOps(color_op, alpha_op);
    blend.setFactors(src_color, dst_color, src_alpha, dst_alpha);
    gpu.cmd.bindBlendStates(0, blend);
    gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, Src);
}

void BlendSrcFactor(Gpu& gpu, const Factor& src) {
    gpu.Begin();
    gpu.ClearColor({0, 0, 0, 0});
    gpu.cmd.setBlendConst(Const.r, Const.g, Const.b, Const.a);
    uint32_t index = 0;
    for (const Factor& dst : Factors) {
        DrawBlended(gpu, CellOf(index++), DkBlendOp_Add, DkBlendOp_Add, src.value, dst.value, src.value, dst.value);
    }
    gpu.Finish();
    index = 0;
    for (const Factor& dst : Factors) {
        const Cell cell = CellOf(index++);
        gpu.Expect(cell.CenterX(), cell.CenterY(),
                   Blended(DkBlendOp_Add, DkBlendOp_Add, src.value, dst.value, src.value, dst.value));
    }
}

constexpr DkBlendOp Ops[] = {DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_RevSub, DkBlendOp_Min, DkBlendOp_Max};

const char* OpName(DkBlendOp op) {
    switch (op) {
    case DkBlendOp_Add:
        return "add";
    case DkBlendOp_Sub:
        return "sub";
    case DkBlendOp_RevSub:
        return "revsub";
    case DkBlendOp_Min:
        return "min";
    default:
        return "max";
    }
}

uint32_t LogicResult(DkLogicOp op, uint32_t s, uint32_t d) {
    switch (op) {
    case DkLogicOp_Clear:
        return 0;
    case DkLogicOp_And:
        return s & d;
    case DkLogicOp_AndReverse:
        return s & ~d;
    case DkLogicOp_Copy:
        return s;
    case DkLogicOp_AndInverted:
        return ~s & d;
    case DkLogicOp_NoOp:
        return d;
    case DkLogicOp_Xor:
        return s ^ d;
    case DkLogicOp_Or:
        return s | d;
    case DkLogicOp_Nor:
        return ~(s | d);
    case DkLogicOp_Equivalent:
        return ~(s ^ d);
    case DkLogicOp_Invert:
        return ~d;
    case DkLogicOp_OrReverse:
        return s | ~d;
    case DkLogicOp_CopyInverted:
        return ~s;
    case DkLogicOp_OrInverted:
        return ~s | d;
    case DkLogicOp_Nand:
        return ~(s & d);
    default:
        return 0xFFFFFFFFu;
    }
}

constexpr DkLogicOp LogicOps[] = {
    DkLogicOp_Clear,        DkLogicOp_And,  DkLogicOp_AndReverse, DkLogicOp_Copy,
    DkLogicOp_AndInverted,  DkLogicOp_NoOp, DkLogicOp_Xor,        DkLogicOp_Or,
    DkLogicOp_Nor,          DkLogicOp_Equivalent, DkLogicOp_Invert, DkLogicOp_OrReverse,
    DkLogicOp_CopyInverted, DkLogicOp_OrInverted, DkLogicOp_Nand,  DkLogicOp_Set,
};

bool Compare(DkCompareOp op, double a, double b) {
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

constexpr DkCompareOp CompareOps[] = {DkCompareOp_Never,   DkCompareOp_Less,     DkCompareOp_Equal,  DkCompareOp_Lequal,
                                      DkCompareOp_Greater, DkCompareOp_NotEqual, DkCompareOp_Gequal, DkCompareOp_Always};

} // namespace

void RegisterBlend(std::vector<SceneEntry>& scenes) {
    for (const Factor& src : Factors) {
        scenes.push_back({Named("blend_src_", src.name), [src](Gpu& gpu) { BlendSrcFactor(gpu, src); }});
    }
    for (DkBlendOp color_op : Ops) {
        for (DkBlendOp alpha_op : Ops) {
            std::string name = std::string{"blend_op_"} + OpName(color_op) + "_" + OpName(alpha_op);
            scenes.push_back({name, [color_op, alpha_op](Gpu& gpu) {
                gpu.Begin();
                gpu.ClearColor({0, 0, 0, 0});
                const DkBlendFactor one = DkBlendFactor_One, half = DkBlendFactor_SrcAlpha;
                DrawBlended(gpu, CellOf(0, 2, 16), color_op, alpha_op, one, one, one, one);
                DrawBlended(gpu, CellOf(1, 2, 16), color_op, alpha_op, half, DkBlendFactor_InvSrcAlpha, half,
                            DkBlendFactor_InvSrcAlpha);
                gpu.Finish();
                gpu.Expect(8, 8, Blended(color_op, alpha_op, one, one, one, one));
                gpu.Expect(24, 8, Blended(color_op, alpha_op, half, DkBlendFactor_InvSrcAlpha, half,
                                          DkBlendFactor_InvSrcAlpha));
            }});
        }
    }
    scenes.push_back({"blend_separate_alpha", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor({0, 0, 0, 0});
        // deko3d writes the color's destination factor as the alpha's too (dkCmdBufBindBlendStates), so
        // only the alpha equation and source factor are separate; the scene asks for what is sent.
        DrawBlended(gpu, CellOf(0, 1, 32), DkBlendOp_Add, DkBlendOp_RevSub, DkBlendFactor_SrcAlpha,
                    DkBlendFactor_InvSrcAlpha, DkBlendFactor_Zero, DkBlendFactor_InvSrcAlpha);
        gpu.Finish();
        gpu.Expect(16, 16, Blended(DkBlendOp_Add, DkBlendOp_RevSub, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
                                   DkBlendFactor_Zero, DkBlendFactor_InvSrcAlpha));
    }});
    scenes.push_back({"blend_dual_source", [](Gpu& gpu) {
        struct Pair {
            DkBlendFactor src, dst;
        };
        constexpr Pair Pairs[] = {
            {DkBlendFactor_One, DkBlendFactor_Src1Color},
            {DkBlendFactor_One, DkBlendFactor_InvSrc1Color},
            {DkBlendFactor_Src1Alpha, DkBlendFactor_InvSrc1Alpha},
            {DkBlendFactor_Src1Color, DkBlendFactor_Zero},
        };
        gpu.Begin();
        gpu.ClearColor({0, 0, 0, 0});
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask, {gpu.Shader("basic_vsh"), gpu.Shader("color_fsh")});
        uint32_t index = 0;
        for (const Pair& pair : Pairs) {
            const Cell cell = CellOf(index++, 2, 16);
            dk::ColorState color_state;
            gpu.cmd.bindColorState(color_state);
            gpu.Shaders("basic_vsh", "color_fsh");
            gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, Dst);
            color_state.setBlendEnable(0, true);
            gpu.cmd.bindColorState(color_state);
            dk::BlendState blend;
            blend.setFactors(pair.src, pair.dst, pair.src, pair.dst);
            gpu.cmd.bindBlendStates(0, blend);
            gpu.Shaders("basic_vsh", "dual_fsh");
            gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, Src);
        }
        gpu.Finish();
        index = 0;
        for (const Pair& pair : Pairs) {
            const Cell cell = CellOf(index++, 2, 16);
            gpu.Expect(cell.CenterX(), cell.CenterY(),
                       Blended(DkBlendOp_Add, DkBlendOp_Add, pair.src, pair.dst, pair.src, pair.dst));
        }
    }});
    scenes.push_back({"blend_srgb_target", [](Gpu& gpu) {
        // Blending happens in linear light and the result is encoded when written.
        gpu.Begin(DkImageFormat_RGBA8_Unorm_sRGB);
        gpu.ClearColor({0, 0, 0, 1});
        gpu.Rect(0, 0, 32, 32, {0.2, 0.2, 0.2, 1.0});
        dk::ColorState color_state;
        color_state.setBlendEnable(0, true);
        gpu.cmd.bindColorState(color_state);
        dk::BlendState blend;
        blend.setFactors(DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_Zero);
        gpu.cmd.bindBlendStates(0, blend);
        gpu.Rect(0, 0, 32, 32, {0.8, 0.8, 0.8, 0.5});
        gpu.Finish();
        const double encoded = LinearToSrgb(0.8 * 0.5 + 0.2 * 0.5);
        // The alpha's destination factor is the color's (deko3d): 0.5 * 1 + 1 * (1 - 0.5).
        gpu.Expect(16, 16, {encoded, encoded, encoded, 1.0}, 3.0 / 255.0);
    }});
    scenes.push_back({"blend_float_target_not_clamped", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA16_Float);
        gpu.ClearColor({1.5, 1.5, 1.5, 1.0});
        dk::ColorState color_state;
        color_state.setBlendEnable(0, true);
        gpu.cmd.bindColorState(color_state);
        dk::BlendState blend;
        blend.setFactors(DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_Zero);
        gpu.cmd.bindBlendStates(0, blend);
        gpu.Full({1.25, 0.25, 0.5, 1.0});
        gpu.Finish();
        // The alpha's destination factor is the color's (deko3d): 1 * 1 + 1 * 1.
        gpu.Expect(16, 16, {2.75, 1.75, 2.0, 2.0}, 0.002);
    }});
    scenes.push_back({"logic_op", [](Gpu& gpu) {
        constexpr uint32_t S = 0xAC, D = 0xCA;
        const Color src{S / 255.0, S / 255.0, S / 255.0, S / 255.0};
        const Color dst{D / 255.0, D / 255.0, D / 255.0, D / 255.0};
        gpu.Begin();
        gpu.ClearColor({0, 0, 0, 0});
        uint32_t index = 0;
        for (DkLogicOp op : LogicOps) {
            const Cell cell = CellOf(index++);
            dk::ColorState color_state;
            gpu.cmd.bindColorState(color_state);
            gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, dst);
            color_state.setLogicOp(op);
            gpu.cmd.bindColorState(color_state);
            gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, src);
        }
        gpu.Finish();
        index = 0;
        for (DkLogicOp op : LogicOps) {
            const Cell cell = CellOf(index++);
            const double value = (LogicResult(op, S, D) & 0xFF) / 255.0;
            gpu.Expect(cell.CenterX(), cell.CenterY(), {value, value, value, value}, 0.6 / 255.0);
        }
    }});
    scenes.push_back({"write_mask", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor({0.25, 0.25, 0.25, 0.25});
        for (uint32_t mask = 0; mask < 16; ++mask) {
            const Cell cell = CellOf(mask);
            dk::ColorWriteState write;
            write.setMask(0, mask);
            gpu.cmd.bindColorWriteState(write);
            gpu.Rect(cell.x, cell.y, cell.x + cell.size, cell.y + cell.size, {0.75, 0.75, 0.75, 0.75});
        }
        gpu.Finish();
        for (uint32_t mask = 0; mask < 16; ++mask) {
            const Cell cell = CellOf(mask);
            const auto pick = [&](uint32_t bit) { return (mask & bit) ? 0.75 : 0.25; };
            gpu.Expect(cell.CenterX(), cell.CenterY(),
                       {pick(DkColorMask_R), pick(DkColorMask_G), pick(DkColorMask_B), pick(DkColorMask_A)});
        }
    }});
    scenes.push_back({"mrt_outputs_and_masks", [](Gpu& gpu) {
        Image& first = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        Image& second = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        gpu.UseTarget(first);
        dk::ImageView view_first{first.image}, view_second{second.image};
        gpu.cmd.bindRenderTargets({&view_first, &view_second});
        gpu.ClearColor({0, 0, 0, 0}, 0);
        gpu.ClearColor({0, 0, 0, 0}, 1);
        gpu.Shaders("basic_vsh", "mrt_fsh");
        gpu.Rect(0, 0, 16, 32, {1.0, 0.5, 0.25, 1.0});
        dk::ColorWriteState write;
        write.setMask(0, DkColorMask_R).setMask(1, DkColorMask_B | DkColorMask_A);
        gpu.cmd.bindColorWriteState(write);
        gpu.Rect(16, 0, 32, 32, {1.0, 0.5, 0.25, 1.0});
        gpu.Finish(&first);
        gpu.Expect(8, 16, {1.0, 0.5, 0.25, 1.0});
        gpu.Expect(24, 16, {1.0, 0.0, 0.0, 0.0});
        gpu.Finish(&second);
        gpu.Expect(8, 16, {0.25, 0.5, 1.0, 1.0});
        gpu.Expect(24, 16, {0.0, 0.0, 1.0, 1.0});
    }});
    scenes.push_back({"mrt_independent_blend", [](Gpu& gpu) {
        Image& first = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        Image& second = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        gpu.UseTarget(first);
        dk::ImageView view_first{first.image}, view_second{second.image};
        gpu.cmd.bindRenderTargets({&view_first, &view_second});
        gpu.ClearColor({0.5, 0.5, 0.5, 1.0}, 0);
        gpu.ClearColor({0.5, 0.5, 0.5, 1.0}, 1);
        gpu.Shaders("basic_vsh", "mrt_fsh");
        dk::ColorState color_state;
        color_state.setBlendEnable(0, true).setBlendEnable(1, false);
        gpu.cmd.bindColorState(color_state);
        dk::BlendState add;
        add.setFactors(DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_One, DkBlendFactor_Zero);
        gpu.cmd.bindBlendStates(0, add);
        gpu.Full({0.25, 0.25, 0.25, 1.0});
        gpu.Finish(&first);
        gpu.Expect(16, 16, {0.75, 0.75, 0.75, 1.0});
        gpu.Finish(&second);
        gpu.Expect(16, 16, {0.25, 0.25, 0.25, 1.0});
    }});
    scenes.push_back({"mrt_different_formats", [](Gpu& gpu) {
        Image& first = gpu.MakeImage(DkImageFormat_RGBA8_Unorm, TargetSize, TargetSize);
        Image& second = gpu.MakeImage(DkImageFormat_RGBA16_Float, TargetSize, TargetSize);
        gpu.UseTarget(first);
        dk::ImageView view_first{first.image}, view_second{second.image};
        gpu.cmd.bindRenderTargets({&view_first, &view_second});
        gpu.Shaders("basic_vsh", "mrt_fsh");
        gpu.Full({0.25, 0.5, 2.0, 1.0});
        gpu.Finish(&first);
        gpu.Expect(16, 16, {0.25, 0.5, 1.0, 1.0});
        gpu.Finish(&second);
        gpu.Expect(16, 16, {2.0, 0.5, 0.25, 1.0}, 0.002);
    }});
    for (DkCompareOp op : CompareOps) {
        scenes.push_back({Named("alpha_test_", std::to_string(static_cast<int>(op)).c_str()), [op](Gpu& gpu) {
            gpu.Begin();
            gpu.ClearColor(Black);
            dk::ColorState color_state;
            color_state.setAlphaCompareOp(op);
            gpu.cmd.bindColorState(color_state);
            gpu.cmd.setAlphaRef(0.5f);
            // The alpha comes from a uniform: an interpolated 0.5 can land a hair below the reference,
            // and the equal cases would test the interpolator instead of the comparison.
            gpu.Shaders("basic_vsh", "ubo_fsh");
            constexpr double Alphas[] = {0.25, 0.5, 0.75};
            for (int i = 0; i < 3; ++i) {
                const float value[4] = {0, 1, 0, static_cast<float>(Alphas[i])};
                gpu.Uniform(DkStage_Fragment, 0, value, sizeof(value));
                gpu.Rect(i * 8.0f, 0, i * 8.0f + 8.0f, 32, White);
            }
            gpu.Finish();
            for (int i = 0; i < 3; ++i) {
                const bool passes = Compare(op, Alphas[i], 0.5);
                gpu.Expect(i * 8 + 4, 16, passes ? Color{0, 1, 0, Alphas[i]} : Black);
            }
        }});
    }
}
