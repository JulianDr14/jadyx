// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
// SHFL mask 0x1c03: clamp 3 inside segments of the lanes sharing bits 2..4, the 2x2 pixel quad.
constexpr u32 QUAD_CLAMP = 3;
constexpr u32 QUAD_SEGMENT_MASK = 28;

void LowerQuadShuffle(IR::Block& block, IR::Inst& inst) {
    const IR::Value index{inst.Arg(1).Resolve()};
    const IR::Value clamp{inst.Arg(2).Resolve()};
    const IR::Value segmentation_mask{inst.Arg(3).Resolve()};
    if (!index.IsImmediate() || !clamp.IsImmediate() || !segmentation_mask.IsImmediate()) {
        return;
    }
    if (clamp.U32() != QUAD_CLAMP || segmentation_mask.U32() != QUAD_SEGMENT_MASK) {
        return;
    }
    const u32 lane{index.U32()};
    const bool is_index{inst.GetOpcode() == IR::Opcode::ShuffleIndex};
    // Indices past 31 leave the warp, and butterflies past 3 leave the quad: both keep the
    // lane's own value when out of bounds, which only the generic shuffle reports.
    if (is_index ? lane > 31 : (lane < 1 || lane > 3)) {
        return;
    }
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::U32 value{inst.Arg(0)};
    const IR::U32 result{is_index ? ir.QuadBroadcast(value, ir.Imm32(lane & 3))
                                  : ir.QuadSwap(value, ir.Imm32(lane - 1))};
    if (IR::Inst* const in_bounds{
            inst.GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)}) {
        in_bounds->ReplaceUsesWith(IR::Value{true});
    }
    inst.ReplaceUsesWith(result);
}
} // Anonymous namespace

/// Turns fragment shuffles that stay inside the pixel quad into quad operations.
///
/// Maxwell runs helper pixels (quad lanes outside the primitive) as ordinary threads, so a
/// shader may read its quad leader with SHFL even when that pixel belongs to the neighbouring
/// triangle. Host wave shuffles skip helper lanes (D3D12 before SM 6.7) and read garbage there,
/// along every triangle edge; quad operations include them. Runs after constant propagation so
/// indices and masks from registers (RZ, constants moved into registers) are recognised too,
/// and after the derivative folds that match the generic shuffles.
void QuadShufflePass(Environment& env, IR::Program& program) {
    if (env.ShaderStage() != Stage::Fragment) {
        return;
    }
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::ShuffleIndex:
            case IR::Opcode::ShuffleButterfly:
                LowerQuadShuffle(*block, inst);
                break;
            default:
                break;
            }
        }
    }
}

} // namespace Shader::Optimization
