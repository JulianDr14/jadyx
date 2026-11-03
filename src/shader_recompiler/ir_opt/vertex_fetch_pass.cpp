// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {

IR::F32 Normalize(IR::IREmitter& ir, const IR::U32& raw, VertexFetchFixup::Normalize mode) {
    if (mode == VertexFetchFixup::Normalize::Unsigned) {
        const IR::F32 value{ir.ConvertUToF(32, 32, raw)};
        return ir.FPMul(value, ir.Imm32(1.0f / 4294967295.0f));
    }
    const IR::F32 value{ir.ConvertSToF(32, 32, raw)};
    return IR::F32{ir.FPClamp(ir.FPMul(value, ir.Imm32(1.0f / 2147483647.0f)), ir.Imm32(-1.0f),
                              ir.Imm32(1.0f))};
}

} // Anonymous namespace

void VertexFetchPass(IR::Program& program, const std::array<VertexFetchFixup, 32>& fixups) {
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::GetAttribute) {
                continue;
            }
            IR::Attribute attribute{inst.Arg(0).Attribute()};
            if (!IR::IsGeneric(attribute)) {
                continue;
            }
            const VertexFetchFixup& fixup{fixups[IR::GenericAttributeIndex(attribute)]};
            u32 element{IR::GenericAttributeElement(attribute)};
            if (fixup.swap_red_blue && (element == 0 || element == 2)) {
                const u64 first{static_cast<u64>(attribute) - element};
                element = 2 - element;
                attribute = static_cast<IR::Attribute>(first + element);
                inst.SetArg(0, IR::Value{attribute});
                program.info.loads.Set(attribute, true);
            }
            if (fixup.normalize == VertexFetchFixup::Normalize::None) {
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            if (element >= fixup.components) {
                inst.ReplaceUsesWith(ir.Imm32(element == 3 ? 1.0f : 0.0f));
                continue;
            }
            const IR::F32 raw{ir.GetAttribute(attribute, IR::U32{inst.Arg(1)})};
            inst.ReplaceUsesWith(
                Normalize(ir, ir.BitCast<IR::U32, IR::F32>(raw), fixup.normalize));
        }
    }
    IdentityRemovalPass(program);
}

} // namespace Shader::Optimization
