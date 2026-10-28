// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
Id SubgroupScope(EmitContext& ctx) {
    return ctx.Const(static_cast<u32>(spv::Scope::Subgroup));
}

bool StageSupportsSubgroups(EmitContext& ctx) {
    return ctx.profile.SupportsSubgroupStage(ctx.stage);
}

Id GetThreadId(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

bool QuadShufflesThroughDerivatives(EmitContext& ctx) {
    return ctx.profile.has_helperless_quad_shuffles && ctx.stage == Stage::Fragment;
}

// Quad lane bit of this pixel along one axis: quads start on even window coordinates.
Id QuadAxisBit(EmitContext& ctx, u32 component) {
    const Id pointer{ctx.OpAccessChain(ctx.input_f32, ctx.input_position, ctx.Const(component))};
    const Id coord{ctx.OpConvertFToU(ctx.U32[1], ctx.OpLoad(ctx.F32[1], pointer))};
    return ctx.OpBitwiseAnd(ctx.U32[1], coord, ctx.Const(1U));
}

// Value of the quad neighbour along one axis when cross is true, else the lane's own. A fine
// derivative is the second lane's value minus the first's, exact for 16-bit integers.
Id QuadAcross(EmitContext& ctx, Id half, Id axis_bit, Id cross, bool vertical) {
    const Id delta{vertical ? ctx.OpDPdyFine(ctx.F32[1], half) : ctx.OpDPdxFine(ctx.F32[1], half)};
    const Id is_second{ctx.OpINotEqual(ctx.U1, axis_bit, ctx.u32_zero_value)};
    const Id across{ctx.OpSelect(ctx.F32[1], is_second, ctx.OpFSub(ctx.F32[1], half, delta),
                                 ctx.OpFAdd(ctx.F32[1], half, delta))};
    return ctx.OpSelect(ctx.F32[1], cross, across, half);
}

// Reads the quad lane across the axes flagged by cross_x and cross_y through fine derivatives,
// which run on helper lanes on every host. The value moves as two 16-bit halves held in floats.
Id QuadExchange(EmitContext& ctx, Id value, Id cross_x, Id cross_y) {
    const Id x_bit{QuadAxisBit(ctx, 0)};
    const Id y_bit{QuadAxisBit(ctx, 1)};
    Id result{ctx.u32_zero_value};
    for (const u32 shift : {0U, 16U}) {
        const Id bits{ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.Const(shift), ctx.Const(16U))};
        Id half{ctx.OpConvertUToF(ctx.F32[1], bits)};
        half = QuadAcross(ctx, half, x_bit, cross_x, false);
        half = QuadAcross(ctx, half, y_bit, cross_y, true);
        const Id moved{ctx.OpShiftLeftLogical(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], half),
                                              ctx.Const(shift))};
        result = ctx.OpBitwiseOr(ctx.U32[1], result, moved);
    }
    return result;
}

Id WarpExtract(EmitContext& ctx, Id value) {
    const Id thread_id{GetThreadId(ctx)};
    const Id local_index{ctx.OpShiftRightArithmetic(ctx.U32[1], thread_id, ctx.Const(5U))};
    if (ctx.profile.has_broken_spirv_subgroup_mask_vector_extract_dynamic) {
        const Id c0_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(0U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 0U), ctx.Const(0U))};
        const Id c1_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(1U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 1U), ctx.Const(0U))};
        const Id c2_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(2U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 2U), ctx.Const(0U))};
        const Id c3_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(3U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 3U), ctx.Const(0U))};
        const Id c0_or_c1{ctx.OpBitwiseOr(ctx.U32[1], c0_sel, c1_sel)};
        const Id c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c2_sel, c3_sel)};
        const Id c0_or_c1_or_c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c0_or_c1, c2_or_c3)};
        return c0_or_c1_or_c2_or_c3;
    } else {
        return ctx.OpVectorExtractDynamic(ctx.U32[1], value, local_index);
    }
}

Id LoadMask(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpLoad(ctx.U32[4], mask)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], value, 0U);
    }
    return WarpExtract(ctx, value);
}

void SetInBoundsFlag(IR::Inst* inst, Id result) {
    IR::Inst* const in_bounds{inst->GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)};
    if (!in_bounds) {
        return;
    }
    in_bounds->SetDefinition(result);
    in_bounds->Invalidate();
}

