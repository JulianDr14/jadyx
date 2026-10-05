// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <unordered_set>
#include <vector>
#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

namespace D3D12 {
constexpr u32 ASTC_GUEST_BINDING_OFFSET = 4096;
constexpr u32 ASTC_GUEST_METADATA_SPACE = 29;
void SetAstcGuestSampling(bool enabled);
bool AstcGuestSamplingEnabled();
struct AstcGuestShader {
    std::vector<u32> code;
    std::unordered_set<u32> bindings;
    u32 decoder_calls{};
};
// Native operations are retained for fallback. Unsupported operations make the entire texture
// binding native; descriptors must never silently substitute approximate gather/aniso semantics.
AstcGuestShader PatchAstcGuestShader(std::vector<u32> code, const Shader::Info& info,
                                     u32 first_binding, bool fragment,
                                     const std::unordered_set<u32>* allowed_descriptors = nullptr);
} // namespace D3D12
