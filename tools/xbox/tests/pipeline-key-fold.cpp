// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What each folding rule drops, what no rule may drop, and that folding settles in one pass.

#include <bit>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

#include "common/logging.h"
#include "video_core/gpu.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_fold.h"
#include "video_core/renderer_d3d12/d3d12_render_corrections.h"

namespace {

using namespace D3D12::PipelineKeyFold;
using Key = D3D12::GraphicsPipelineCacheKey;
using D3D12::Maxwell;
using Size = Maxwell::VertexAttribute::Size;
using Type = Maxwell::VertexAttribute::Type;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        throw std::runtime_error("FAIL " + what);
    }
}

constexpr u32 FILL = 2; // FixedPipelineState::PackPolygonMode(Fill)
constexpr u32 ALWAYS = 7;

Key Base() {
    Key key{};
    key.unique_hashes[1] = 0x11;
    key.unique_hashes[5] = 0x55;
    key.state.polygon_mode.Assign(FILL);
    key.state.depth_enabled.Assign(1);
    key.state.color_formats[0] = static_cast<u8>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM);
    key.state.attachments[0].mask_r.Assign(1);
    return key;
}

void SetAttribute(Key& key, size_t index, u32 slot, Type type, Size size, u32 offset) {
    auto& attribute = key.state.attributes[index];
    attribute.raw = 0;
    attribute.enabled.Assign(1);
    attribute.buffer.Assign(slot);
    attribute.type.Assign(static_cast<u32>(type));
    attribute.size.Assign(static_cast<u32>(size));
    attribute.offset.Assign(offset);
}

/// Two keys that differ as change says fold to one under the rule alone.
template <typename Change>
void Folds(Rule rule, const Key& base, Change&& change, const std::string& what) {
    Key other = base;
    change(other);
    Check(other != base, what + ": the keys differ to begin with");
    Check(Fold(base, RuleBit(rule)) == Fold(other, RuleBit(rule)), what);
    Check(Fold(base, ALL_RULES) == Fold(other, ALL_RULES), what + ", with every rule");
}

/// Two keys that differ as change says stay apart under every rule.
template <typename Change>
void Stays(const Key& base, Change&& change, const std::string& what) {
    Key other = base;
    change(other);
    Check(other != base, what + ": the keys differ to begin with");
    Check(Fold(base, ALL_RULES) != Fold(other, ALL_RULES), what);
}

