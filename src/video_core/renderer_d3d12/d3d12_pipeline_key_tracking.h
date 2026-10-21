// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_mode.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace D3D12::PipelineKeyTracking {

using Maxwell3D = Tegra::Engines::Maxwell3D;

/// Raised by a changed register that may feed the graphics pipeline key. The third dirty table
/// maps every register to it except IgnoredRegisters(), so a register nobody reviewed always
/// counts as relevant.
inline constexpr u8 KeyRegisters = Vulkan::Dirty::Last;
static_assert(KeyRegisters < Maxwell3D::DirtyState::Flags{}.size());

/// The dirty table CurrentGraphicsPipeline owns; Vulkan's state tracker fills the other two.
inline constexpr size_t TABLE_INDEX = 2;

struct RegisterRange {
    u32 first;
    u32 count;
};

/// Registers the key provably does not read: draw parameters (taken from the draw state or
/// the draw itself), buffer and texture addresses (resolved by the caches every draw), inline
/// uploads, clears, synchronization and macro scratch. The HLE macros also write these
/// directly, past the dirty tables. pipeline-key-tracking.cpp checks every one against the
/// real key refresh.
[[nodiscard]] std::span<const RegisterRange> IgnoredRegisters();

/// Maps every register to KeyRegisters except the ignored ones.
void SetupTable(Maxwell3D::DirtyState::Table& table);

/// What the key reads besides the tracked registers: the engine whose registers they are, the
/// draw's topology, the HLE macro hint and the stage hashes RefreshStages produced.
struct Inputs {
    const Maxwell3D* engine{};
    Maxwell3D::Regs::PrimitiveTopology topology{};
    Maxwell3D::EngineHint engine_state{};
    std::array<u64, 6> unique_hashes{};

    bool operator==(const Inputs&) const = default;
};

/// Decides whether the previous draw's pipeline still stands. Only a lookup that resolved to a
/// pipeline for exactly these inputs is remembered; a failed one forgets, so a draw that should
/// be skipped never reuses the pipeline before it.
class Reuse {
public:
    [[nodiscard]] bool Allows(const Inputs& inputs, bool key_registers_changed) const noexcept {
        return valid && !key_registers_changed && inputs == last;
    }
    void Remember(const Inputs& inputs) noexcept {
        last = inputs;
        valid = true;
    }
    void Forget() noexcept {
        valid = false;
    }

private:
    Inputs last{};
    bool valid{};
};

} // namespace D3D12::PipelineKeyTracking
