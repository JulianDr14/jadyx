// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Validate the boot.cfg render corrections: their host math, the pipeline key bits, the fp16
// lowering marks and the SPIR-V each one adds, linked through Mesa to DXIL.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <spirv-tools/libspirv.h>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "video_core/gpu.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_render_corrections.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/texture_cache/decode_bc.h"
#include "video_core/texture_cache/types.h"

using namespace Shader;

namespace {

void Check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("FAIL " + what);
}

IR::Block* Block(IR::Program& p, ObjectPool<IR::Inst>& insts, ObjectPool<IR::Block>& blocks) {
    auto* b = blocks.Create(insts);
    p.blocks.push_back(b);
    p.post_order_blocks.push_back(b);
    return b;
}

void Append(IR::Program& p, IR::Block* b) {
    IR::AbstractSyntaxNode n;
    n.type = IR::AbstractSyntaxNode::Type::Block;
    n.data.block = b;
    p.syntax_list.push_back(n);
}

struct Counts {
    unsigned kills{}, ceils{}, clamps{}, abs{}, is_nan{}, to_uint{}, frag_coord{};
    unsigned frag_depth{}, depth_replacing{}, dpdx{};
};

Counts Scan(const std::vector<u32>& code) {
    Counts c;
    for (std::size_t i = 5; i < code.size(); i += code[i] >> 16) {
        const u32 op = code[i] & 0xffff;
        c.kills += op == 252;
        c.is_nan += op == 156;
        c.to_uint += op == 109;
        if (op == 12) {
            const u32 ext = code[i + 4];
            c.abs += ext == 4;
            c.ceils += ext == 9;
            c.clamps += ext == 43;
        }
        c.frag_coord += op == 71 && code[i + 2] == 11 && code[i + 3] == 15;
        c.frag_depth += op == 71 && code[i + 2] == 11 && code[i + 3] == 22;
        c.depth_replacing += op == 16 && code[i + 2] == 12;
        c.dpdx += op == 207;
    }
    return c;
}

class Pipeline {
public:
    using Body = std::function<void(IR::IREmitter&, IR::Program&)>;

    Pipeline() {
        validator = spvContextCreate(SPV_ENV_VULKAN_1_1);
        if (!compiler.IsAvailable()) throw std::runtime_error("compiler unavailable");
    }
    ~Pipeline() {
        spvContextDestroy(validator);
    }

    /// Emits a pass-through vertex shader and a fragment shader from body; returns its SPIR-V
    /// after validating it and linking both stages to DXIL.
    std::vector<u32> Fragment(const Profile& profile, RuntimeInfo pr, const Body& body,
                              const std::string& what) {
        ObjectPool<IR::Inst> insts;
        ObjectPool<IR::Block> blocks;
        IR::Program vs, ps;
        vs.stage = Stage::VertexB;
        ps.stage = Stage::Fragment;
        auto* vb = Block(vs, insts, blocks);
        Append(vs, vb);
        vs.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        IR::IREmitter v{*vb};
        v.Prologue();
        for (u32 c = 0; c < 4; ++c) {
            auto a = v.GetAttribute(IR::Attribute::Generic0X + c);
            v.SetAttribute(IR::Attribute::PositionX + c, a, v.Imm32(0U));
            v.SetAttribute(IR::Attribute::Generic0X + c, a, v.Imm32(0U));
            vs.info.loads.Set(IR::Attribute::Generic0X + c);
            vs.info.stores.Set(IR::Attribute::PositionX + c);
            vs.info.stores.Set(IR::Attribute::Generic0X + c);
        }
        v.Epilogue();
        auto* fb = Block(ps, insts, blocks);
        Append(ps, fb);
        IR::IREmitter f{*fb};
        f.Prologue();
        for (u32 c = 0; c < 4; ++c) {
            ps.info.loads.Set(IR::Attribute::Generic0X + c);
        }
        body(f, ps);
        f.Epilogue();
        ps.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        ps.info.stores_frag_color[0] = true;
        RuntimeInfo vr;
        vr.previous_stage_stores.mask.set();
        pr.previous_stage_stores = vs.info.stores;
        Backend::Bindings vertex_bindings, pixel_bindings;
        const auto vertex = Backend::SPIRV::EmitSPIRV(profile, vr, vs, vertex_bindings);
        const auto pixel = Backend::SPIRV::EmitSPIRV(profile, pr, ps, pixel_bindings);
        spv_diagnostic diagnostic{};
        if (spvValidateBinary(validator, pixel.data(), pixel.size(), &diagnostic) != SPV_SUCCESS) {
            const std::string error = diagnostic ? diagnostic->error : "invalid SPIR-V";
            spvDiagnosticDestroy(diagnostic);
            throw std::runtime_error("FAIL " + what + ": " + error);
        }
        spvDiagnosticDestroy(diagnostic);
        const D3D12::ShaderCompiler::PipelineStage stages[]{
            {vertex, DXIL_SPIRV_SHADER_VERTEX}, {pixel, DXIL_SPIRV_SHADER_FRAGMENT}};
        Check(compiler.CompilePipeline(stages, {}).size() == 2, what + " links to DXIL");
        return pixel;
    }

private:
    D3D12::ShaderCompiler compiler;
    spv_context validator{};
};