Id ComputeMinThreadId(EmitContext& ctx, Id thread_id, Id segmentation_mask) {
    return ctx.OpBitwiseAnd(ctx.U32[1], thread_id, segmentation_mask);
}

Id ComputeMaxThreadId(EmitContext& ctx, Id min_thread_id, Id clamp, Id not_seg_mask) {
    return ctx.OpBitwiseOr(ctx.U32[1], min_thread_id,
                           ctx.OpBitwiseAnd(ctx.U32[1], clamp, not_seg_mask));
}

Id GetMaxThreadId(EmitContext& ctx, Id thread_id, Id clamp, Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    return ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask);
}

Id SelectValue(EmitContext& ctx, Id in_range, Id value, Id src_thread_id) {
    if (!StageSupportsSubgroups(ctx)) {
        return value;
    }
    return ctx.OpSelect(
        ctx.U32[1], in_range,
        ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, src_thread_id), value);
}

Id AddPartitionBase(EmitContext& ctx, Id thread_id) {
    const Id partition_idx{ctx.OpShiftRightLogical(ctx.U32[1], GetThreadId(ctx), ctx.Const(5u))};
    const Id partition_base{ctx.OpShiftLeftLogical(ctx.U32[1], partition_idx, ctx.Const(5u))};
    return ctx.OpIAdd(ctx.U32[1], thread_id, partition_base);
}
} // Anonymous namespace

Id EmitLaneId(EmitContext& ctx) {
    const Id id{GetThreadId(ctx)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return id;
    }
    return ctx.OpBitwiseAnd(ctx.U32[1], id, ctx.Const(31U));
}

Id EmitVoteAll(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return pred;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpGroupNonUniformAll(ctx.U1, SubgroupScope(ctx), pred);
    }
    const Id mask_ballot{
        ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
    const Id active_mask{WarpExtract(ctx, mask_ballot)};
    const Id ballot{
        WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], ballot, active_mask)};
    return ctx.OpIEqual(ctx.U1, lhs, active_mask);
}

Id EmitVoteAny(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return pred;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpGroupNonUniformAny(ctx.U1, SubgroupScope(ctx), pred);
    }
    const Id mask_ballot{
        ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
    const Id active_mask{WarpExtract(ctx, mask_ballot)};
    const Id ballot{
        WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], ballot, active_mask)};
    return ctx.OpINotEqual(ctx.U1, lhs, ctx.u32_zero_value);
}

Id EmitVoteEqual(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.true_value;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpGroupNonUniformAllEqual(ctx.U1, SubgroupScope(ctx), pred);
    }
    const Id mask_ballot{
        ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), ctx.true_value)};
    const Id active_mask{WarpExtract(ctx, mask_ballot)};
    const Id ballot{
        WarpExtract(ctx, ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred))};
    const Id lhs{ctx.OpBitwiseXor(ctx.U32[1], ballot, active_mask)};
    return ctx.OpLogicalOr(ctx.U1, ctx.OpIEqual(ctx.U1, lhs, ctx.u32_zero_value),
                           ctx.OpIEqual(ctx.U1, lhs, active_mask));
}

Id EmitSubgroupBallot(EmitContext& ctx, Id pred) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.OpSelect(ctx.U32[1], pred, ctx.Const(1u), ctx.u32_zero_value);
    }
    const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), pred)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], ballot, 0U);
    }
    return WarpExtract(ctx, ballot);
}

Id EmitSubgroupEqMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    return LoadMask(ctx, ctx.subgroup_mask_eq);
}

Id EmitSubgroupLtMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return LoadMask(ctx, ctx.subgroup_mask_lt);
}

Id EmitSubgroupLeMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    return LoadMask(ctx, ctx.subgroup_mask_le);
}

Id EmitSubgroupGtMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return LoadMask(ctx, ctx.subgroup_mask_gt);
}

Id EmitSubgroupGeMask(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.Const(1u);
    }
    return LoadMask(ctx, ctx.subgroup_mask_ge);
}

