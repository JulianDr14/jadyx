// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_mode.h"

/// Folding drops from a pipeline key the state its pipeline cannot show, so keys that differ only
/// there share one pipeline. The result is still a valid key: a rule taken back later only leaves
/// entries nothing asks for, and the disk cache needs no new version.
namespace D3D12::PipelineKeyFold {

/// One bit of the rule mask each. Each rule's reason is in the implementation.
enum class Rule : u32 {
    StencilOff,
    DepthWriteOff,
    MaskedTarget,
    DepthBiasRound,
    VertexStrides,
    AlphaRef,
    UnusedStages,
    NoEffect,
    Count,
};

inline constexpr size_t NUM_RULES = static_cast<size_t>(Rule::Count);
inline constexpr std::array<const char*, NUM_RULES> RULE_NAMES{
    "stencil_off", "depth_write", "masked_target", "bias_round",
    "strides",     "alpha_ref",   "unused_stages", "no_effect",
};

[[nodiscard]] constexpr u32 RuleBit(Rule rule) noexcept {
    return 1U << static_cast<u32>(rule);
}

/// The rules in force: GetRules without the depth bias one when rounding is off, as the keys of
/// the draws are not rounded then.
[[nodiscard]] u32 ActiveRules();

/// Pure: the same key and rules give the same result, and folding again changes nothing.
[[nodiscard]] GraphicsPipelineCacheKey Fold(const GraphicsPipelineCacheKey& key,
                                            u32 rules) noexcept;

/// What the keys of a disk cache come to: one thread feeds it while the cache is read.
class Census {
public:
    explicit Census(u32 rules_) : rules{rules_} {}

    void Add(const GraphicsPipelineCacheKey& key);

    struct Summary {
        u64 stored;
        u64 distinct;
        u64 sets;
        u64 max_per_set;
        u64 stale_bias;
        std::array<u64, NUM_RULES> saved_alone;
    };
    [[nodiscard]] Summary Read() const;
    [[nodiscard]] std::string Describe() const;

private:
    u32 rules;
    u64 stored{};
    u64 stale_bias{};
    std::unordered_set<u64> raw;
    std::unordered_set<u64> folded;
    std::array<std::unordered_set<u64>, NUM_RULES> alone;
    std::unordered_map<u64, u64> per_set;
};

} // namespace D3D12::PipelineKeyFold