IR::F32 Input(IR::IREmitter& f, u32 component) {
    return IR::F32{f.GetAttribute(IR::Attribute::Generic0X + component)};
}

void WriteInputs(IR::IREmitter& f, IR::Program&) {
    for (u32 c = 0; c < 4; ++c) {
        f.SetFragColor(0, c, Input(f, c));
    }
}

void HostMath() {
    using namespace D3D12;
    for (u32 k = 0; k <= 255; ++k) {
        Check(RoundUnorm8HalfDown(k / 255.0f) == k / 255.0f, "unorm step " + std::to_string(k));
        if (k < 255) {
            Check(RoundUnorm8HalfDown((k + 0.5f) / 255.0f) == k / 255.0f,
                  "unorm tie rounds down at " + std::to_string(k));
            Check(RoundUnorm8HalfDown((k + 0.6f) / 255.0f) == (k + 1) / 255.0f,
                  "unorm above tie rounds up at " + std::to_string(k));
        }
    }
    Check(RoundUnorm8HalfDown(0.5f) == 127.0f / 255.0f, "unorm 0.5 is 127");
    Check(RoundUnorm8HalfDown(-1.0f) == 0.0f && RoundUnorm8HalfDown(2.0f) == 1.0f,
          "unorm clamps");

    Check(AlphaToCoverageThreshold(0, 0, 0) == 0.5f && AlphaToCoverageThreshold(1, 1, 1) == 0.5f &&
              AlphaToCoverageThreshold(3, 1, 0) == 0.5f,
          "a2c threshold without dither");
    Check(AlphaToCoverageThreshold(2, 0, 0) == 0.125f && AlphaToCoverageThreshold(2, 1, 0) == 0.625f &&
              AlphaToCoverageThreshold(2, 0, 1) == 0.875f &&
              AlphaToCoverageThreshold(2, 1, 1) == 0.375f && AlphaToCoverageThreshold(2, 2, 2) == 0.125f,
          "a2c 2x2 dither");

    using Tegra::RenderTargetFormat;
    for (const auto format : {RenderTargetFormat::A8R8G8B8_UNORM, RenderTargetFormat::A8B8G8R8_UNORM,
                              RenderTargetFormat::R8G8_UNORM, RenderTargetFormat::R8_UNORM}) {
        Check(IsUnorm8RenderTarget(static_cast<u32>(format)), "unorm8 target");
    }
    for (const auto format : {RenderTargetFormat::A8B8G8R8_SRGB, RenderTargetFormat::R16G16B16A16_FLOAT,
                              RenderTargetFormat::A2B10G10R10_UNORM, RenderTargetFormat::R8_SNORM}) {
        Check(!IsUnorm8RenderTarget(static_cast<u32>(format)), "not a unorm8 target");
    }

    Check(EffectiveDepthBias(3.0f) == 2.0f && EffectiveDepthBias(-3.0f) == -2.0f &&
              EffectiveDepthBias(1.9f) == 0.0f,
          "effective depth bias");
    Check(RoundDepthBiasStep(0.53f) == 0.5f, "depth bias step");
    const std::array<f32, 4> grey{0.3f, 0.6f, 0.2f, 0.7f};
    Check(CorrectBorderColor(grey, BorderColorMode::Exact) == grey, "border exact");
    const auto bw = CorrectBorderColor(grey, BorderColorMode::BlackWhite);
    Check(bw == std::array<f32, 4>{0.0f, 0.0f, 0.0f, 1.0f}, "border black");

    const auto toggles = {std::pair{&SetUnormHalfDownRounding, &GetUnormHalfDownRounding},
                          std::pair{&SetCubeDirectionSanitize, &GetCubeDirectionSanitize},
                          std::pair{&SetHalfFloatFlush, &GetHalfFloatFlush},
                          std::pair{&SetShaderAlphaToCoverage, &GetShaderAlphaToCoverage},
                          std::pair{&SetSmallBcGuestPalette, &GetSmallBcGuestPalette},
                          std::pair{&SetShaderDepthBias, &GetShaderDepthBias}};
    for (const auto& [set, get] : toggles) {
        Check(!get(), "switch off by default");
        set(true);
        Check(get(), "switch on");
        set(false);
    }
    SetSmallBcGuestPalette(true);
    Check(VideoCommon::GetBcGuestPalette(), "small_bc_palette turns the guest palette on");
    SetSmallBcGuestPalette(false);
    Check(!VideoCommon::GetBcGuestPalette(), "small_bc_palette turns the guest palette off");

    using VideoCore::Surface::PixelFormat;
    Check(IsSmallGuestPaletteBc(PixelFormat::BC1_RGBA_UNORM, 8) &&
              IsSmallGuestPaletteBc(PixelFormat::BC3_SRGB, SMALL_BC_GUEST_PALETTE_BYTES),
          "small BC1-3 decoded with the guest palette");
    Check(!IsSmallGuestPaletteBc(PixelFormat::BC1_RGBA_UNORM, SMALL_BC_GUEST_PALETTE_BYTES + 1) &&
              !IsSmallGuestPaletteBc(PixelFormat::BC2_UNORM, 0) &&
              !IsSmallGuestPaletteBc(PixelFormat::BC4_UNORM, 8) &&
              !IsSmallGuestPaletteBc(PixelFormat::BC7_UNORM, 8),
          "big, empty or other BC textures keep the native path");

    Check(DepthBiasUnit(0x0A) == 0.0f && DepthBiasUnit(0x19) == 0.0f, "float depth unit");
    Check(DepthBiasUnit(0x13) == 0x1p-16f, "D16 unit");
    Check(DepthBiasUnit(0x14) == 0x1p-24f && DepthBiasUnit(0x16) == 0x1p-24f, "D24 unit");
    std::cout << "PASS host math: UNORM8 ties, a2c thresholds, formats, depth units, switches\n";
}

