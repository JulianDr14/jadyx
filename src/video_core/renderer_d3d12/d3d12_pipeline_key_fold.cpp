// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "video_core/gpu.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_fold.h"
#include "video_core/renderer_d3d12/d3d12_render_corrections.h"

namespace D3D12::PipelineKeyFold {
namespace {

using Key = GraphicsPipelineCacheKey;

constexpr u32 STENCIL_OPS_MASK = 0x00FFFFFF; ///< Front and back faces in DynamicState::raw2.
constexpr u32 ALWAYS = 7;                    ///< FixedPipelineState::PackComparisonOp's.

std::atomic<Mode> mode{Mode::Off};
std::atomic<u32> rules_mask{ALL_RULES};

[[nodiscard]] bool SameFetch(const MaxwellToD3D12::AttributeFetch& a,
                             const MaxwellToD3D12::AttributeFetch& b) {
    return a.parts == b.parts && a.part_components == b.part_components &&
           a.part_bytes == b.part_bytes && a.part_format == b.part_format;
}

/// D3D12 ignores the stencil operations without the test, and Describe has no test without a depth
/// target. The enable and the masks stay: the draw reads them to split the faces.
void FoldStencilOff(Key& key) {
    auto& dynamic = key.state.dynamic_state;
    if (dynamic.stencil_enable == 0 || key.state.depth_enabled == 0) {
        dynamic.raw2 &= ~STENCIL_OPS_MASK;
    }
}

/// A depth test that is off writes no depth, in the guest and in D3D12.
void FoldDepthWriteOff(Key& key) {
    auto& dynamic = key.state.dynamic_state;
    if (dynamic.depth_test_enable == 0) {
        dynamic.depth_write_enable.Assign(0);
    }
}

/// A target with no format or no channel to write has no blending to do. Alpha-to-coverage still
/// reads the first target's alpha, so that one stays.
void FoldMaskedTargets(Key& key) {
    for (size_t index = 0; index < key.state.attachments.size(); ++index) {
        auto& attachment = key.state.attachments[index];
        const auto mask = attachment.Mask();
        const bool writes = mask[0] || mask[1] || mask[2] || mask[3];
        const bool has_format = static_cast<Tegra::RenderTargetFormat>(
                                    key.state.color_formats[index]) !=
                                Tegra::RenderTargetFormat::NONE;
        if ((writes && has_format) || (index == 0 && key.state.alpha_to_coverage_enabled != 0)) {
            continue;
        }
        attachment.raw = 0;
    }
}

/// What RefreshStatic does to the keys of today's draws: the keys stored before it rounded could
/// never be asked for again, and every boot would still translate them.
void FoldDepthBias(Key& key) {
    const auto round_step = [](u32 bits) {
        return std::bit_cast<u32>(RoundDepthBiasStep(std::bit_cast<f32>(bits)));
    };
    if ((key.shader_flags & Key::SHADER_DEPTH_BIAS) == 0) {
        key.depth_bias = std::bit_cast<u32>(EffectiveDepthBias(std::bit_cast<f32>(key.depth_bias)));
    }
    key.depth_bias_clamp = round_step(key.depth_bias_clamp);
    key.slope_scaled_depth_bias = round_step(key.slope_scaled_depth_bias);
}

/// Strides go in the vertex buffer views: the pipeline only sees them through how an attribute is
/// fetched. A slot keeps its stride if dropping it would fetch one of its attributes differently.
void FoldVertexStrides(Key& key) {
    auto& state = key.state;
    std::array<bool, Maxwell::NumVertexArrays> keep{};
    for (const auto& attribute : state.attributes) {
        if (attribute.enabled == 0) {
            continue;
        }
        const u32 slot = attribute.buffer;
        if (!SameFetch(MaxwellToD3D12::SplitAttributeFetch(attribute.Type(), attribute.Size(),
                                                           attribute.offset,
                                                           state.vertex_strides[slot]),
                       MaxwellToD3D12::SplitAttributeFetch(attribute.Type(), attribute.Size(),
                                                           attribute.offset, 0))) {
            keep[slot] = true;
        }
    }
    for (size_t slot = 0; slot < keep.size(); ++slot) {
        if (!keep[slot]) {
            state.vertex_strides[slot] = 0;
        }
    }
}

/// Only a fragment shader tests alpha, and a test that always passes has no use for a reference.
void FoldAlphaRef(Key& key) {
    auto& state = key.state;
    if (key.unique_hashes[5] == 0) {
        state.alpha_test_func.Assign(ALWAYS);
    }
    if (state.alpha_test_func == ALWAYS) {
        state.alpha_test_ref = 0;
    }
}

/// State that only the stages a key lacks read.
void FoldUnusedStages(Key& key) {
    auto& state = key.state;
    if (key.unique_hashes[2] == 0 && key.unique_hashes[3] == 0) {
        state.tessellation_primitive.Assign(0);
        state.tessellation_spacing.Assign(0);
        state.tessellation_clockwise.Assign(0);
        state.patch_control_points_minus_one.Assign(0);
    }
    if (key.unique_hashes[5] == 0) {
        state.early_z.Assign(0);
        state.attachment0_dual_source_blend.Assign(0);
    }
}

/// State nothing reads while what it depends on is off, or that D3D12 never reads.
void FoldNoEffect(Key& key) {
    auto& state = key.state;
    auto& dynamic = state.dynamic_state;
    if (dynamic.cull_enable == 0) {
        dynamic.cull_face.Assign(0);
    }
    if (dynamic.logic_op_enable == 0) {
        dynamic.logic_op.Assign(0);
    }
    if (dynamic.depth_test_enable == 0) {
        dynamic.depth_test_func.Assign(0);
    }
    if (state.depth_enabled == 0) {
        state.depth_format.Assign(0);
    }
    if (Vulkan::FixedPipelineState::UnpackPolygonMode(state.polygon_mode) !=
        Maxwell::PolygonMode::Fill) {
        state.conservative_raster_enable.Assign(0); // D3D12 does not allow it with wireframe.
    }
    state.alpha_to_one_enabled.Assign(0);
    state.app_stage.Assign(static_cast<Tegra::Engines::Maxwell3D::EngineHint>(0));
    state.enabled_divisors = 0;

    std::array<bool, Maxwell::NumVertexArrays> used{};
    for (auto& attribute : state.attributes) {
        if (attribute.enabled == 0) {
            attribute.raw = 0;
        } else {
            used[attribute.buffer] = true;
        }
    }
    for (size_t slot = 0; slot < used.size(); ++slot) {
        if (!used[slot]) {
            state.binding_divisors[slot] = 0;
        }
    }
}

using RuleFunction = void (*)(Key&);
constexpr std::array<RuleFunction, NUM_RULES> RULE_FUNCTIONS{
    FoldStencilOff, FoldDepthWriteOff, FoldMaskedTargets, FoldDepthBias,
    FoldVertexStrides, FoldAlphaRef, FoldUnusedStages, FoldNoEffect,
};

} // Anonymous namespace

void SetMode(Mode new_mode) {
    mode.store(new_mode, std::memory_order_relaxed);
}

Mode GetMode() {
    return mode.load(std::memory_order_relaxed);
}

void SetRules(u32 rules) {
    rules_mask.store(rules, std::memory_order_relaxed);
}

u32 GetRules() {
    return rules_mask.load(std::memory_order_relaxed);
}

u32 ActiveRules() {
    return GetDepthBiasRounding() ? GetRules() : GetRules() & ~RuleBit(Rule::DepthBiasRound);
}

GraphicsPipelineCacheKey Fold(const GraphicsPipelineCacheKey& key, u32 rules) noexcept {
    GraphicsPipelineCacheKey folded = key;
    for (size_t index = 0; index < NUM_RULES; ++index) {
        if ((rules >> index) & 1) {
            RULE_FUNCTIONS[index](folded);
        }
    }
    return folded;
}

void Census::Add(const GraphicsPipelineCacheKey& key) {
    ++stored;
    raw.insert(key.Hash());
    folded.insert(Fold(key, rules).Hash());
    for (size_t index = 0; index < NUM_RULES; ++index) {
        alone[index].insert(Fold(key, 1U << index).Hash());
    }
    const Key rounded = Fold(key, RuleBit(Rule::DepthBiasRound));
    if (rounded.depth_bias != key.depth_bias || rounded.depth_bias_clamp != key.depth_bias_clamp ||
        rounded.slope_scaled_depth_bias != key.slope_scaled_depth_bias) {
        ++stale_bias;
    }
    ++per_set[Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                                 sizeof(key.unique_hashes))];
}

Census::Summary Census::Read() const {
    Summary summary{
        .stored = stored,
        .distinct = folded.size(),
        .sets = per_set.size(),
        .max_per_set = 0,
        .stale_bias = stale_bias,
        .saved_alone = {},
    };
    for (const auto& [set, count] : per_set) {
        summary.max_per_set = (std::max)(summary.max_per_set, count);
    }
    for (size_t index = 0; index < NUM_RULES; ++index) {
        summary.saved_alone[index] = raw.size() - alone[index].size();
    }
    return summary;
}

std::string Census::Describe() const {
    const Summary summary = Read();
    std::string text = fmt::format("{} stored keys, {} distinct once folded, {} with a depth bias "
                                   "today's draws cannot ask for, {} sets of shaders (at most {} "
                                   "keys for one); saved by each rule alone:",
                                   summary.stored, summary.distinct, summary.stale_bias,
                                   summary.sets, summary.max_per_set);
    for (size_t index = 0; index < NUM_RULES; ++index) {
        text += fmt::format(" {} {}", RULE_NAMES[index], summary.saved_alone[index]);
    }
    return text;
}

} // namespace D3D12::PipelineKeyFold
