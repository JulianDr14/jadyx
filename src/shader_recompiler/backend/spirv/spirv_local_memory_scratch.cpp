// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/div_ceil.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/local_memory_scratch.h"

namespace Shader::Backend::SPIRV {
namespace Scratch = LocalMemoryScratch;
namespace {
constexpr u32 ACQUIRE_SEMANTICS{static_cast<u32>(spv::MemorySemanticsMask::Acquire) |
                                static_cast<u32>(spv::MemorySemanticsMask::UniformMemory)};
constexpr u32 RELEASE_SEMANTICS{static_cast<u32>(spv::MemorySemanticsMask::Release) |
                                static_cast<u32>(spv::MemorySemanticsMask::UniformMemory)};
constexpr u32 BARRIER_SEMANTICS{static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease) |
                                static_cast<u32>(spv::MemorySemanticsMask::UniformMemory)};

/// Ballot words that can hold live lanes, or 0 when every live lane leaves at once (one exit
/// and no demote), so the slot is freed at that exit without tracking its lanes.
u32 MaskWords(const EmitContext& ctx, bool tracks_lanes) {
    if (!tracks_lanes) {
        return 0;
    }
    return std::min(Common::DivCeil(ctx.profile.fragment_local_memory_scratch_lanes, 32U),
                    Scratch::MASK_WORDS);
}

Id PoolWord(EmitContext& ctx, Id index) {
    return ctx.OpAccessChain(ctx.local_memory_scratch_element, ctx.local_memory_scratch,
                             ctx.u32_zero_value, index);
}

Id MaskIndex(EmitContext& ctx, Id slot, u32 word) {
    const Id base{ctx.OpIMul(ctx.U32[1], slot, ctx.Const(Scratch::MASK_WORDS))};
    return ctx.OpIAdd(ctx.U32[1], base, ctx.Const(Scratch::MASK_BASE + word));
}

/// Elected live lane of the wave, without OpGroupNonUniformElect: the lowest lane of a ballot.
Id IsLeader(EmitContext& ctx, Id lane, Id ballot) {
    const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Subgroup))};
    const Id leader{ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], scope, ballot)};
    return ctx.OpIEqual(ctx.U1, lane, leader);
}

