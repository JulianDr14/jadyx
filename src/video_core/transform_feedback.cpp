// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bitset>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include <ranges>
#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/transform_feedback.h"

namespace VideoCommon {

void TransformFeedbackState::Refresh(const Tegra::Engines::Maxwell3D::Regs& regs) {
    for (size_t index = 0; index < layouts.size(); ++index) {
        if (regs.transform_feedback.buffers[index].enable == 0) {
            layouts[index] = {};
            varyings[index] = {};
            continue;
        }
        const auto& control{regs.transform_feedback.controls[index]};
        layouts[index] = {
            .stream = control.stream,
            .varying_count = control.varying_count,
            .stride = control.stride,
        };
        varyings[index] = regs.stream_out_layout[index];
    }
}

bool TransformFeedbackVaryings::CapturesGeneric(u32 index) const {
    const auto first{varyings.begin() + static_cast<size_t>(Shader::IR::Attribute::Generic0X) +
                     index * 4};
    return std::any_of(first, first + 4, [](const auto& varying) { return varying.components; });
}

void TransformFeedbackVaryings::Apply(Shader::RuntimeInfo& info) const {
    info.xfb_varyings = varyings;
    info.xfb_count = count;
    info.xfb_repeats = repeats;
}

TransformFeedbackVaryings MakeTransformFeedbackVaryings(const TransformFeedbackState& state) {
    static constexpr std::array VECTORS{
        28U,  // gl_Position
        32U,  // Generic 0
        36U,  // Generic 1
        40U,  // Generic 2
        44U,  // Generic 3
        48U,  // Generic 4
        52U,  // Generic 5
        56U,  // Generic 6
        60U,  // Generic 7
        64U,  // Generic 8
        68U,  // Generic 9
        72U,  // Generic 10
        76U,  // Generic 11
        80U,  // Generic 12
        84U,  // Generic 13
        88U,  // Generic 14
        92U,  // Generic 15
        96U,  // Generic 16
        100U, // Generic 17
        104U, // Generic 18
        108U, // Generic 19
        112U, // Generic 20
        116U, // Generic 21
        120U, // Generic 22
        124U, // Generic 23
        128U, // Generic 24
        132U, // Generic 25
        136U, // Generic 26
        140U, // Generic 27
        144U, // Generic 28
        148U, // Generic 29
        152U, // Generic 30
        156U, // Generic 31
        160U, // gl_FrontColor
        164U, // gl_FrontSecondaryColor
        160U, // gl_BackColor
        164U, // gl_BackSecondaryColor
        192U, // gl_TexCoord[0]
        196U, // gl_TexCoord[1]
        200U, // gl_TexCoord[2]
        204U, // gl_TexCoord[3]
        208U, // gl_TexCoord[4]
        212U, // gl_TexCoord[5]
        216U, // gl_TexCoord[6]
        220U, // gl_TexCoord[7]
    };
    TransformFeedbackVaryings xfb{};
    // The attributes some entry of xfb.varyings captures: one entry per attribute holds one capture.
    std::bitset<256> covered;
    for (size_t buffer = 0; buffer < state.layouts.size(); ++buffer) {
        const auto& locations = state.varyings[buffer];
        const auto& layout = state.layouts[buffer];
        const u32 varying_count = layout.varying_count;
        u32 highest = 0;
        for (u32 offset = 0; offset < varying_count; ++offset) {
            const auto get_attribute = [&locations](u32 index) -> u32 {
                switch (index % 4) {
                case 0:
                    return locations[index / 4].attribute0.Value();
                case 1:
                    return locations[index / 4].attribute1.Value();
                case 2:
                    return locations[index / 4].attribute2.Value();
                case 3:
                    return locations[index / 4].attribute3.Value();
                }
                UNREACHABLE();
                return 0;
            };

            Shader::TransformFeedbackVarying varying{
                .buffer = static_cast<u32>(buffer),
                .stream = layout.stream,
                .stride = layout.stride,
                .offset = offset * 4,
                .components = 1,
            };
            const u32 base_offset = offset;
            const auto attribute{get_attribute(offset)};
            if (std::ranges::find(VECTORS, Common::AlignDown(attribute, 4)) != VECTORS.end()) {
                // Generics and the position may start past x; the legacy vectors may not.
                UNIMPLEMENTED_IF_MSG(attribute % 4 != 0 && attribute >= 160, "Unaligned TFB {}",
                                     attribute);

                // Only the next components of the same vector form one varying.
                while (offset + 1 < varying_count && (attribute + varying.components) % 4 != 0 &&
                       get_attribute(offset + 1) == attribute + varying.components) {
                    ++offset;
                    ++varying.components;
                }
            }
            bool repeated{};
            for (u32 component = 0; component < varying.components; ++component) {
                repeated |= covered[attribute + component];
            }
            if (repeated) {
                xfb.repeats.push_back({.attribute = attribute, .varying = varying});
            } else {
                for (u32 component = 0; component < varying.components; ++component) {
                    covered[attribute + component] = true;
                }
                xfb.varyings[attribute] = varying;
                xfb.count = (std::max)(xfb.count, attribute + 1);
            }
            highest = (std::max)(highest, (base_offset + varying.components) * 4);
        }
        UNIMPLEMENTED_IF(highest != layout.stride);
    }
    return xfb;
}

} // namespace VideoCommon
