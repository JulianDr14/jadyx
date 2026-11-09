// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
/// Whether `value` is FindUMsb32 of a ballot: the leader lane of a warp-aggregated atomic.
bool IsBallotLeader(const IR::Value& value) {
    const IR::Value resolved{value.Resolve()};
    if (resolved.IsImmediate()) {
        return false;
    }
    const IR::Inst* const inst{resolved.InstRecursive()};
    if (inst->GetOpcode() != IR::Opcode::FindUMsb32) {
        return false;
    }
    const IR::Value ballot{inst->Arg(0).Resolve()};
    return !ballot.IsImmediate() &&
           ballot.InstRecursive()->GetOpcode() == IR::Opcode::SubgroupBallot;
}
} // Anonymous namespace

bool WarpPerLanePass(IR::Program& program) {
    std::vector<std::pair<IR::Block*, IR::Inst*>> ballots;
    std::vector<IR::Inst*> shuffles;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::SubgroupBallot:
                ballots.emplace_back(block, &inst);
                break;
            case IR::Opcode::ShuffleIndex:
                if (!IsBallotLeader(inst.Arg(1))) {
                    return false;
                }
                shuffles.push_back(&inst);
                break;
            case IR::Opcode::ShuffleUp:
            case IR::Opcode::ShuffleDown:
            case IR::Opcode::ShuffleButterfly:
            case IR::Opcode::VoteAll:
            case IR::Opcode::VoteAny:
            case IR::Opcode::VoteEqual:
                // Real exchanges between lanes: a warp of one lane would change their result.
                return false;
            default:
                break;
            }
        }
    }
    if (ballots.empty() || shuffles.empty()) {
        return false;
    }
    // Each lane's ballot holds only its own bit, so FindUMsb32 picks itself as the leader, it
    // adds BitCount = 1 to the counter alone, and the lower-lane mask count is 0.
    program.info.uses_subgroup_invocation_id = true;
    for (const auto [block, ballot] : ballots) {
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*ballot)};
        const IR::U1 pred{ballot->Arg(0)};
        const IR::U32 own_bit{ir.ShiftLeftLogical(ir.Imm32(1u), ir.LaneId())};
        ballot->ReplaceUsesWith(IR::U32{ir.Select(pred, own_bit, ir.Imm32(0u))});
    }
    // Reading the leader is reading the lane itself.
    for (IR::Inst* const shuffle : shuffles) {
        if (IR::Inst* const in_bounds{
                shuffle->GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)}) {
            in_bounds->ReplaceUsesWith(IR::Value{true});
        }
        shuffle->ReplaceUsesWith(shuffle->Arg(0));
    }
    return true;
}

} // namespace Shader::Optimization