void PipelineKey() {
    using Maxwell = D3D12::Maxwell;
    auto regs = std::make_unique<Maxwell>();
    regs->alpha_to_coverage_dither = Maxwell::AlphaToCoverageDither::Footprint_2x2;
    D3D12::GraphicsPipelineCacheKey key{};
    key.state.alpha_to_coverage_enabled.Assign(1);
    key.state.msaa_mode.Assign(Tegra::Texture::MsaaMode::Msaa1x1);
    key.RefreshStatic(*regs);
    Check(key.ShaderAlphaToCoverage() == 0, "a2c stays in the PSO with the switch off");
    D3D12::SetShaderAlphaToCoverage(true);
    key.RefreshStatic(*regs);
    Check(key.ShaderAlphaToCoverage() == 2, "a2c 2x2 footprint in the key");
    regs->alpha_to_coverage_dither = Maxwell::AlphaToCoverageDither::Footprint_1x1_Virtual;
    key.RefreshStatic(*regs);
    Check(key.ShaderAlphaToCoverage() == 3, "a2c virtual footprint in the key");
    key.shader_flags |= D3D12::GraphicsPipelineCacheKey::SHADER_REDUCTION;
    key.RefreshStatic(*regs);
    Check(key.ShaderAlphaToCoverage() == 3, "refresh keeps the a2c bits");
    key.state.msaa_mode.Assign(Tegra::Texture::MsaaMode::Msaa2x2);
    key.RefreshStatic(*regs);
    Check(key.ShaderAlphaToCoverage() == 0, "multisampled a2c stays in the PSO");
    key.state.msaa_mode.Assign(Tegra::Texture::MsaaMode::Msaa1x1);
    key.state.alpha_to_coverage_enabled.Assign(0);
    key.RefreshStatic(*regs);
    Check(key.ShaderAlphaToCoverage() == 0, "no a2c without the guest enabling it");
    D3D12::SetShaderAlphaToCoverage(false);

    // Depth bias in the shader.
    using Key = D3D12::GraphicsPipelineCacheKey;
    D3D12::GraphicsPipelineCacheKey biased{};
    biased.state.msaa_mode.Assign(Tegra::Texture::MsaaMode::Msaa1x1);
    biased.state.depth_enabled.Assign(1);
    biased.state.dynamic_state.depth_bias_enable.Assign(1);
    biased.unique_hashes[5] = 1;
    regs->depth_bias = 3.0f;
    regs->slope_scale_depth_bias = 0.0f;
    D3D12::SetDepthBiasRounding(true);
    biased.RefreshStatic(*regs);
    Check((biased.shader_flags & Key::SHADER_DEPTH_BIAS) == 0, "bias in the PSO with the switch off");
    Check(biased.depth_bias == std::bit_cast<u32>(2.0f), "PSO bias rounded");
    D3D12::SetShaderDepthBias(true);
    biased.RefreshStatic(*regs);
    Check((biased.shader_flags & Key::SHADER_DEPTH_BIAS) != 0, "bias in the shader");
    Check(biased.depth_bias == std::bit_cast<u32>(3.0f), "shader bias keeps the fraction");
    const auto without = [&](auto&& change, const std::string& what) {
        D3D12::GraphicsPipelineCacheKey other = biased;
        change(other);
        other.RefreshStatic(*regs);
        Check((other.shader_flags & Key::SHADER_DEPTH_BIAS) == 0, what);
    };
    without([](Key& k) { k.state.early_z.Assign(1); }, "early z keeps the PSO bias");
    without([](Key& k) { k.state.depth_enabled.Assign(0); }, "no depth target, no shader bias");
    without([](Key& k) { k.unique_hashes[5] = 0; }, "no fragment shader, no shader bias");
    without([](Key& k) { k.state.msaa_mode.Assign(Tegra::Texture::MsaaMode::Msaa2x2); },
            "multisampled bias stays in the PSO");
    without([](Key& k) { k.state.dynamic_state.depth_bias_enable.Assign(0); },
            "no bias without the guest enabling it");
    regs->depth_bias = 0.0f;
    without([](Key&) {}, "zero bias stays in the PSO");
    regs->slope_scale_depth_bias = 1.0f;
    biased.RefreshStatic(*regs);
    Check((biased.shader_flags & Key::SHADER_DEPTH_BIAS) != 0, "slope-only bias in the shader");
    D3D12::SetShaderDepthBias(false);
    D3D12::SetDepthBiasRounding(false);

    // The stipple of a generated line is in its shaders, so it splits only those pipelines.
    Key stippled{};
    stippled.state.dynamic_state.generated_lines.Assign(1);
    stippled.state.dynamic_state.line_stipple_enable.Assign(1);
    stippled.state.line_stipple_pattern = 0x00FF;
    Key other_pattern = stippled;
    other_pattern.state.line_stipple_pattern = 0x0F0F;
    Check(stippled != other_pattern, "generated lines with another stipple pattern differ");
    Check(stippled.Hash() != other_pattern.Hash(), "and hash apart");
    Key other_factor = stippled;
    other_factor.state.line_stipple_factor = 3;
    Check(stippled != other_factor, "generated lines with another stipple factor differ");
    Key same = stippled;
    Check(stippled == same && stippled.Hash() == same.Hash(), "equal stipple keeps the pipeline");
    Key disabled = stippled;
    disabled.state.dynamic_state.line_stipple_enable.Assign(0);
    Key disabled_other = disabled;
    disabled_other.state.line_stipple_pattern = 0x0F0F;
    Check(disabled == disabled_other && disabled.Hash() == disabled_other.Hash(),
          "a stipple not in use splits nothing");
    std::cout << "PASS pipeline key: a2c footprint, shader depth bias and line stipple only where "
                 "they apply\n";
}

