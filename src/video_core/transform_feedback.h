// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/engines/maxwell_3d.h"

namespace VideoCommon {

struct TransformFeedbackState {
    struct Layout {
        u32 stream;
        u32 varying_count;
        u32 stride;
    };
    std::array<Layout, Tegra::Engines::Maxwell3D::Regs::NumTransformFeedbackBuffers> layouts;
    std::array<std::array<Tegra::Engines::Maxwell3D::Regs::StreamOutLayout, 32>,
               Tegra::Engines::Maxwell3D::Regs::NumTransformFeedbackBuffers>
        varyings;

    /// Takes the layouts from regs. A disabled buffer gets none, as it captures nothing: its
    /// stale layout stays out of the pipeline key, and the pipeline declares no capture for it
    /// (stream output into an unbound slot 0 loses every buffer's capture on some drivers).
    void Refresh(const Tegra::Engines::Maxwell3D::Regs& regs);
};

/// The captures of a transform feedback layout, as the shader recompiler takes them.
struct TransformFeedbackVaryings {
    std::array<Shader::TransformFeedbackVarying, 256> varyings{};
    u32 count{};
    std::vector<Shader::TransformFeedbackRepeat> repeats;

    /// Whether any component of generic index is captured.
    [[nodiscard]] bool CapturesGeneric(u32 index) const;

    /// Sets the captures of the stage that feeds transform feedback.
    void Apply(Shader::RuntimeInfo& info) const;
};

[[nodiscard]] TransformFeedbackVaryings MakeTransformFeedbackVaryings(
    const TransformFeedbackState& state);

} // namespace VideoCommon