/// Returns the pool index of word 0 of the calling lane and stores the row stride in stride_var.
/// The leader takes the lowest free slot of a bitmap word, starting at a rotating word so
/// concurrent waves rarely collide; a wave that finds the pool full keeps retrying for a bounded
/// time and then shares the overflow slot. Both outcomes are counted for the host, which grows
/// the pool when waves wait.
Id DefineAcquire(EmitContext& ctx, Id slot_var, Id live_var, Id stride_var, u32 mask_words) {
    const u32 lanes{ctx.profile.fragment_local_memory_scratch_lanes};
    const Id u32_type{ctx.U32[1]};
    const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Subgroup))};
    const Id device{ctx.Const(static_cast<u32>(spv::Scope::Device))};
    const Id relaxed{ctx.u32_zero_value};
    const Id function_pointer{ctx.TypePointer(spv::StorageClass::Function, u32_type)};

    const Id func{ctx.OpFunction(u32_type, spv::FunctionControlMask::MaskNone,
                                 ctx.TypeFunction(u32_type))};
    ctx.Name(func, "local_memory_acquire");
    ctx.AddLabel();
    const Id attempt_var{ctx.AddLocalVariable(function_pointer, spv::StorageClass::Function,
                                              ctx.u32_zero_value)};
    const Id found_var{ctx.AddLocalVariable(function_pointer, spv::StorageClass::Function,
                                            ctx.Const(Scratch::INVALID_SLOT))};
    const Id lane{ctx.OpLoad(u32_type, ctx.subgroup_local_invocation_id)};
    const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], scope, ctx.true_value)};
    // Every lane reads the size of the pool, helper lanes included: they need the stride too. The
    // clamp keeps a missing header inside the smallest pool.
    const Id stored_words{
        ctx.OpLoad(u32_type, PoolWord(ctx, ctx.Const(Scratch::BITMAP_WORDS_WORD)))};
    const Id bitmap_words{ctx.OpUMin(u32_type,
                                     ctx.OpUMax(u32_type, stored_words,
                                                ctx.Const(Scratch::MIN_BITMAP_WORDS)),
                                     ctx.Const(Scratch::MAX_BITMAP_WORDS))};
    const Id word_mask{ctx.OpISub(u32_type, bitmap_words, ctx.Const(1U))};
    const Id slots{ctx.OpShiftLeftLogical(u32_type, bitmap_words, ctx.Const(5U))};

    const Id leader_label{ctx.OpLabel()};
    const Id header_label{ctx.OpLabel()};
    const Id body_label{ctx.OpLabel()};
    const Id try_label{ctx.OpLabel()};
    const Id won_label{ctx.OpLabel()};
    const Id won_merge{ctx.OpLabel()};
    const Id try_merge{ctx.OpLabel()};
    const Id continue_label{ctx.OpLabel()};
    const Id loop_merge{ctx.OpLabel()};
    const Id count_label{ctx.OpLabel()};
    const Id count_merge{ctx.OpLabel()};
    const Id owned_label{ctx.OpLabel()};
    const Id owned_merge{ctx.OpLabel()};
    const Id done_label{ctx.OpLabel()};

    // Conditions are emitted before their OpSelectionMerge: the merge must directly precede
    // the branch.
    const Id is_leader{IsLeader(ctx, lane, ballot)};
    ctx.OpSelectionMerge(done_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(is_leader, leader_label, done_label);

    ctx.AddLabel(leader_label);
    const Id start{
        ctx.OpAtomicIIncrement(u32_type, PoolWord(ctx, ctx.Const(Scratch::ACQUIRE_WORD)), device,
                               relaxed)};
    ctx.OpBranch(header_label);

    ctx.AddLabel(header_label);
    const Id attempt{ctx.OpLoad(u32_type, attempt_var)};
    const Id searching{ctx.OpULessThan(ctx.U1, attempt, ctx.Const(Scratch::ACQUIRE_ATTEMPTS))};
    ctx.OpLoopMerge(loop_merge, continue_label, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(searching, body_label, loop_merge);

    ctx.AddLabel(body_label);
    const Id word{ctx.OpBitwiseAnd(u32_type, ctx.OpIAdd(u32_type, start, attempt), word_mask)};
    const Id bitmap{PoolWord(ctx, ctx.OpIAdd(u32_type, word, ctx.Const(Scratch::BITMAP_BASE)))};
    const Id free_bits{ctx.OpNot(u32_type, ctx.OpAtomicLoad(u32_type, bitmap, device, relaxed))};
    const Id has_free{ctx.OpINotEqual(ctx.U1, free_bits, ctx.u32_zero_value)};
    ctx.OpSelectionMerge(try_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(has_free, try_label, try_merge);

    ctx.AddLabel(try_label);
    const Id bit{ctx.OpFindILsb(u32_type, free_bits)};
    const Id bit_mask{ctx.OpShiftLeftLogical(u32_type, ctx.Const(1U), bit)};
    const Id previous{
        ctx.OpAtomicOr(u32_type, bitmap, device, ctx.Const(ACQUIRE_SEMANTICS), bit_mask)};
    const Id won{
        ctx.OpIEqual(ctx.U1, ctx.OpBitwiseAnd(u32_type, previous, bit_mask), ctx.u32_zero_value)};
    ctx.OpSelectionMerge(won_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(won, won_label, won_merge);

    ctx.AddLabel(won_label);
    ctx.OpStore(found_var, ctx.OpIAdd(u32_type, ctx.OpShiftLeftLogical(u32_type, word,
                                                                       ctx.Const(5U)),
                                      bit));
    ctx.OpBranch(loop_merge);

    ctx.AddLabel(won_merge);
    ctx.OpBranch(try_merge);

    ctx.AddLabel(try_merge);
    ctx.OpBranch(continue_label);

    ctx.AddLabel(continue_label);
    ctx.OpStore(attempt_var, ctx.OpIAdd(u32_type, attempt, ctx.Const(1U)));
    ctx.OpBranch(header_label);

    ctx.AddLabel(loop_merge);
    // A wave that scanned every bitmap word before winning found the pool full: it waited.
    const Id found{ctx.OpLoad(u32_type, found_var)};
    const Id overflow{ctx.OpIEqual(ctx.U1, found, ctx.Const(Scratch::INVALID_SLOT))};
    const Id waited{
        ctx.OpUGreaterThanEqual(ctx.U1, ctx.OpLoad(u32_type, attempt_var), bitmap_words)};
    const Id counter{ctx.OpSelect(u32_type, overflow, ctx.Const(Scratch::OVERFLOW_WORD),
                                  ctx.Const(Scratch::WAIT_WORD))};
    const Id counted{ctx.OpLogicalOr(ctx.U1, overflow, waited)};
    ctx.OpSelectionMerge(count_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(counted, count_label, count_merge);

    ctx.AddLabel(count_label);
    ctx.OpAtomicIIncrement(u32_type, PoolWord(ctx, counter), device, relaxed);
    ctx.OpBranch(count_merge);

    ctx.AddLabel(count_merge);
    ctx.OpSelectionMerge(owned_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(overflow, owned_merge, owned_label);

    ctx.AddLabel(owned_label);
    // An exchange instead of OpAtomicStore: sirit encodes that one with a stray result id.
    for (u32 index = 0; index < mask_words; ++index) {
        ctx.OpAtomicExchange(u32_type, PoolWord(ctx, MaskIndex(ctx, found, index)), device,
                             relaxed, ctx.OpCompositeExtract(u32_type, ballot, index));
    }
    ctx.OpBranch(owned_merge);

    ctx.AddLabel(owned_merge);
    ctx.OpBranch(done_label);

    ctx.AddLabel(done_label);
    // The leader is the first live lane. Helper lanes get no broadcast on D3D12 and may read
    // anything: the clamps keep their accesses inside the pool, and their stores are dropped.
    const Id broadcast{
        ctx.OpGroupNonUniformBroadcastFirst(u32_type, scope, ctx.OpLoad(u32_type, found_var))};
    const Id owned{ctx.OpULessThan(ctx.U1, broadcast, slots)};
    ctx.OpStore(slot_var,
                ctx.OpSelect(u32_type, owned, broadcast, ctx.Const(Scratch::INVALID_SLOT)));
    ctx.OpStore(live_var, ctx.true_value);
    // Slot `slots` is the overflow slot.
    const Id data_slot{ctx.OpUMin(u32_type, broadcast, slots)};
    const Id total_slots{ctx.OpIAdd(u32_type, slots, ctx.Const(1U))};
    ctx.OpStore(stride_var, ctx.OpIMul(u32_type, total_slots, ctx.Const(lanes)));
    const Id lane_offset{ctx.OpUMin(u32_type, lane, ctx.Const(lanes - 1))};
    const Id slot_offset{ctx.OpIMul(u32_type, data_slot, ctx.Const(lanes))};
    ctx.OpReturnValue(ctx.OpIAdd(u32_type, ctx.OpIAdd(u32_type, slot_offset, lane_offset),
                                 ctx.Const(Scratch::DATA_BASE)));
    ctx.OpFunctionEnd();
    return func;
}

/// Called by the lanes that leave (epilogue, demote or kill). Their leader clears them from the
/// live mask of the slot, and frees the slot when no live lane remains. Only the owning wave
/// touches its mask, so the per-word atomics need no ordering among themselves.
Id DefineRelease(EmitContext& ctx, Id slot_var, Id live_var, u32 mask_words) {
    const Id u32_type{ctx.U32[1]};
    const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Subgroup))};
    const Id device{ctx.Const(static_cast<u32>(spv::Scope::Device))};
    const Id relaxed{ctx.u32_zero_value};

    const Id func{ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                 ctx.TypeFunction(ctx.void_id))};
    ctx.Name(func, "local_memory_release");
    ctx.AddLabel();
    const Id live_label{ctx.OpLabel()};
    const Id owner_label{ctx.OpLabel()};
    const Id free_label{ctx.OpLabel()};
    const Id free_merge{ctx.OpLabel()};
    const Id owner_merge{ctx.OpLabel()};
    const Id end_label{ctx.OpLabel()};

    const Id live{ctx.OpLoad(ctx.U1, live_var)};
    ctx.OpSelectionMerge(end_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(live, live_label, end_label);

    ctx.AddLabel(live_label);
    ctx.OpStore(live_var, ctx.false_value);
    // Data stores of the leaving lanes complete before the slot can change owner.
    ctx.OpMemoryBarrier(device, ctx.Const(BARRIER_SEMANTICS));
    const Id lane{ctx.OpLoad(u32_type, ctx.subgroup_local_invocation_id)};
    const Id slot{ctx.OpLoad(u32_type, slot_var)};
    const Id leaving{ctx.OpGroupNonUniformBallot(ctx.U32[4], scope, ctx.true_value)};
    const Id owner{ctx.OpLogicalAnd(ctx.U1, IsLeader(ctx, lane, leaving),
                                    ctx.OpINotEqual(ctx.U1, slot,
                                                    ctx.Const(Scratch::INVALID_SLOT)))};
    ctx.OpSelectionMerge(owner_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(owner, owner_label, owner_merge);

    ctx.AddLabel(owner_label);
    Id remaining{ctx.u32_zero_value};
    for (u32 index = 0; index < mask_words; ++index) {
        const Id staying{ctx.OpNot(u32_type, ctx.OpCompositeExtract(u32_type, leaving, index))};
        const Id previous{ctx.OpAtomicAnd(u32_type, PoolWord(ctx, MaskIndex(ctx, slot, index)),
                                          device, relaxed, staying)};
        remaining =
            ctx.OpBitwiseOr(u32_type, remaining, ctx.OpBitwiseAnd(u32_type, previous, staying));
    }
    const Id last{mask_words == 0 ? ctx.true_value
                                  : ctx.OpIEqual(ctx.U1, remaining, ctx.u32_zero_value)};
    ctx.OpSelectionMerge(free_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(last, free_label, free_merge);

    ctx.AddLabel(free_label);
    const Id word{ctx.OpShiftRightLogical(u32_type, slot, ctx.Const(5U))};
    const Id bit{ctx.OpBitwiseAnd(u32_type, slot, ctx.Const(31U))};
    const Id clear{ctx.OpNot(u32_type, ctx.OpShiftLeftLogical(u32_type, ctx.Const(1U), bit))};
    ctx.OpAtomicAnd(u32_type,
                    PoolWord(ctx, ctx.OpIAdd(u32_type, word, ctx.Const(Scratch::BITMAP_BASE))),
                    device, ctx.Const(RELEASE_SEMANTICS), clear);
    ctx.OpBranch(free_merge);

    ctx.AddLabel(free_merge);
    ctx.OpBranch(owner_merge);

    ctx.AddLabel(owner_merge);
    ctx.OpBranch(end_label);

    ctx.AddLabel(end_label);
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    return func;
}
} // Anonymous namespace

void EmitLocalMemoryRelease(EmitContext& ctx) {
    if (Sirit::ValidId(ctx.local_memory_release_func)) {
        ctx.OpFunctionCall(ctx.void_id, ctx.local_memory_release_func);
    }
}

void EmitContext::DefineLocalMemoryScratch(u32 words, bool tracks_lanes) {
    AddCapability(spv::Capability::GroupNonUniform);
    AddCapability(spv::Capability::GroupNonUniformBallot);
    local_memory_scratch_words = words;

    const Id array_type{TypeRuntimeArray(U32[1])};
    Decorate(array_type, spv::Decoration::ArrayStride, 4U);
    const Id struct_type{TypeStruct(array_type)};
    Decorate(struct_type, spv::Decoration::Block);
    MemberDecorate(struct_type, 0, spv::Decoration::Offset, 0U);
    local_memory_scratch_element = TypePointer(spv::StorageClass::StorageBuffer, U32[1]);
    local_memory_scratch =
        AddGlobalVariable(TypePointer(spv::StorageClass::StorageBuffer, struct_type),
                          spv::StorageClass::StorageBuffer);
    Decorate(local_memory_scratch, spv::Decoration::Binding, Scratch::BINDING);
    Decorate(local_memory_scratch, spv::Decoration::DescriptorSet, Scratch::DESCRIPTOR_SET);
    Name(local_memory_scratch, "local_memory_scratch");

    if (!Sirit::ValidId(subgroup_local_invocation_id)) {
        const Id pointer{TypePointer(spv::StorageClass::Input, U32[1])};
        subgroup_local_invocation_id = AddGlobalVariable(pointer, spv::StorageClass::Input);
        Decorate(subgroup_local_invocation_id, spv::Decoration::BuiltIn,
                 spv::BuiltIn::SubgroupLocalInvocationId);
        Decorate(subgroup_local_invocation_id, spv::Decoration::Flat);
        interfaces.push_back(subgroup_local_invocation_id);
    }
    const Id slot_var{AddGlobalVariable(TypePointer(spv::StorageClass::Private, U32[1]),
                                        spv::StorageClass::Private,
                                        Const(Scratch::INVALID_SLOT))};
    const Id live_var{AddGlobalVariable(TypePointer(spv::StorageClass::Private, U1),
                                        spv::StorageClass::Private, false_value)};
    local_memory_stride_var = AddGlobalVariable(TypePointer(spv::StorageClass::Private, U32[1]),
                                                spv::StorageClass::Private, u32_zero_value);
    Name(slot_var, "local_memory_slot");
    Name(live_var, "local_memory_live");
    Name(local_memory_stride_var, "local_memory_stride");
    if (profile.supported_spirv >= 0x00010400) {
        interfaces.push_back(local_memory_scratch);
        interfaces.push_back(slot_var);
        interfaces.push_back(live_var);
        interfaces.push_back(local_memory_stride_var);
    }
    const u32 mask_words{MaskWords(*this, tracks_lanes)};
    local_memory_acquire_func =
        DefineAcquire(*this, slot_var, live_var, local_memory_stride_var, mask_words);
    local_memory_release_func = DefineRelease(*this, slot_var, live_var, mask_words);
}

} // namespace Shader::Backend::SPIRV
