// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Optimization {

void CollectShaderInfoPass(Environment& env, IR::Program& program);
void ConditionalBarrierPass(IR::Program& program);
void ConstantPropagationPass(Environment& env, IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
void GlobalMemoryToStorageBufferPass(IR::Program& program, const HostTranslateInfo& host_info);
void IdentityRemovalPass(IR::Program& program);
void LowerFp64ToFp32(IR::Program& program);
void LowerFp16ToFp32(IR::Program& program);
void LowerInt64ToInt32(IR::Program& program);
void QuadShufflePass(Environment& env, IR::Program& program);
void RescalingPass(IR::Program& program);
void SsaRewritePass(IR::Program& program);
void PositionPass(Environment& env, IR::Program& program);
/// Stage constants holding, per axis (u, v), the texture descriptors whose sampler mirrors once and
/// clamps to the border. Those samples take the coordinate's absolute value.
inline constexpr u32 MIRROR_BORDER_FIRST_CONSTANT = 6;
inline constexpr u32 MIRROR_BORDER_MAX_DESCRIPTORS = 24;
void MirrorBorderPass(IR::Program& program);
/// What the vertex shader finishes of an attribute D3D12's input assembler cannot fetch as asked.
struct VertexFetchFixup {
    enum class Normalize : u8 { None, Unsigned, Signed };
    bool swap_red_blue{};
    /// 32-bit normalized, fetched as integers. The components after `components` read 0, 0, 0, 1.
    Normalize normalize{};
    u8 components{4};
};
void VertexFetchPass(IR::Program& program, const std::array<VertexFetchFixup, 32>& fixups);
void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info);
void LayerPass(IR::Program& program, const HostTranslateInfo& host_info);
void VendorWorkaroundPass(IR::Program& program);
void VerificationPass(const IR::Program& program);

// Dual Vertex
void VertexATransformPass(IR::Program& program);
void VertexBTransformPass(IR::Program& program);
void JoinTextureInfo(Info& base, Info& source);
void JoinStorageInfo(Info& base, Info& source);

} // namespace Shader::Optimization