void Rules() {
    Key off = Base();
    Folds(Rule::StencilOff, off, [](Key& k) { k.state.dynamic_state.front.test_func.Assign(3); },
          "stencil ops with the test off");
    Key no_depth = Base();
    no_depth.state.depth_enabled.Assign(0);
    no_depth.state.dynamic_state.stencil_enable.Assign(1);
    Folds(Rule::StencilOff, no_depth,
          [](Key& k) { k.state.dynamic_state.back.action_depth_pass.Assign(2); },
          "stencil ops without a depth target");
    Check(Fold(no_depth, ALL_RULES).state.dynamic_state.stencil_enable == 1,
          "the draw still reads the stencil enable");
    Key stencil = Base();
    stencil.state.dynamic_state.stencil_enable.Assign(1);
    Stays(stencil, [](Key& k) { k.state.dynamic_state.front.test_func.Assign(3); },
          "stencil ops with the test on");
    Key one_sided = stencil;
    one_sided.stencil_read_mask = 0xF0;
    Stays(one_sided, [](Key& k) { k.stencil_read_mask = 0x0F; }, "stencil masks with the test on");

    Folds(Rule::DepthWriteOff, Base(),
          [](Key& k) { k.state.dynamic_state.depth_write_enable.Assign(1); },
          "depth writes with the test off");
    Key tested = Base();
    tested.state.dynamic_state.depth_test_enable.Assign(1);
    Stays(tested, [](Key& k) { k.state.dynamic_state.depth_write_enable.Assign(1); },
          "depth writes with the test on");

    Key masked = Base();
    masked.state.color_formats[1] = static_cast<u8>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM);
    Folds(Rule::MaskedTarget, masked,
          [](Key& k) {
              k.state.attachments[1].enable.Assign(1);
              k.state.attachments[1].factor_source_rgb.Assign(4);
          },
          "blending on a target that writes nothing");
    Folds(Rule::MaskedTarget, Base(),
          [](Key& k) {
              k.state.attachments[3].mask_g.Assign(1);
              k.state.attachments[3].enable.Assign(1);
          },
          "blending on a target with no format");
    Stays(Base(),
          [](Key& k) {
              k.state.attachments[0].enable.Assign(1);
              k.state.attachments[0].factor_source_rgb.Assign(4);
          },
          "blending on a target that writes");
    Key coverage = Base();
    coverage.state.alpha_to_coverage_enabled.Assign(1);
    coverage.state.attachments[0].raw = 0;
    Stays(coverage,
          [](Key& k) {
              k.state.attachments[0].enable.Assign(1);
              k.state.attachments[0].factor_source_a.Assign(4);
          },
          "the first target under alpha-to-coverage");

    Key biased = Base();
    biased.state.dynamic_state.depth_bias_enable.Assign(1);
    biased.depth_bias = std::bit_cast<u32>(3.0f);
    biased.slope_scaled_depth_bias = std::bit_cast<u32>(1.01f);
    Folds(Rule::DepthBiasRound, biased,
          [](Key& k) {
              k.depth_bias = std::bit_cast<u32>(2.5f);
              k.slope_scaled_depth_bias = std::bit_cast<u32>(1.02f);
          },
          "bias stored before it was rounded");
    Stays(biased, [](Key& k) { k.depth_bias = std::bit_cast<u32>(6.0f); }, "a bias D3D12 applies");
    Stays(biased, [](Key& k) { k.slope_scaled_depth_bias = std::bit_cast<u32>(2.0f); },
          "a slope past its step");
    Key rounded = Fold(biased, RuleBit(Rule::DepthBiasRound));
    Check(std::bit_cast<f32>(rounded.depth_bias) == D3D12::EffectiveDepthBias(3.0f) &&
              std::bit_cast<f32>(rounded.slope_scaled_depth_bias) ==
                  D3D12::RoundDepthBiasStep(1.01f),
          "the rule gives what the draws' keys hold");
    Key in_shader = biased;
    in_shader.shader_flags |= Key::SHADER_DEPTH_BIAS;
    Check(Fold(in_shader, ALL_RULES).depth_bias == in_shader.depth_bias,
          "a bias in the shader keeps its fraction");

    Key plain = Base();
    SetAttribute(plain, 0, 0, Type::Float, Size::Size_R32_G32_B32, 0);
    Folds(Rule::VertexStrides, plain, [](Key& k) { k.state.vertex_strides[0] = 24; },
          "stride of a slot fetched whole");
    Folds(Rule::VertexStrides, plain, [](Key& k) { k.state.vertex_strides[7] = 16; },
          "stride of a slot no attribute reads");
    // 16-bit components at an offset of 2: a stride of 8 fetches them in parts, and so does 0.
    Key split = Base();
    SetAttribute(split, 0, 0, Type::UNorm, Size::Size_R16_G16_B16_A16, 2);
    split.state.vertex_strides[0] = 8;
    const Key split_folded = Fold(split, RuleBit(Rule::VertexStrides));
    Check(D3D12::MaxwellToD3D12::SplitAttributeFetch(Type::UNorm, Size::Size_R16_G16_B16_A16, 2, 8)
                  .parts ==
              D3D12::MaxwellToD3D12::SplitAttributeFetch(
                  Type::UNorm, Size::Size_R16_G16_B16_A16, 2, split_folded.state.vertex_strides[0])
                  .parts,
          "a fold keeps how an attribute is fetched");
    // An odd stride never splits, and dropping it would: it has to stay.
    Key odd = Base();
    SetAttribute(odd, 0, 0, Type::UNorm, Size::Size_R16_G16_B16_A16, 2);
    odd.state.vertex_strides[0] = 9;
    Check(Fold(odd, RuleBit(Rule::VertexStrides)).state.vertex_strides[0] == 9,
          "a stride whose drop would split the fetch stays");

    Key always = Base();
    always.state.alpha_test_func.Assign(ALWAYS);
    Folds(Rule::AlphaRef, always, [](Key& k) { k.state.alpha_test_ref = 0x3F000000; },
          "reference of a test that always passes");
    Key tests_alpha = Base();
    tests_alpha.state.alpha_test_func.Assign(1);
    Stays(tests_alpha, [](Key& k) { k.state.alpha_test_ref = 0x3F000000; },
          "reference of a test that can fail");
    Key no_fragment = Base();
    no_fragment.unique_hashes[5] = 0;
    Folds(Rule::AlphaRef, no_fragment,
          [](Key& k) {
              k.state.alpha_test_func.Assign(2);
              k.state.alpha_test_ref = 0x3F000000;
          },
          "alpha test without a fragment shader");

    Folds(Rule::UnusedStages, Base(),
          [](Key& k) { k.state.patch_control_points_minus_one.Assign(3); },
          "patch size without tessellation");
    Key tessellated = Base();
    tessellated.unique_hashes[3] = 0x33;
    Stays(tessellated, [](Key& k) { k.state.patch_control_points_minus_one.Assign(3); },
          "patch size with tessellation");
    Folds(Rule::UnusedStages, no_fragment, [](Key& k) { k.state.early_z.Assign(1); },
          "early z without a fragment shader");
    Stays(Base(), [](Key& k) { k.state.early_z.Assign(1); }, "early z with a fragment shader");

    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.dynamic_state.cull_face.Assign(1); },
          "cull face with culling off");
    Key culled = Base();
    culled.state.dynamic_state.cull_enable.Assign(1);
    Stays(culled, [](Key& k) { k.state.dynamic_state.cull_face.Assign(1); },
          "cull face with culling on");
    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.dynamic_state.logic_op.Assign(5); },
          "logic op switched off");
    Key logic = Base();
    logic.state.dynamic_state.logic_op_enable.Assign(1);
    Stays(logic, [](Key& k) { k.state.dynamic_state.logic_op.Assign(5); }, "logic op in use");
    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.dynamic_state.depth_test_func.Assign(3); },
          "depth function with the test off");
    Stays(tested, [](Key& k) { k.state.dynamic_state.depth_test_func.Assign(3); },
          "depth function with the test on");
    Key depthless = Base();
    depthless.state.depth_enabled.Assign(0);
    Folds(Rule::NoEffect, depthless, [](Key& k) { k.state.depth_format.Assign(4); },
          "depth format without a depth target");
    Stays(Base(), [](Key& k) { k.state.depth_format.Assign(4); },
          "depth format with a depth target");
    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.alpha_to_one_enabled.Assign(1); },
          "alpha to one, which D3D12 never reads");
    Folds(Rule::NoEffect, Base(),
          [](Key& k) { k.state.app_stage.Assign(Tegra::Engines::Maxwell3D::EngineHint(1)); },
          "the macro's hint, which D3D12 never reads");
    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.enabled_divisors = 5; },
          "the instancing bits, which D3D12 never reads");
    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.attributes[4].offset.Assign(8); },
          "a disabled attribute");
    Folds(Rule::NoEffect, Base(), [](Key& k) { k.state.binding_divisors[9] = 1; },
          "divisor of a slot no attribute reads");
    Key instanced = Base();
    SetAttribute(instanced, 0, 2, Type::Float, Size::Size_R32_G32_B32, 0);
    Stays(instanced, [](Key& k) { k.state.binding_divisors[2] = 1; }, "divisor of a slot in use");
    Key wire = Base();
    wire.state.polygon_mode.Assign(1);
    Folds(Rule::NoEffect, wire, [](Key& k) { k.state.conservative_raster_enable.Assign(1); },
          "conservative rasterization on wireframe");
    Stays(Base(), [](Key& k) { k.state.conservative_raster_enable.Assign(1); },
          "conservative rasterization on solid triangles");
    std::cout << "PASS rules: each drops its state only while it has no effect\n";
}

