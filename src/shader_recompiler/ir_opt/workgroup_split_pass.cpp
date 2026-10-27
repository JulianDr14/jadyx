// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/ir_opt/workgroup_split_pass.h"

namespace Shader::Optimization {
namespace {
using Op = IR::Opcode;
using Verdict = WorkgroupSplitVerdict;
using InstSet = std::unordered_set<const IR::Inst*>;

bool IsWorkgroupSync(Op op) {
    return op == Op::Barrier || op == Op::WorkgroupMemoryBarrier;
}

bool IsSharedMemory(Op op) {
    switch (op) {
    case Op::LoadSharedU8:
    case Op::LoadSharedS8:
    case Op::LoadSharedU16:
    case Op::LoadSharedS16:
    case Op::LoadSharedU32:
    case Op::LoadSharedU64:
    case Op::LoadSharedU128:
    case Op::WriteSharedU8:
    case Op::WriteSharedU16:
    case Op::WriteSharedU32:
    case Op::WriteSharedU64:
    case Op::WriteSharedU128:
    case Op::SharedAtomicIAdd32:
    case Op::SharedAtomicSMin32:
    case Op::SharedAtomicUMin32:
    case Op::SharedAtomicSMax32:
    case Op::SharedAtomicUMax32:
    case Op::SharedAtomicInc32:
    case Op::SharedAtomicDec32:
    case Op::SharedAtomicAnd32:
    case Op::SharedAtomicOr32:
    case Op::SharedAtomicXor32:
    case Op::SharedAtomicExchange32:
    case Op::SharedAtomicExchange64:
    case Op::SharedAtomicExchange32x2:
        return true;
    default:
        return false;
    }
}

/// index < bound and its negation, with the index as the first operand.
bool IsOrdering(Op op) {
    return op == Op::SLessThan || op == Op::ULessThan || op == Op::SGreaterThanEqual ||
           op == Op::UGreaterThanEqual;
}

bool IsImm(const IR::Value& value, u32 imm) {
    const IR::Value resolved{value.Resolve()};
    return resolved.IsImmediate() && resolved.Type() == IR::Type::U32 && resolved.U32() == imm;
}

bool Contains(const InstSet& set, const IR::Value& value) {
    const IR::Inst* const inst{value.TryInstRecursive()};
    return inst && set.contains(inst);
}

/// The values a value can take through phis: everything it resolves to that is not a phi.
std::vector<IR::Value> Leaves(const IR::Value& value) {
    std::vector<IR::Value> leaves;
    std::vector<IR::Value> pending{value};
    InstSet visited;
    while (!pending.empty()) {
        const IR::Value current{pending.back().Resolve()};
        pending.pop_back();
        if (!current.IsPhi()) {
            if (std::ranges::find(leaves, current) == leaves.end()) leaves.push_back(current);
            continue;
        }
        IR::Inst* const phi{current.Inst()};
        if (!visited.insert(phi).second) continue;
        for (size_t i = 0; i < phi->NumArgs(); ++i) pending.push_back(phi->Arg(i));
    }
    return leaves;
}

/// `value == 0` with `value` only ever one of `bounds`: no slice has items to walk then.
bool IsEmptyBound(const IR::Inst* inst, const std::vector<IR::Value>& bounds) {
    if (!inst || inst->GetOpcode() != Op::IEqual) return false;
    for (size_t zero = 0; zero < 2; ++zero) {
        if (!IsImm(inst->Arg(zero), 0)) continue;
        const auto leaves{Leaves(inst->Arg(1 - zero))};
        return std::ranges::all_of(leaves, [&](const IR::Value& leaf) {
            return std::ranges::find(bounds, leaf) != bounds.end();
        });
    }
    return false;
}

/// Writes whose count does not depend on the group width: per-lane local memory and the
/// structural pseudo-instructions. Barriers and shared memory are refused earlier.
bool IsWrite(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case Op::ConditionRef:
    case Op::Reference:
    case Op::PhiMove:
    case Op::Prologue:
    case Op::Epilogue:
    case Op::Join:
    case Op::DeviceMemoryBarrier:
    case Op::WriteLocal:
        return false;
    default:
        return inst.MayHaveSideEffects();
    }
}

/// What an If condition tests, behind its ConditionRef, Identity copies and LogicalNots.
std::pair<const IR::Inst*, bool> Predicate(const IR::Value& cond) {
    bool negated{};
    IR::Value value{cond.Resolve()};
    while (!value.IsImmediate()) {
        const Op op{value.InstRecursive()->GetOpcode()};
        if (op != Op::LogicalNot && op != Op::ConditionRef) break;
        negated ^= op == Op::LogicalNot;
        value = value.InstRecursive()->Arg(0).Resolve();
    }
    return {value.IsImmediate() ? nullptr : value.InstRecursive(), negated};
}

/// Whether every write runs inside a stride loop, whose items are shared out among the slices,
/// or behind an empty-group leader check, which only the first lane of slice 0 passes. Any other
/// write would run once more per extra thread: an atomic or counter before the loop would count
/// `factor` times as many lanes.
bool WritesConfined(const IR::Program& program, const InstSet& roots,
                    const InstSet& empty_leaders) {
    const auto& syntax{program.syntax_list};
    std::unordered_set<const IR::Block*> loop_blocks;
    std::vector<std::pair<const IR::Block*, const IR::Block*>> leader_edges;
    std::vector<bool> loops; ///< Open Loop nodes: whether each is a stride loop.
    IR::Block* last_block{};
    IR::Block* entry{};
    for (size_t index = 0; index < syntax.size(); ++index) {
        const auto& node{syntax[index]};
        using Type = IR::AbstractSyntaxNode::Type;
        switch (node.type) {
        case Type::Block:
            last_block = node.data.block;
            if (!entry) entry = last_block;
            if (std::ranges::find(loops, true) != loops.end()) loop_blocks.insert(last_block);
            break;
        case Type::Loop: {
            // The frontend puts the loop header, which holds the index phi, right before it.
            const bool stride{last_block &&
                              std::ranges::any_of(last_block->Instructions(),
                                                  [&](const IR::Inst& inst) {
                                                      return roots.contains(&inst);
                                                  })};
            if (stride) loop_blocks.insert(last_block);
            loops.push_back(stride);
            break;
        }
        case Type::Repeat:
            if (loops.empty()) return false;
            loops.pop_back();
            break;
        case Type::If: {
            const auto [inst, negated]{Predicate(node.data.if_node.cond)};
            if (last_block && inst && empty_leaders.contains(inst)) {
                leader_edges.emplace_back(last_block, negated ? node.data.if_node.merge
                                                              : node.data.if_node.body);
            }
            break;
        }
        default:
            break;
        }
    }
    if (!entry) return false;
    // Blocks any lane can reach without passing an empty-group leader check.
    std::unordered_set<const IR::Block*> reached{entry};
    std::vector<const IR::Block*> pending{entry};
    while (!pending.empty()) {
        const IR::Block* const block{pending.back()};
        pending.pop_back();
        for (const IR::Block* const next : block->ImmSuccessors()) {
            const std::pair edge{block, next};
            if (std::ranges::find(leader_edges, edge) != leader_edges.end()) continue;
            if (reached.insert(next).second) pending.push_back(next);
        }
    }
    for (const IR::Block* const block : reached) {
        if (loop_blocks.contains(block)) continue;
        if (std::ranges::any_of(block->Instructions(), IsWrite)) return false;
    }
    return true;
}

/// An instruction and the block holding it, where its replacement is emitted.
struct Site {
    IR::Block* block;
    IR::Inst* inst;
};

struct Plan {
    std::vector<Site> group_ids; ///< X components of WorkgroupId
    std::vector<Site> local_ids; ///< X components of LocalInvocationId
    std::vector<IR::Inst*> steps;     ///< IAdd32 index, #width of each stride loop
};

/// The loop step `phi + #width` among a stride loop's values, when the phi only ever holds the
/// local id or that step.
IR::Inst* StepOf(const IR::Value& leaf, u32 width, const InstSet& local_ids) {
    IR::Inst* const inst{leaf.TryInstRecursive()};
    if (!inst || inst->GetOpcode() != Op::IAdd32) return nullptr;
    for (size_t imm = 0; imm < 2; ++imm) {
        const IR::Value base{inst->Arg(1 - imm).Resolve()};
        if (!IsImm(inst->Arg(imm), width) || !base.IsPhi()) continue;
        const bool closed = std::ranges::all_of(Leaves(base), [&](const IR::Value& v) {
            return v.TryInstRecursive() == inst || Contains(local_ids, v);
        });
        if (closed) return inst;
    }
    return nullptr;
}

Verdict Analyze(IR::Program& program, u32 width, Plan& plan) {
    InstSet local_ids;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const Op op{inst.GetOpcode()};
            if (IsWorkgroupSync(op)) return Verdict::Barrier;
            if (IsSharedMemory(op)) return Verdict::SharedMemory;
            if (op == Op::Identity) continue;
            for (size_t i = 0; i < inst.NumArgs(); ++i) {
                const IR::Inst* const def{inst.Arg(i).TryInstRecursive()};
                if (!def || (def->GetOpcode() != Op::WorkgroupId &&
                             def->GetOpcode() != Op::LocalInvocationId)) {
                    continue;
                }
                if (op != Op::CompositeExtractU32x3 || i != 0 || !inst.Arg(1).IsImmediate()) {
                    return Verdict::PackedId;
                }
                if (!IsImm(inst.Arg(1), 0)) continue; // Y and Z stay 0 and the group's own
                IR::Inst* const component{&inst};
                if (def->GetOpcode() == Op::WorkgroupId) {
                    plan.group_ids.push_back({block, component});
                } else {
                    plan.local_ids.push_back({block, component});
                    local_ids.insert(component);
                }
            }
        }
    }

    // Who reads local id X.
    std::vector<IR::Inst*> roots;
    std::vector<IR::Inst*> guards; ///< index (local id or loop phi) checked against a bound
    std::vector<IR::Inst*> leaders; ///< local id == 0
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const Op op{inst.GetOpcode()};
            if (op == Op::Identity) continue;
            for (size_t i = 0; i < inst.NumArgs(); ++i) {
                if (!Contains(local_ids, inst.Arg(i))) continue;
                if (op == Op::Phi) {
                    roots.push_back(&inst);
                } else if (IsOrdering(op) && i == 0) {
                    guards.push_back(&inst);
                } else if (op == Op::IEqual && IsImm(inst.Arg(1 - i), 0)) {
                    leaders.push_back(&inst);
                } else {
                    return Verdict::OtherLocalUse;
                }
            }
        }
    }
    if (roots.empty()) return Verdict::NoStrideLoop;

    InstSet root_set(roots.begin(), roots.end());
    for (IR::Inst* const root : roots) {
        IR::Inst* root_step{};
        for (const IR::Value& leaf : Leaves(IR::Value{root})) {
            if (Contains(local_ids, leaf)) continue;
            IR::Inst* const step{StepOf(leaf, width, local_ids)};
            if (!step || (root_step && root_step != step)) return Verdict::BadStrideLoop;
            root_step = step;
        }
        if (!root_step) return Verdict::BadStrideLoop;
        if (std::ranges::find(plan.steps, root_step) == plan.steps.end()) {
            plan.steps.push_back(root_step);
        }
    }

    // What each loop runs up to, and which checks of an index are against it.
    InstSet step_set(plan.steps.begin(), plan.steps.end());
    std::vector<IR::Value> bounds;
    std::vector<IR::Inst*> bounded;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsOrdering(inst.GetOpcode())) continue;
            IR::Inst* const index{inst.Arg(0).TryInstRecursive()};
            if (index && root_set.contains(index)) {
                guards.push_back(&inst);
            }
            if (!index || !step_set.contains(index)) continue;
            for (const IR::Value& bound : Leaves(inst.Arg(1))) {
                if (std::ranges::find(bounds, bound) == bounds.end()) bounds.push_back(bound);
            }
            if (std::ranges::find(bounded, index) == bounded.end()) {
                bounded.push_back(index);
            }
        }
    }
    if (bounded.size() != plan.steps.size()) return Verdict::BadStrideLoop;
    if (guards.empty()) return Verdict::NoGuard;
    for (const IR::Inst* const guard : guards) {
        // Extra threads past the original width must skip the loop: only a check against what
        // the loop runs up to does that for them.
        for (const IR::Value& bound : Leaves(guard->Arg(1))) {
            if (std::ranges::find(bounds, bound) == bounds.end()) return Verdict::OtherLocalUse;
        }
    }
    // A leader check alone runs when its slice is done, not the whole original group, so a
    // "last group done" counter after it would race the other slices. Joined with the loop
    // bound being 0 it is safe: no slice has items, and only the first lane of slice 0 passes.
    InstSet empty_leaders;
    for (const IR::Inst* const leader : leaders) {
        for (IR::Block* const block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == Op::Identity) continue;
                for (size_t i = 0; i < inst.NumArgs(); ++i) {
                    if (inst.Arg(i).TryInstRecursive() != leader) continue;
                    if (inst.GetOpcode() != Op::LogicalAnd || inst.NumArgs() != 2 ||
                        !IsEmptyBound(inst.Arg(1 - i).TryInstRecursive(), bounds)) {
                        return Verdict::OtherLocalUse;
                    }
                    empty_leaders.insert(&inst);
                }
            }
        }
    }
    if (!WritesConfined(program, root_set, empty_leaders)) return Verdict::UnconfinedWrite;
    return Verdict::Split;
}