std::array<u8, 4> DecodeBc1Texel(const std::array<u8, 8>& block) {
    std::array<u8, 64> rgba{};
    VideoCommon::BufferImageCopy copy{};
    copy.buffer_row_length = 4;
    copy.buffer_image_height = 4;
    copy.image_extent = {4, 4, 1};
    VideoCommon::DecompressBCn(block, rgba, copy, VideoCore::Surface::PixelFormat::BC1_RGBA_UNORM);
    for (std::size_t texel = 1; texel < 16; ++texel) {
        Check(std::equal(rgba.begin(), rgba.begin() + 4, rgba.begin() + texel * 4),
              "uniform BC1 block decodes uniform");
    }
    return {rgba[0], rgba[1], rgba[2], rgba[3]};
}

void BcPalette() {
    using Texel = std::array<u8, 4>;
    // White and black endpoints: thirds and halves in the guest's steps.
    const u8 white_black[8]{0xff, 0xff, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa};
    const auto four = VideoCommon::GuestBc1Palette(white_black, false);
    Check(four[0] == Texel{255, 255, 255, 255} && four[1] == Texel{0, 0, 0, 255},
          "BC1 endpoints");
    Check(four[2] == Texel{170, 175, 170, 255}, "BC1 two thirds of white");
    Check(four[3] == Texel{85, 80, 85, 255}, "BC1 one third of white");
    const u8 black_white[8]{0x00, 0x00, 0xff, 0xff, 0xaa, 0xaa, 0xaa, 0xaa};
    const auto three = VideoCommon::GuestBc1Palette(black_white, false);
    Check(three[2] == Texel{127, 128, 127, 255} && three[3] == Texel{0, 0, 0, 0},
          "BC1 three-color mode: half and transparent black");
    const auto forced = VideoCommon::GuestBc1Palette(black_white, true);
    Check(forced[2] == Texel{85, 80, 85, 255}, "BC2/BC3 colors never use the three-color mode");

    std::array<u8, 8> block{};
    std::copy(std::begin(white_black), std::end(white_black), block.begin());
    const Texel ideal = DecodeBc1Texel(block);
    Check(ideal[1] == ideal[0], "BC1 decodes the ideal palette by default");
    VideoCommon::SetBcGuestPalette(true);
    Check(DecodeBc1Texel(block) == Texel{170, 175, 170, 255}, "BC1 decodes the guest palette");
    VideoCommon::SetBcGuestPalette(false);
    Check(DecodeBc1Texel(block) == ideal, "BC1 back to the ideal palette");
    std::cout << "PASS BC palette: guest BC1 thirds and halves, CPU decode switch\n";
}