void NeverFolded() {
    const Key base = Base();
    Stays(base, [](Key& k) { k.unique_hashes[5] = 0x56; }, "shaders");
    Stays(base, [](Key& k) { k.state.msaa_mode.Assign(Tegra::Texture::MsaaMode::Msaa2x2); },
          "sample count");
    Stays(base, [](Key& k) { k.state.y_negate.Assign(1); }, "y negate");
    Stays(base, [](Key& k) { k.state.ndc_minus_one_to_one.Assign(1); }, "depth mode");
    Stays(base, [](Key& k) { k.state.polygon_mode.Assign(1); }, "polygon mode");
    Stays(base, [](Key& k) { k.state.topology.Assign(Maxwell::PrimitiveTopology::Lines); },
          "topology");
    Stays(base, [](Key& k) { k.state.dynamic_state.front_face.Assign(1); }, "front face");
    Stays(base, [](Key& k) { k.state.dynamic_state.depth_clamp_disabled.Assign(1); }, "depth clip");
    Stays(base, [](Key& k) { k.state.dynamic_state.rasterize_enable.Assign(1); }, "rasterization");
    Stays(base, [](Key& k) { k.state.dynamic_state.primitive_restart_enable.Assign(1); },
          "primitive restart");
    Stays(base, [](Key& k) { k.strip_cut = 1; }, "strip cut");
    Stays(base, [](Key& k) { k.shader_flags = Key::SHADER_REDUCTION; }, "shader flags");
    Stays(base, [](Key& k) { k.state.sample_mask_rest.Assign(1); }, "sample mask");
    Stays(base, [](Key& k) { k.state.dynamic_state.generated_lines.Assign(1); },
          "generated stages");
    Stays(base, [](Key& k) { k.state.dynamic_state.shader_mirror_border.Assign(1); },
          "mirror border");
    Stays(base, [](Key& k) { k.state.provoking_vertex_last.Assign(1); }, "provoking vertex");
    Stays(base, [](Key& k) {
        k.state.color_formats[0] = static_cast<u8>(Tegra::RenderTargetFormat::R8G8_UNORM);
    }, "format of a written target");
    Key attribute = base;
    SetAttribute(attribute, 0, 0, Type::Float, Size::Size_R32_G32_B32, 0);
    Stays(attribute, [](Key& k) { k.state.attributes[0].offset.Assign(4); },
          "offset of an enabled attribute");
    Key feedback = base;
    feedback.state.xfb_enabled.Assign(1);
    Stays(feedback, [](Key& k) { k.state.xfb_state.layouts[0].stride = 16; }, "transform feedback");
    Key line = base;
    line.state.dynamic_state.generated_lines.Assign(1);
    line.state.dynamic_state.line_stipple_enable.Assign(1);
    Stays(line, [](Key& k) { k.state.line_stipple_pattern = 0xFF; }, "stipple of generated lines");
    std::cout << "PASS never folded: what a pipeline shows stays in its key\n";
}

