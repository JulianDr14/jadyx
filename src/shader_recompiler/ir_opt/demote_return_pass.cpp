// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <initializer_list>
#include <vector>
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/demote_return_pass.h"

namespace Shader::Optimization {
namespace {
using Type = IR::AbstractSyntaxNode::Type;
using Op = IR::Opcode;
using Verdict = DemoteReturnVerdict;

bool OutputOnly(Op op) {
    return op == Op::SetFragColor || op == Op::SetFragDepth ||
           op == Op::SetSampleMask || op == Op::Epilogue;
}

// Merged when the program can be rewritten, otherwise why not. Scans everything so that a
// rejection is still reported when helper lanes are needed (the verdict is diagnostic).
// Loops and phis are checked per arm in FindCandidates.
Verdict Eligibility(const IR::Program& p) {
    const auto& info = p.info;
    if (p.stage != Stage::Fragment || !info.uses_demote_to_helper_invocation) {
        return Verdict::NotNeeded;
    }
    Verdict reason = Verdict::Merged;
    const auto reject = [&reason](Verdict why) {
        if (reason == Verdict::Merged) reason = why;
    };
    if (info.uses_subgroup_shuffles || info.uses_quad_shuffles || info.uses_subgroup_vote ||
        info.uses_subgroup_mask || info.uses_subgroup_invocation_id) {
        reject(Verdict::RejectedSubgroup);
    }
    // Declared memory alone does not hold the pass back: helper lanes' stores and atomics have
    // no effect (SPIR-V), local memory is per lane, and real writes are side effects below.
    for (const auto& n : p.syntax_list) {
        if (n.type == Type::Unreachable) reject(Verdict::RejectedUnstructured);
    }
    bool needs_helpers = info.uses_derivatives;
    for (const auto* b : p.blocks) {
        for (const auto& i : b->Instructions()) {
            const auto op = i.GetOpcode();
            needs_helpers |= op == Op::ImageSampleImplicitLod ||
                             op == Op::ImageSampleDrefImplicitLod;
            if (i.MayHaveSideEffects() && !OutputOnly(op) && op != Op::Prologue &&
                op != Op::DemoteToHelperInvocation && op != Op::ConditionRef &&
                op != Op::Reference) reject(Verdict::RejectedSideEffect);
        }
    }
    return needs_helpers ? reason : Verdict::NotNeeded;
}

struct Candidate {
    std::size_t first, last, returned;
    IR::Block* tail;
    IR::Block* merge;
};

struct Scan {
    std::vector<Candidate> candidates;
    bool early_return{};               ///< A demoted lane returns early, rewritable or not.
    Verdict refused{Verdict::RejectedArm}; ///< Why such a return stays, first reason found.
    bool refusal_set{};
};

/// What an If condition tests, behind its ConditionRef, Identity copies and LogicalNots. SSA
/// values never change, so two arms with equal predicates take the same lanes.
struct Predicate {
    const IR::Inst* inst{};
    bool negated{};
    bool operator==(const Predicate&) const = default;
};

Predicate Resolve(const IR::Value& cond) {
    Predicate p;
    IR::Value v = cond.Resolve();
    // The frontend wraps every If condition in its own ConditionRef.
    while (!v.IsImmediate()) {
        const auto op = v.InstRecursive()->GetOpcode();
        if (op != Op::LogicalNot && op != Op::ConditionRef) break;
        if (op == Op::LogicalNot) p.negated = !p.negated;
        v = v.InstRecursive()->Arg(0).Resolve();
    }
    p.inst = v.IsImmediate() ? nullptr : v.InstRecursive();
    return p;
}

bool Has(const IR::Block* block, std::initializer_list<Op> ops) {
    return std::any_of(block->Instructions().begin(), block->Instructions().end(),
                       [ops](const IR::Inst& i) {
                           return std::find(ops.begin(), ops.end(), i.GetOpcode()) != ops.end();
                       });
}

// Terminal arms whose lanes are already demoted, in source order. Read-only. False on an
// unbalanced If/EndIf or Loop/Repeat list. Two shapes:
//   if (c) { demote; outputs; return }
//   if (c) { demote } ... if (c) { outputs; return }    (guest @P KIL ... @P EXIT)
// The second needs both arms in the same construct, so the demote runs first.
bool FindCandidates(const IR::Program& program, Scan& scan) {
    const auto& syntax = program.syntax_list;
    struct Open {
        std::size_t begin;
        bool loop;
    };
    struct Killed {
        Predicate predicate;
        std::size_t parent;
    };
    std::vector<Open> open;
    std::vector<Killed> killed;
    const auto parent = [&] { return open.empty() ? syntax.size() : open.back().begin; };
    const auto refuse = [&scan](Verdict why) {
        if (!scan.refusal_set) scan.refused = why;
        scan.refusal_set = true;
    };
    for (std::size_t end = 0; end < syntax.size(); ++end) {
        const auto type = syntax[end].type;
        if (type == Type::If || type == Type::Loop) {
            open.push_back({end, type == Type::Loop});
            continue;
        }
        if (type == Type::Repeat) {
            if (open.empty() || !open.back().loop) return false;
            open.pop_back();
            continue;
        }
        if (type != Type::EndIf) continue;
        if (open.empty() || open.back().loop) return false;
        const auto begin = open.back().begin;
        open.pop_back();
        const auto& branch = syntax[begin].data.if_node;
        const auto predicate = Resolve(branch.cond);
        const bool body_first = end > begin + 1 && syntax[begin + 1].type == Type::Block &&
                                syntax[begin + 1].data.block == branch.body;
        if (body_first && predicate.inst &&
            Has(branch.body, {Op::DemoteToHelperInvocation})) {
            killed.push_back({predicate, parent()});
        }
        if (end <= begin + 1 || syntax[end - 1].type != Type::Return) continue;
        bool demotes = false;
        for (auto n = begin + 1; n < end - 1 && !demotes; ++n) {
            demotes = syntax[n].type == Type::Block &&
                      Has(syntax[n].data.block, {Op::DemoteToHelperInvocation});
        }
        const bool covered = !demotes && predicate.inst &&
            std::any_of(killed.begin(), killed.end(), [&](const Killed& k) {
                return k.predicate == predicate && k.parent == parent();
            });
        if (!demotes && !covered) continue;
        scan.early_return = true;
        if (std::any_of(open.begin(), open.end(), [](const Open& o) { return o.loop; })) {
            // Without the return the lane would run the rest of the iteration.
            refuse(Verdict::RejectedLoop);
            continue;
        }
        if (end <= begin + 2 || !body_first || syntax[end].data.end_if.merge != branch.merge ||
            end + 1 >= syntax.size() || syntax[end + 1].type != Type::Block ||
            syntax[end + 1].data.block != branch.merge) {
            refuse(Verdict::RejectedArm);
            continue;
        }
        bool demoted = covered, safe = true;
        IR::Block* tail{};
        for (auto n = begin + 1; safe && n < end - 1; ++n) {
            if (syntax[n].type != Type::Block) { safe = false; break; }
            tail = syntax[n].data.block;
            for (const auto& inst : tail->Instructions()) {
                const auto op = inst.GetOpcode();
                if (op == Op::DemoteToHelperInvocation) demoted = true;
                else if (op == Op::Identity || op == Op::Void) continue;
                // Bit casts feeding the outputs: dead once the outputs go.
                else if (covered && !OutputOnly(op) && !inst.MayHaveSideEffects()) continue;
                else if (!demoted || !OutputOnly(op)) { safe = false; break; }
            }
        }
        if (!safe || !demoted || !tail || !tail->ImmSuccessors().empty()) {
            refuse(Verdict::RejectedArm);
            continue;
        }
        // The merge gains the arm as a predecessor: its phis would need a value from it.
        if (Has(branch.merge, {Op::Phi, Op::PhiMove})) {
            refuse(Verdict::RejectedPhi);
            continue;
        }
        scan.candidates.push_back({begin + 1, end - 1, end - 1, tail, branch.merge});
    }
    return open.empty();
}
}

std::string_view Name(DemoteReturnVerdict verdict) noexcept {
    switch (verdict) {
    case Verdict::NotNeeded:
        return "not_needed";
    case Verdict::Merged:
        return "merged";
    case Verdict::NoTerminalArm:
        return "no_terminal_arm";
    case Verdict::RejectedLoop:
        return "rejected_loop";
    case Verdict::RejectedPhi:
        return "rejected_phi";
    case Verdict::RejectedSubgroup:
        return "rejected_subgroup";
    case Verdict::RejectedSideEffect:
        return "rejected_side_effect";
    case Verdict::RejectedArm:
        return "rejected_arm";
    case Verdict::RejectedUnstructured:
        return "rejected_unstructured";
    }
    return "unknown";
}

std::size_t DemoteReturnPass(IR::Program& program, DemoteReturnVerdict* verdict) {
    Verdict result = Eligibility(program);
    Scan scan;
    if (result != Verdict::NotNeeded) {
        if (!FindCandidates(program, scan)) {
            result = Verdict::RejectedUnstructured;
        } else if (!scan.early_return) {
            // No demoted lane leaves early, whatever else the shader does.
            result = Verdict::NoTerminalArm;
        } else if (result == Verdict::Merged && scan.candidates.empty()) {
            result = scan.refused;
        }
    }
    if (verdict) *verdict = result;
    if (result != Verdict::Merged) return 0;
    const auto& candidates = scan.candidates;
    auto& syntax = program.syntax_list;
    std::vector<bool> removed(syntax.size());
    for (const auto& c : candidates) {
        for (auto n = c.first; n < c.last; ++n) {
            for (auto& inst : syntax[n].data.block->Instructions()) {
                if (OutputOnly(inst.GetOpcode())) inst.Invalidate();
            }
        }
        c.tail->AddBranch(c.merge);
        removed[c.returned] = true;
    }
    // Compact in source order; do not depend on an algorithm's predicate call order.
    auto output = syntax.begin();
    for (std::size_t index = 0; index < syntax.size(); ++index) {
        if (!removed[index]) *output++ = syntax[index];
    }
    syntax.erase(output, syntax.end());
    return candidates.size();
}
}
