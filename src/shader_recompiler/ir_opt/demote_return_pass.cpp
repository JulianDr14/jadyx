// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

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
    if (info.uses_global_memory || p.local_memory_size || p.shared_memory_size ||
        !info.storage_buffers_descriptors.empty() || !info.image_descriptors.empty() ||
        !info.image_buffer_descriptors.empty()) {
        reject(Verdict::RejectedMemory);
    }
    for (const auto& n : p.syntax_list) {
        if (n.type == Type::Loop || n.type == Type::Repeat || n.type == Type::Break) {
            reject(Verdict::RejectedLoop);
        } else if (n.type == Type::Unreachable) {
            reject(Verdict::RejectedUnstructured);
        }
    }
    bool needs_helpers = info.uses_derivatives;
    for (const auto* b : p.blocks) {
        for (const auto& i : b->Instructions()) {
            const auto op = i.GetOpcode();
            needs_helpers |= op == Op::ImageSampleImplicitLod ||
                             op == Op::ImageSampleDrefImplicitLod;
            // Adding a predecessor requires updating phis. Do not approximate it.
            if (op == Op::Phi || op == Op::PhiMove) reject(Verdict::RejectedPhi);
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

// Terminal discard arms, in source order. Read-only. False on an unbalanced If/EndIf list.
// `early_return`: some arm demotes and returns, rewritable or not.
bool FindCandidates(const IR::Program& program, std::vector<Candidate>& candidates,
                    bool& early_return) {
    const auto& syntax = program.syntax_list;
    std::vector<std::size_t> stack;
    for (std::size_t end = 0; end < syntax.size(); ++end) {
        if (syntax[end].type == Type::If) {
            stack.push_back(end);
            continue;
        }
        if (syntax[end].type != Type::EndIf) continue;
        if (stack.empty()) return false;
        const auto begin = stack.back();
        stack.pop_back();
        const auto& branch = syntax[begin].data.if_node;
        if (end > begin + 1 && syntax[end - 1].type == Type::Return) {
            for (auto n = begin + 1; n < end - 1 && !early_return; ++n) {
                if (syntax[n].type != Type::Block) continue;
                for (const auto& inst : syntax[n].data.block->Instructions()) {
                    if (inst.GetOpcode() == Op::DemoteToHelperInvocation) early_return = true;
                }
            }
        }
        if (end <= begin + 2 || syntax[end - 1].type != Type::Return ||
            syntax[begin + 1].type != Type::Block ||
            syntax[begin + 1].data.block != branch.body ||
            syntax[end].data.end_if.merge != branch.merge || end + 1 >= syntax.size() ||
            syntax[end + 1].type != Type::Block || syntax[end + 1].data.block != branch.merge)
            continue;
        bool demoted = false, safe = true;
        IR::Block* tail{};
        for (auto n = begin + 1; safe && n < end - 1; ++n) {
            if (syntax[n].type != Type::Block) { safe = false; break; }
            tail = syntax[n].data.block;
            for (const auto& inst : tail->Instructions()) {
                const auto op = inst.GetOpcode();
                if (op == Op::DemoteToHelperInvocation) demoted = true;
                else if (op == Op::Identity || op == Op::Void) continue;
                else if (!demoted || !OutputOnly(op)) { safe = false; break; }
            }
        }
        if (safe && demoted && tail && tail->ImmSuccessors().empty())
            candidates.push_back({begin + 1, end - 1, end - 1, tail, branch.merge});
    }
    return stack.empty();
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
    case Verdict::RejectedMemory:
        return "rejected_memory";
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
    std::vector<Candidate> candidates;
    if (result != Verdict::NotNeeded) {
        bool early_return{};
        if (!FindCandidates(program, candidates, early_return)) {
            result = Verdict::RejectedUnstructured;
        } else if (!early_return) {
            // No demoted lane leaves early, whatever else the shader does.
            result = Verdict::NoTerminalArm;
        } else if (result == Verdict::Merged && candidates.empty()) {
            result = Verdict::RejectedArm;
        }
    }
    if (verdict) *verdict = result;
    if (result != Verdict::Merged) return 0;
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