Key RandomKey(std::mt19937_64& random) {
    Key key;
    auto* const bytes = reinterpret_cast<u8*>(&key);
    for (size_t index = 0; index < sizeof(key); ++index) {
        bytes[index] = static_cast<u8>(random());
    }
    // Registers give a handful of values, so that bits often line up with the rules' conditions.
    for (auto& hash : key.unique_hashes) {
        hash = random() % 3 == 0 ? 0 : hash;
    }
    for (auto& value : key.state.color_formats) {
        value = random() % 2 == 0 ? 0 : value;
    }
    for (auto& attachment : key.state.attachments) {
        attachment.raw = random() % 2 == 0 ? attachment.raw & ~0xFU : attachment.raw;
    }
    key.state.attachments[0].enable.Assign(static_cast<u32>(random() % 2));
    return key;
}

void Settles() {
    std::mt19937_64 random{0x4A4144};
    constexpr int COUNT = 20000;
    for (int index = 0; index < COUNT; ++index) {
        const Key key = RandomKey(random);
        const Key folded = Fold(key, ALL_RULES);
        const Key again = Fold(folded, ALL_RULES);
        Check(std::memcmp(&folded, &again, sizeof(Key)) == 0, "folding twice changes nothing");
        Check(folded.Hash() == again.Hash(), "and hashes the same");
        Check(Fold(key, 0) == key, "no rule, no change");
        for (size_t rule = 0; rule < NUM_RULES; ++rule) {
            const Key once = Fold(key, 1U << rule);
            const Key twice = Fold(once, 1U << rule);
            Check(std::memcmp(&once, &twice, sizeof(Key)) == 0,
                  std::string(RULE_NAMES[rule]) + " settles in one pass");
        }
    }
    std::cout << "PASS settles: " << COUNT << " random keys fold in one pass\n";
}

