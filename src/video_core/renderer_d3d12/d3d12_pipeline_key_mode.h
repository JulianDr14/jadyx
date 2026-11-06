// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace D3D12::PipelineKeyTracking {

enum class Mode : u8 {
    Off,   ///< Refresh and compare the key on every draw, as before.
    On,    ///< Reuse the last pipeline while nothing the key reads changed.
    Check, ///< Refresh anyway and report every reuse that would have picked another pipeline.
};
void SetMode(Mode mode);
[[nodiscard]] Mode GetMode();

} // namespace D3D12::PipelineKeyTracking

namespace D3D12::PipelineKeyFold {

enum class Mode : u8 {
    Off,   ///< Every key is a pipeline of its own, as before.
    Check, ///< Pipelines as Off; logs what folding would save and compares the ones it would merge.
    On,    ///< Keys that differ only in state their pipeline cannot show share one pipeline.
};
void SetMode(Mode mode);
[[nodiscard]] Mode GetMode();

/// The mask of rules (PipelineKeyFold::Rule) folding may use.
inline constexpr u32 ALL_RULES = 0xFF;
void SetRules(u32 rules);
[[nodiscard]] u32 GetRules();

} // namespace D3D12::PipelineKeyFold