Id EmitShuffleIndex(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                    Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    const Id max_thread_id{ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask)};

    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], index, not_seg_mask)};
    Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], lhs, min_thread_id)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitShuffleUp(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                 Id segmentation_mask) {
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    Id src_thread_id{ctx.OpISub(ctx.U32[1], thread_id, index)};
    const Id in_range{ctx.OpSGreaterThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitShuffleDown(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                   Id segmentation_mask) {
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    Id src_thread_id{ctx.OpIAdd(ctx.U32[1], thread_id, index)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitShuffleButterfly(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                        Id segmentation_mask) {
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    Id src_thread_id{ctx.OpBitwiseXor(ctx.U32[1], thread_id, index)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    if (ctx.profile.warp_size_potentially_larger_than_guest) {
        src_thread_id = AddPartitionBase(ctx, src_thread_id);
    }

    SetInBoundsFlag(inst, in_range);
    return SelectValue(ctx, in_range, value, src_thread_id);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, Id lane) {
    if (QuadShufflesThroughDerivatives(ctx)) {
        const Id lane_x{ctx.OpBitwiseAnd(ctx.U32[1], lane, ctx.Const(1U))};
        const Id lane_y{ctx.OpBitFieldUExtract(ctx.U32[1], lane, ctx.Const(1U), ctx.Const(1U))};
        return QuadExchange(ctx, value, ctx.OpINotEqual(ctx.U1, QuadAxisBit(ctx, 0), lane_x),
                            ctx.OpINotEqual(ctx.U1, QuadAxisBit(ctx, 1), lane_y));
    }
    if (ctx.profile.support_quad_shuffles) {
        return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, lane);
    }
    const Id base{ctx.OpBitwiseAnd(ctx.U32[1], GetThreadId(ctx), ctx.Const(~3u))};
    const Id local_lane{ctx.OpBitwiseAnd(ctx.U32[1], lane, ctx.Const(3u))};
    const Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], base, local_lane)};
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, src_thread_id);
}

Id EmitQuadSwap(EmitContext& ctx, Id value, Id direction) {
    const Id xor_mask{ctx.OpIAdd(ctx.U32[1], direction, ctx.Const(1u))};
    if (QuadShufflesThroughDerivatives(ctx)) {
        const auto crosses{[&](u32 bit) {
            return ctx.OpINotEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], xor_mask, ctx.Const(bit)),
                                   ctx.u32_zero_value);
        }};
        return QuadExchange(ctx, value, crosses(1U), crosses(2U));
    }
    if (ctx.profile.support_quad_shuffles) {
        // Quad broadcasts include helper lanes; a shuffle may not (D3D12 before SM 6.7), and
        // manual derivatives then read garbage at triangle edges. Sirit has no OpGroupNonUniform-
        // QuadSwap, so broadcast every quad lane and pick the one across.
        const Id source{ctx.OpBitwiseXor(
            ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], GetThreadId(ctx), ctx.Const(3u)), xor_mask)};
        Id result{ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value,
                                                     ctx.Const(3u))};
        for (u32 lane = 3; lane-- > 0;) {
            const Id lane_value{ctx.OpGroupNonUniformQuadBroadcast(
                ctx.U32[1], SubgroupScope(ctx), value, ctx.Const(lane))};
            result = ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, source, ctx.Const(lane)),
                                  lane_value, result);
        }
        return result;
    }
    return ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, xor_mask);
}

Id EmitFSwizzleAdd(EmitContext& ctx, Id op_a, Id op_b, Id swizzle) {
    const Id three{ctx.Const(3U)};
    Id mask{GetThreadId(ctx)};
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);
    mask = ctx.OpShiftLeftLogical(ctx.U32[1], mask, ctx.Const(1U));
    mask = ctx.OpShiftRightLogical(ctx.U32[1], swizzle, mask);
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);

    const Id modifier_a{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_a, mask)};
    const Id modifier_b{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_b, mask)};

    const Id result_a{ctx.OpFMul(ctx.F32[1], op_a, modifier_a)};
    const Id result_b{ctx.OpFMul(ctx.F32[1], op_b, modifier_b)};
    return ctx.OpFAdd(ctx.F32[1], result_a, result_b);
}

Id EmitDPdxFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxFine(ctx.F32[1], op_a);
}

Id EmitDPdyFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyFine(ctx.F32[1], op_a);
}

Id EmitDPdxCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxCoarse(ctx.F32[1], op_a);
}

Id EmitDPdyCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyCoarse(ctx.F32[1], op_a);
}

} // namespace Shader::Backend::SPIRV