void RulesInForce() {
    SetRules(ALL_RULES);
    D3D12::SetDepthBiasRounding(true);
    Check(ActiveRules() == ALL_RULES, "every rule with bias rounding on");
    D3D12::SetDepthBiasRounding(false);
    Check((ActiveRules() & RuleBit(Rule::DepthBiasRound)) == 0,
          "draws' keys are not rounded with the switch off, so stored ones must not be either");
    SetRules(RuleBit(Rule::StencilOff));
    D3D12::SetDepthBiasRounding(true);
    Check(ActiveRules() == RuleBit(Rule::StencilOff), "the mask chooses the rules");
    SetRules(ALL_RULES);
    Check(NUM_RULES == 8 && (ALL_RULES >> NUM_RULES) == 0, "the mask covers every rule");
    std::cout << "PASS rules in force: the mask and the bias switch\n";
}

void CensusCounts() {
    Census census{ALL_RULES};
    Key first = Base();
    Key same_folded = first;
    same_folded.state.dynamic_state.depth_write_enable.Assign(1);
    Key stale = first;
    stale.state.dynamic_state.depth_bias_enable.Assign(1);
    stale.slope_scaled_depth_bias = std::bit_cast<u32>(1.01f);
    Key other_set = first;
    other_set.unique_hashes[5] = 0x56;
    census.Add(first);
    census.Add(same_folded);
    census.Add(stale);
    census.Add(other_set);
    const Census::Summary summary = census.Read();
    Check(summary.stored == 4, "stored keys");
    Check(summary.distinct == 3, "distinct once folded");
    Check(summary.sets == 2, "sets of shaders");
    Check(summary.max_per_set == 3, "keys of the largest set");
    Check(summary.stale_bias == 1, "keys the draws can no longer ask for");
    Check(summary.saved_alone[static_cast<size_t>(Rule::DepthWriteOff)] == 1, "saved by one rule");
    Check(summary.saved_alone[static_cast<size_t>(Rule::AlphaRef)] == 0, "saved by another");
    Check(census.Describe().find("4 stored keys, 3 distinct") != std::string::npos,
          "what the log says");
    std::cout << "PASS census: the counts of the boot log\n";
}

} // Anonymous namespace

int main() try {
    Common::Log::Initialize();
    D3D12::SetDepthBiasRounding(true);
    Rules();
    NeverFolded();
    Settles();
    RulesInForce();
    CensusCounts();
    std::cout << "PASS pipeline key folding\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