void HalfLowering() {
    for (const auto [mode, marked] : {std::pair{IR::FmzMode::FTZ, true},
                                      std::pair{IR::FmzMode::FMZ, true},
                                      std::pair{IR::FmzMode::None, false},
                                      std::pair{IR::FmzMode::DontCare, false}}) {
        ObjectPool<IR::Inst> insts;
        ObjectPool<IR::Block> blocks;
        IR::Program ps;
        ps.stage = Stage::Fragment;
        auto* b = Block(ps, insts, blocks);
        Append(ps, b);
        ps.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        IR::IREmitter f{*b};
        const auto x = f.FPConvert(16, Input(f, 0));
        const auto y = f.FPConvert(16, Input(f, 1));
        IR::FpControl control{};
        control.fmz_mode = mode;
        const auto add = f.FPAdd(x, y, control);
        const auto mul = f.FPMul(x, y, control);
        const auto fma = f.FPFma(x, y, add, control);
        const auto neg = f.FPNeg(mul);
        f.SetFragColor(0, 0, IR::F32{f.FPConvert(32, fma)});
        f.SetFragColor(0, 1, IR::F32{f.FPConvert(32, neg)});
        Optimization::LowerFp16ToFp32(ps);
        for (const auto& value : {add, mul, fma}) {
            const auto& inst = *value.Inst();
            const auto op = inst.GetOpcode();
            Check(op == IR::Opcode::FPAdd32 || op == IR::Opcode::FPMul32 || op == IR::Opcode::FPFma32,
                  "fp16 op lowered");
            Check(inst.Flags<IR::FpControl>().half_ftz == marked, "half_ftz mark");
            Check(inst.Flags<IR::FpControl>().fmz_mode == mode, "fmz mode kept");
        }
        Check(neg.Inst()->GetOpcode() == IR::Opcode::FPNeg32, "fp16 neg lowered");
    }
    std::cout << "PASS fp16 lowering: FTZ/FMZ add, mul and fma marked, others not\n";
}