void Rewrite(IR::Program& program, const Plan& plan, u32 width, u32 factor) {
    const u32 shift{static_cast<u32>(std::countr_zero(factor))};
    for (const auto [block, group_id] : plan.group_ids) {
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*group_id)};
        group_id->ReplaceUsesWith(
            IR::U32{ir.ShiftRightLogical(ir.WorkgroupIdX(), ir.Imm32(shift))});
    }
    for (const auto [block, local_id] : plan.local_ids) {
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*local_id)};
        const IR::U32 slice{ir.BitwiseAnd(ir.WorkgroupIdX(), ir.Imm32(factor - 1))};
        local_id->ReplaceUsesWith(
            ir.IAdd(ir.LocalInvocationIdX(), ir.IMul(slice, ir.Imm32(width))));
    }
    for (IR::Inst* const step : plan.steps) {
        const size_t imm{IsImm(step->Arg(0), width) ? 0U : 1U};
        step->SetArg(imm, IR::Value{width * factor});
    }
    program.info.uses_workgroup_id = true;
    program.info.uses_local_invocation_id = true;
    DeadCodeEliminationPass(program);
}
} // Anonymous namespace

std::string_view Name(WorkgroupSplitVerdict verdict) noexcept {
    switch (verdict) {
    case Verdict::Split:
        return "split";
    case Verdict::NotCompute:
        return "not compute";
    case Verdict::BadFactor:
        return "factor not a power of two";
    case Verdict::NotLinear:
        return "local size not X only";
    case Verdict::SharedMemory:
        return "shared memory";
    case Verdict::Barrier:
        return "workgroup barrier";
    case Verdict::NoStrideLoop:
        return "no stride loop";
    case Verdict::BadStrideLoop:
        return "loop not a plain stride loop";
    case Verdict::NoGuard:
        return "loop index never checked before the loop";
    case Verdict::OtherLocalUse:
        return "local id used outside its loop";
    case Verdict::PackedId:
        return "id vector used whole";
    case Verdict::UnconfinedWrite:
        return "write outside the loop and any empty-group leader check";
    }
    return "unknown";
}

WorkgroupSplitVerdict WorkgroupSplitPass(IR::Program& program, u32 factor) {
    if (program.stage != Stage::Compute) return Verdict::NotCompute;
    if (factor < 2 || !std::has_single_bit(factor)) return Verdict::BadFactor;
    const auto& size{program.workgroup_size};
    if (size[1] != 1 || size[2] != 1 || size[0] == 0 || size[0] > 0xFFFF / factor) {
        return Verdict::NotLinear;
    }
    if (program.shared_memory_size != 0) return Verdict::SharedMemory;
    Plan plan;
    const Verdict verdict{Analyze(program, size[0], plan)};
    if (verdict == Verdict::Split) Rewrite(program, plan, size[0], factor);
    return verdict;
}

} // namespace Shader::Optimization
