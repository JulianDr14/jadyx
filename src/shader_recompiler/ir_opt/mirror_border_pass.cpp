// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {

// Which texture is mirrored depends on the bound sampler, so it comes from a stage constant: one
// mask per axis, written as a float so that no driver flushes a small mask as a denormal.
IR::F32 MirrorCoord(IR::IREmitter& ir, const IR::F32& coord, u32 axis, u32 descriptor) {
    const IR::U32 mask{ir.ConvertFToU(32, ir.StageConstant(MIRROR_BORDER_FIRST_CONSTANT + axis))};
    const IR::U1 mirrored{
        ir.INotEqual(ir.BitwiseAnd(mask, ir.Imm32(u32{1} << descriptor)), ir.Imm32(0U))};
    return IR::F32{ir.Select(mirrored, ir.FPAbs(coord), coord)};
}

void Rewrite(IR::Block& block, IR::Inst& inst) {
    const IR::TextureInstInfo info{inst.Flags<IR::TextureInstInfo>()};
    const u32 descriptor{info.descriptor_index};
    if (descriptor >= MIRROR_BORDER_MAX_DESCRIPTORS) {
        return;
    }
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value coords{inst.Arg(1)};
    switch (info.type) {
    case TextureType::Color1D:
        inst.SetArg(1, MirrorCoord(ir, IR::F32{coords}, 0, descriptor));
        break;
    case TextureType::ColorArray1D:
        inst.SetArg(1, ir.CompositeConstruct(
                           MirrorCoord(ir, IR::F32{ir.CompositeExtract(coords, 0)}, 0, descriptor),
                           ir.CompositeExtract(coords, 1)));
        break;
    case TextureType::Color2D:
    case TextureType::Color2DRect:
        inst.SetArg(1, ir.CompositeConstruct(
                           MirrorCoord(ir, IR::F32{ir.CompositeExtract(coords, 0)}, 0, descriptor),
                           MirrorCoord(ir, IR::F32{ir.CompositeExtract(coords, 1)}, 1, descriptor)));
        break;
    case TextureType::ColorArray2D:
    case TextureType::Color3D:
        inst.SetArg(1, ir.CompositeConstruct(
                           MirrorCoord(ir, IR::F32{ir.CompositeExtract(coords, 0)}, 0, descriptor),
                           MirrorCoord(ir, IR::F32{ir.CompositeExtract(coords, 1)}, 1, descriptor),
                           ir.CompositeExtract(coords, 2)));
        break;
    default:
        break; // A cube's coordinates are a direction.
    }
}

} // Anonymous namespace

void MirrorBorderPass(IR::Program& program) {
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::ImageSampleImplicitLod:
            case IR::Opcode::ImageSampleExplicitLod:
            case IR::Opcode::ImageSampleDrefImplicitLod:
            case IR::Opcode::ImageSampleDrefExplicitLod:
            case IR::Opcode::ImageGather:
            case IR::Opcode::ImageGatherDref:
                Rewrite(*block, inst);
                break;
            default:
                break;
            }
        }
    }
    program.info.uses_stage_constants = true;
}

} // namespace Shader::Optimization