void Emitters() {
    Pipeline pipeline;
    const Profile base_profile;
    const RuntimeInfo base_info;

    // UNORM8 ties-down rounding.
    const auto plain = pipeline.Fragment(base_profile, base_info, WriteInputs, "plain output");
    RuntimeInfo unorm = base_info;
    unorm.unorm8_output_mask = 1;
    const auto rounded = pipeline.Fragment(base_profile, unorm, WriteInputs, "unorm output");
    const auto plain_counts = Scan(plain), rounded_counts = Scan(rounded);
    Check(rounded_counts.ceils == plain_counts.ceils + 4 &&
              rounded_counts.clamps == plain_counts.clamps + 4,
          "unorm rounding per channel");
    unorm.unorm8_output_mask = 2;
    Check(pipeline.Fragment(base_profile, unorm, WriteInputs, "unorm unused target") == plain,
          "unorm mask of an unwritten target changes nothing");

    // Alpha to coverage in the shader.
    RuntimeInfo a2c = base_info;
    a2c.alpha_to_coverage_footprint = 1;
    const auto one = Scan(pipeline.Fragment(base_profile, a2c, WriteInputs, "a2c 1x1"));
    Check(one.kills == plain_counts.kills + 1 && one.frag_coord == plain_counts.frag_coord,
          "a2c 1x1 discards without the position");
    a2c.alpha_to_coverage_footprint = 2;
    const auto dither = Scan(pipeline.Fragment(base_profile, a2c, WriteInputs, "a2c 2x2"));
    Check(dither.kills == plain_counts.kills + 1 && dither.frag_coord == 1 &&
              dither.to_uint == plain_counts.to_uint + 2,
          "a2c 2x2 dithers on the pixel position");
    a2c.alpha_to_coverage_footprint = 3;
    const auto virtual_one = Scan(pipeline.Fragment(base_profile, a2c, WriteInputs, "a2c virtual"));
    Check(virtual_one.kills == plain_counts.kills + 1, "a2c virtual discards");

    // fp16 denormal flush on lowered math.
    const auto half_add = [](bool marked) {
        return [marked](IR::IREmitter& f, IR::Program&) {
            IR::FpControl control{};
            control.half_ftz = marked;
            f.SetFragColor(0, 0, IR::F32{f.FPAdd(Input(f, 0), Input(f, 1), control)});
            for (u32 c = 1; c < 4; ++c) {
                f.SetFragColor(0, c, Input(f, c));
            }
        };
    };
    Profile flush = base_profile;
    flush.flush_lowered_fp16_denorms = true;
    const auto marked_off = Scan(pipeline.Fragment(base_profile, base_info, half_add(true), "half off"));
    const auto marked_on = Scan(pipeline.Fragment(flush, base_info, half_add(true), "half on"));
    Check(marked_on.abs == marked_off.abs + 1, "marked add flushed");
    Check(pipeline.Fragment(flush, base_info, half_add(false), "fp32 add flush") ==
              pipeline.Fragment(base_profile, base_info, half_add(false), "fp32 add"),
          "unmarked fp32 math untouched");

    // Cube direction sanitizing.
    const auto sample = [](TextureType type) {
        return [type](IR::IREmitter& f, IR::Program& ps) {
            ps.info.texture_descriptors.push_back({.type = type, .count = 1});
            IR::TextureInstInfo info{};
            info.type.Assign(type);
            const IR::Value coords =
                type == TextureType::Color2D
                    ? f.CompositeConstruct(Input(f, 0), Input(f, 1))
                : type == TextureType::ColorCube
                    ? f.CompositeConstruct(Input(f, 0), Input(f, 1), Input(f, 2))
                    : f.CompositeConstruct(Input(f, 0), Input(f, 1), Input(f, 2), Input(f, 3));
            const auto color = f.ImageSampleImplicitLod(f.Imm32(0U), coords, {}, {}, {}, info);
            color.InstRecursive()->ReplaceOpcode(IR::Opcode::ImageSampleImplicitLod);
            for (u32 c = 0; c < 4; ++c) {
                f.SetFragColor(0, c, IR::F32{f.CompositeExtract(color, c)});
            }
        };
    };
    Profile sanitize = base_profile;
    sanitize.sanitize_cube_directions = true;
    for (const auto type : {TextureType::ColorCube, TextureType::ColorArrayCube}) {
        const auto raw = Scan(pipeline.Fragment(base_profile, base_info, sample(type), "cube"));
        const auto clean = Scan(pipeline.Fragment(sanitize, base_info, sample(type), "cube sanitized"));
        Check(clean.is_nan == raw.is_nan + 3, "cube direction NaNs replaced");
    }
    Check(pipeline.Fragment(sanitize, base_info, sample(TextureType::Color2D), "2d sanitized") ==
              pipeline.Fragment(base_profile, base_info, sample(TextureType::Color2D), "2d"),
          "2D samples untouched");

    // Depth bias in the shader.
    RuntimeInfo bias = base_info;
    bias.depth_bias_in_shader = true;
    bias.depth_bias_unit = 0x1p-24f;
    bias.depth_bias_constant = 1.5f;
    bias.depth_bias_slope = 1.0f;
    const auto unorm_bias = Scan(pipeline.Fragment(base_profile, bias, WriteInputs, "bias D24"));
    Check(plain_counts.frag_depth == 0 && plain_counts.depth_replacing == 0,
          "plain shader keeps its depth");
    Check(unorm_bias.frag_depth == 1 && unorm_bias.depth_replacing == 1 &&
              unorm_bias.frag_coord == 1 && unorm_bias.dpdx == plain_counts.dpdx + 1,
          "shader bias writes the rasterized depth plus its slope");
    bias.depth_bias_unit = 0.0f;
    bias.depth_bias_clamp = 0.001f;
    const auto float_bias = Scan(pipeline.Fragment(base_profile, bias, WriteInputs, "bias float"));
    Check(float_bias.frag_depth == 1 && float_bias.depth_replacing == 1, "float shader bias");
    bias.depth_bias_clamp = -0.001f;
    pipeline.Fragment(base_profile, bias, WriteInputs, "bias negative clamp");
    const auto writes_depth = [](IR::IREmitter& f, IR::Program& ps) {
        WriteInputs(f, ps);
        ps.info.stores_frag_depth = true;
        f.SetFragDepth(Input(f, 2));
    };
    Check(pipeline.Fragment(base_profile, bias, writes_depth, "own depth biased") ==
              pipeline.Fragment(base_profile, base_info, writes_depth, "own depth"),
          "a shader writing its depth keeps it");

    std::cout << "PASS emitters: UNORM8 rounding, a2c footprints, fp16 flush, cube sanitize, "
                 "depth bias; "
                 "SPIR-V valid, linked DXIL\n";
}

} // Anonymous namespace

int main() try {
    Common::Log::Initialize();
    HostMath();
    PipelineKey();
    HalfLowering();
    BcPalette();
    Emitters();
    std::cout << "PASS render corrections\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
