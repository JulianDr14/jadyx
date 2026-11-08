// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/tessellation_emulation.h"
#include "shader_recompiler/transform_feedback_storage.h"

namespace Shader::Backend::SPIRV {
namespace {
void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void SetFixedPipelinePointSize(EmitContext& ctx) {
    if (ctx.runtime_info.fixed_state_point_size) {
        const float point_size{*ctx.runtime_info.fixed_state_point_size};
        ctx.OpStore(ctx.output_point_size, ctx.Const(point_size));
    }
}

Id DefaultVarying(EmitContext& ctx, u32 num_components, u32 element, Id zero, Id one,
                  Id default_vector) {
    switch (num_components) {
    case 1:
        return element == 3 ? one : zero;
    case 2:
        return ctx.ConstantComposite(ctx.F32[2], zero, element + 1 == 3 ? one : zero);
    case 3:
        return ctx.ConstantComposite(ctx.F32[3], zero, zero, element + 2 == 3 ? one : zero);
    case 4:
        return default_vector;
    }
    throw InvalidArgument("Bad element");
}

Id ComparisonFunction(EmitContext& ctx, CompareFunction comparison, Id operand_1, Id operand_2) {
    switch (comparison) {
    case CompareFunction::Never:
        return ctx.false_value;
    case CompareFunction::Less:
        return ctx.OpFOrdLessThan(ctx.U1, operand_1, operand_2);
    case CompareFunction::Equal:
        return ctx.OpFOrdEqual(ctx.U1, operand_1, operand_2);
    case CompareFunction::LessThanEqual:
        return ctx.OpFOrdLessThanEqual(ctx.U1, operand_1, operand_2);
    case CompareFunction::Greater:
        return ctx.OpFOrdGreaterThan(ctx.U1, operand_1, operand_2);
    case CompareFunction::NotEqual:
        return ctx.OpFOrdNotEqual(ctx.U1, operand_1, operand_2);
    case CompareFunction::GreaterThanEqual:
        return ctx.OpFOrdGreaterThanEqual(ctx.U1, operand_1, operand_2);
    case CompareFunction::Always:
        return ctx.true_value;
    }
    throw InvalidArgument("Comparison function {}", comparison);
}

/// Output 0's alpha as the shader wrote it, before any rounding for a UNORM8 target.
Id FragAlpha(EmitContext& ctx) {
    if (Sirit::ValidId(ctx.frag_alpha_unrounded)) {
        return ctx.OpLoad(ctx.F32[1], ctx.frag_alpha_unrounded);
    }
    const Id rt0_color{ctx.OpLoad(ctx.F32[4], ctx.frag_color[0])};
    return ctx.OpCompositeExtract(ctx.F32[1], rt0_color, 3u);
}

void AlphaTest(EmitContext& ctx) {
    if (!ctx.runtime_info.alpha_test_func) {
        return;
    }
    const auto comparison{*ctx.runtime_info.alpha_test_func};
    if (comparison == CompareFunction::Always) {
        return;
    }
    if (!Sirit::ValidId(ctx.frag_color[0])) {
        return;
    }

    const Id alpha{FragAlpha(ctx)};

    const Id true_label{ctx.OpLabel()};
    const Id discard_label{ctx.OpLabel()};
    const Id alpha_reference{ctx.Const(ctx.runtime_info.alpha_test_reference)};
    const Id condition{ComparisonFunction(ctx, comparison, alpha, alpha_reference)};

    ctx.OpSelectionMerge(true_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, true_label, discard_label);
    ctx.AddLabel(discard_label);
    ctx.OpKill();
    ctx.AddLabel(true_label);
}

/// Ends the invocation when keep is false.
void DiscardUnless(EmitContext& ctx, Id keep) {
    const Id keep_label{ctx.OpLabel()};
    const Id discard_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(keep_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(keep, keep_label, discard_label);
    ctx.AddLabel(discard_label);
    ctx.OpKill();
    ctx.AddLabel(keep_label);
}

/// Line stipple as a discard (RuntimeInfo::line_stipple_generic).
void LineStipple(EmitContext& ctx) {
    const u32 generic{ctx.runtime_info.line_stipple_generic};
    if (generic == 0 || !Sirit::ValidId(ctx.input_generics[generic - 1].id)) {
        return;
    }
    const Id input{ctx.OpLoad(ctx.F32[4], ctx.input_generics[generic - 1].id)};
    const Id pixels{ctx.OpCompositeExtract(ctx.F32[1], input, 0U)};
    const Id step{ctx.OpFDiv(ctx.F32[1], pixels,
                             ctx.Const(static_cast<f32>(ctx.runtime_info.line_stipple_factor)))};
    const Id bit{ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], step), ctx.Const(15u))};
    const Id shifted{
        ctx.OpShiftRightLogical(ctx.U32[1], ctx.Const(ctx.runtime_info.line_stipple_pattern), bit)};
    DiscardUnless(ctx, ctx.OpINotEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], shifted, ctx.Const(1u)),
                                       ctx.u32_zero_value));
}

/// Polygon stipple as a discard: 32 rows of 32 pixels, the leftmost in the row's top bit.
void PolygonStipple(EmitContext& ctx) {
    if (!ctx.runtime_info.polygon_stipple || !Sirit::ValidId(ctx.input_position) ||
        !Sirit::ValidId(ctx.polygon_stipple_pointer)) {
        return;
    }
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.input_position)};
    const auto pixel{[&](u32 axis) {
        const Id coord{ctx.OpCompositeExtract(ctx.F32[1], position, axis)};
        return ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], coord), ctx.Const(31u));
    }};
    const Id row_pointer{ctx.OpAccessChain(ctx.polygon_stipple_pointer,
                                           ctx.stage_constants_push_constant,
                                           ctx.Const(STAGE_CONSTANT_WORDS), pixel(1))};
    // Bytes in order, each read from its top bit: nouveau byte-swaps OpenGL's words for this register.
    const Id shift{ctx.OpBitwiseXor(ctx.U32[1], pixel(0), ctx.Const(7u))};
    const Id bit{ctx.OpShiftRightLogical(ctx.U32[1], ctx.OpLoad(ctx.U32[1], row_pointer), shift)};
    DiscardUnless(ctx, ctx.OpINotEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], bit, ctx.Const(1u)),
                                       ctx.u32_zero_value));
}

/// The logic op D3D12 does not have everywhere (RuntimeInfo::logic_op), on output 0 and in the
/// target's own bits. The target is read from the copy made before the draw.
void LogicOp(EmitContext& ctx) {
    const auto& info{ctx.runtime_info};
    if (!info.logic_op_enable || !Sirit::ValidId(ctx.frag_color[0])) {
        return;
    }
    const Id source{ctx.OpLoad(ctx.F32[4], ctx.frag_color[0])};
    Id destination{};
    if (Sirit::ValidId(ctx.destination_read)) {
        const Id position{ctx.OpLoad(ctx.F32[4], ctx.input_position)};
        const Id coord{ctx.OpConvertFToU(ctx.U32[2], ctx.OpVectorShuffle(ctx.F32[2], position,
                                                                         position, 0U, 1U))};
        const std::array lod{ctx.u32_zero_value};
        destination = ctx.OpImageFetch(ctx.F32[4],
                                       ctx.OpLoad(ctx.destination_read_type, ctx.destination_read),
                                       coord, spv::ImageOperandsMask::Lod, lod);
    }
    const u32 op{info.logic_op};
    std::array<Id, 4> result{};
    for (u32 channel = 0; channel < 4; ++channel) {
        const Id max{ctx.Const((u32{1} << info.logic_op_bits[channel]) - 1)};
        const Id max_float{ctx.OpConvertUToF(ctx.F32[1], max)};
        const auto bits{[&](Id color) {
            const Id value{ctx.OpCompositeExtract(ctx.F32[1], color, channel)};
            const Id clamped{ctx.OpFClamp(ctx.F32[1], value, ctx.Const(0.0f), ctx.Const(1.0f))};
            return ctx.OpConvertFToU(ctx.U32[1], ctx.OpFAdd(ctx.F32[1], ctx.Const(0.5f),
                                                            ctx.OpFMul(ctx.F32[1], clamped,
                                                                       max_float)));
        }};
        const Id s{bits(source)};
        const Id d{Sirit::ValidId(destination) ? bits(destination) : ctx.u32_zero_value};
        const Id not_s{ctx.OpBitwiseXor(ctx.U32[1], s, max)};
        const Id not_d{ctx.OpBitwiseXor(ctx.U32[1], d, max)};
        Id bitwise{ctx.u32_zero_value};
        const auto add{[&](u32 bit, Id a, Id b) {
            if ((op >> bit) & 1) {
                bitwise = ctx.OpBitwiseOr(ctx.U32[1], bitwise, ctx.OpBitwiseAnd(ctx.U32[1], a, b));
            }
        }};
        add(0, s, d);
        add(1, s, not_d);
        add(2, not_s, d);
        add(3, not_s, not_d);
        result[channel] = ctx.OpFDiv(ctx.F32[1], ctx.OpConvertUToF(ctx.F32[1], bitwise), max_float);
    }
    ctx.OpStore(ctx.frag_color[0],
                ctx.OpCompositeConstruct(ctx.F32[4], result[0], result[1], result[2], result[3]));
}

/// The guest's viewport swizzle (RuntimeInfo::viewport_swizzle) on the position about to be used.
void ViewportSwizzle(EmitContext& ctx) {
    const u32 swizzle{ctx.runtime_info.viewport_swizzle};
    if (swizzle == 0) {
        return;
    }
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const auto component{[&](u32 output) {
        const u32 selector{(swizzle >> (output * 4)) & 7};
        const Id value{ctx.OpCompositeExtract(ctx.F32[1], position, selector / 2)};
        return (selector & 1) != 0 ? ctx.OpFNegate(ctx.F32[1], value) : value;
    }};
    ctx.OpStore(ctx.output_position,
                ctx.OpCompositeConstruct(ctx.F32[4], component(0), component(1), component(2),
                                         component(3)));
}

/// Whether the pixel's window coordinate on this axis is odd, position being the loaded FragCoord.
Id OddPixel(EmitContext& ctx, Id position, u32 axis) {
    const Id coord{ctx.OpCompositeExtract(ctx.F32[1], position, axis)};
    const Id pixel{ctx.OpConvertFToU(ctx.U32[1], coord)};
    const Id one{ctx.Const(1u)};
    return ctx.OpIEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], pixel, one), one);
}

/// Alpha-to-coverage as a discard (RuntimeInfo::alpha_to_coverage_footprint): a single sample
/// is covered when output 0's alpha reaches the threshold of its pixel, 0.5 or a 2x2 ordered
/// dither of the four quarter steps.
void AlphaToCoverage(EmitContext& ctx) {
    const u32 footprint{ctx.runtime_info.alpha_to_coverage_footprint};
    if (footprint == 0 || !Sirit::ValidId(ctx.frag_color[0])) {
        return;
    }
    const Id type{ctx.F32[1]};
    const Id alpha{FragAlpha(ctx)};
    Id threshold{ctx.Const(0.5f)};
    if (footprint == 2 && Sirit::ValidId(ctx.input_position)) {
        const Id position{ctx.OpLoad(ctx.F32[4], ctx.input_position)};
        const Id odd_x{OddPixel(ctx, position, 0)};
        const Id odd_y{OddPixel(ctx, position, 1)};
        const Id even_row{ctx.OpSelect(type, odd_x, ctx.Const(0.625f), ctx.Const(0.125f))};
        const Id odd_row{ctx.OpSelect(type, odd_x, ctx.Const(0.375f), ctx.Const(0.875f))};
        threshold = ctx.OpSelect(type, odd_y, odd_row, even_row);
    }
    const Id covered_label{ctx.OpLabel()};
    const Id discard_label{ctx.OpLabel()};
    const Id uncovered{ctx.OpFOrdLessThan(ctx.U1, alpha, threshold)};
    ctx.OpSelectionMerge(covered_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(uncovered, discard_label, covered_label);
    ctx.AddLabel(discard_label);
    ctx.OpKill();
    ctx.AddLabel(covered_label);
}

/// The source blend factors the host's blend constant cannot serve, on each output's RGB
/// (RuntimeInfo::blend_constant_factors). The host pipeline blends these outputs with factor one.
void ShaderBlendConstants(EmitContext& ctx) {
    if (!Sirit::ValidId(ctx.blend_constants_push_constant)) {
        return;
    }
    const Id type{ctx.F32[1]};
    const Id pointer_type{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.F32[4])};
    const Id constant{ctx.OpLoad(ctx.F32[4], ctx.OpAccessChain(pointer_type,
                                                               ctx.blend_constants_push_constant,
                                                               ctx.u32_zero_value))};
    const Id one{ctx.Const(1.0f)};
    for (u32 index = 0; index < 8; ++index) {
        const BlendConstantFactor factor{ctx.runtime_info.blend_constant_factors[index]};
        if (factor == BlendConstantFactor::None || !Sirit::ValidId(ctx.frag_color[index]) ||
            ctx.runtime_info.color_output_types[index] != AttributeType::Float) {
            continue;
        }
        const bool alpha{factor == BlendConstantFactor::Alpha ||
                         factor == BlendConstantFactor::OneMinusAlpha};
        const bool inverted{factor == BlendConstantFactor::OneMinusColor ||
                            factor == BlendConstantFactor::OneMinusAlpha};
        const auto channel{[&](u32 element) {
            const Id value{ctx.OpCompositeExtract(type, constant, alpha ? 3U : element)};
            return inverted ? ctx.OpFSub(type, one, value) : value;
        }};
        const Id scale{ctx.OpCompositeConstruct(ctx.F32[4], channel(0), channel(1), channel(2), one)};
        const Id color{ctx.OpLoad(ctx.F32[4], ctx.frag_color[index])};
        ctx.OpStore(ctx.frag_color[index], ctx.OpFMul(ctx.F32[4], color, scale));
    }
}

/// The guest's sample mask for this pixel's position (RuntimeInfo::sample_mask_footprint), on top
/// of the coverage the shader wrote. Samples above the fourth stay on, as in the host pipeline's.
void ShaderSampleMask(EmitContext& ctx) {
    const u32 footprint{ctx.runtime_info.sample_mask_footprint};
    if (footprint == 0 || !Sirit::ValidId(ctx.input_position) ||
        !Sirit::ValidId(ctx.sample_mask)) {
        return;
    }
    const auto keep{[&](u32 index) { return ctx.Const(~((footprint >> (index * 4)) & 0xF)); }};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.input_position)};
    const Id odd_x{OddPixel(ctx, position, 0)};
    const Id odd_y{OddPixel(ctx, position, 1)};
    const Id even_row{ctx.OpSelect(ctx.U32[1], odd_x, keep(1), keep(0))};
    const Id odd_row{ctx.OpSelect(ctx.U32[1], odd_x, keep(3), keep(2))};
    Id mask{ctx.OpSelect(ctx.U32[1], odd_y, odd_row, even_row)};
    const Id pointer{ctx.OpAccessChain(ctx.output_u32, ctx.sample_mask, ctx.u32_zero_value)};
    if (ctx.guest_writes_sample_mask) {
        mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, ctx.OpLoad(ctx.U32[1], pointer));
    }
    ctx.OpStore(pointer, mask);
}

/// Writes the rasterized depth plus RuntimeInfo's depth bias, as the host would have added it.
void ShaderDepthBias(EmitContext& ctx) {
    if (!ctx.shader_depth_bias || !Sirit::ValidId(ctx.input_position)) {
        return;
    }
    const RuntimeInfo& info{ctx.runtime_info};
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.input_position)};
    const Id depth{ctx.OpCompositeExtract(type, position, 2u)};
    Id unit{ctx.Const(info.depth_bias_unit)};
    if (info.depth_bias_unit == 0.0f) {
        // 2^(exponent - 23): the exponent bits alone are 2^exponent.
        const Id exponent{ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], depth),
                                           ctx.Const(0x7f800000u))};
        unit = ctx.OpFMul(type, ctx.OpBitcast(type, exponent), ctx.Const(0x1p-23f));
    }
    const Id slope{ctx.OpFMax(type, ctx.OpFAbs(type, ctx.OpDPdx(type, depth)),
                              ctx.OpFAbs(type, ctx.OpDPdy(type, depth)))};
    Id bias{ctx.OpFma(type, slope, ctx.Const(info.depth_bias_slope),
                      ctx.OpFMul(type, unit, ctx.Const(info.depth_bias_constant)))};
    if (info.depth_bias_clamp > 0.0f) {
        bias = ctx.OpFMin(type, bias, ctx.Const(info.depth_bias_clamp));
    } else if (info.depth_bias_clamp < 0.0f) {
        bias = ctx.OpFMax(type, bias, ctx.Const(info.depth_bias_clamp));
    }
    ctx.OpStore(ctx.frag_depth, ctx.OpFAdd(type, depth, bias));
}

/// Component of a captured output (an IsCopyableCapture attribute), with its bits as a float, or
/// its default when the shader does not write it (the capture is undefined then, as on Maxwell).
Id CapturedComponent(EmitContext& ctx, u32 attribute) {
    const auto attr{static_cast<IR::Attribute>(attribute)};
    // Vectors default to (0, 0, 0, 1), like the vertex stage prologue writes them.
    const Id vector_default{ctx.Const(attribute % 4 == 3 ? 1.0f : 0.0f)};
    const auto load_float = [&](Id output, std::optional<u32> element = std::nullopt,
                                Id fallback = {}) {
        if (!Sirit::ValidId(output)) {
            return Sirit::ValidId(fallback) ? fallback : ctx.f32_zero_value;
        }
        const Id pointer{element ? ctx.OpAccessChain(ctx.output_f32, output, ctx.Const(*element))
                                 : output};
        return ctx.OpLoad(ctx.F32[1], pointer);
    };
    const auto load_integer = [&](Id output) {
        return Sirit::ValidId(output)
                   ? ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], output))
                   : ctx.f32_zero_value;
    };
    if (attr >= IR::Attribute::PositionX && attr <= IR::Attribute::PositionW) {
        return load_float(ctx.output_position, attribute % 4, vector_default);
    }
    if (attr >= IR::Attribute::ClipDistance0 && attr <= IR::Attribute::ClipDistance7) {
        const u32 index{attribute - static_cast<u32>(IR::Attribute::ClipDistance0)};
        return index < ctx.profile.max_user_clip_distances ? load_float(ctx.clip_distances, index)
                                                           : ctx.f32_zero_value;
    }
    if (IR::IsGeneric(attr)) {
        const GenericElementInfo& info{
            ctx.output_generics[IR::GenericAttributeIndex(attr)][IR::GenericAttributeElement(attr)]};
        if (info.num_components == 1) {
            return load_float(info.id, std::nullopt, vector_default);
        }
        return load_float(info.id, IR::GenericAttributeElement(attr) - info.first_element,
                          vector_default);
    }
    switch (attr) {
    case IR::Attribute::PointSize:
        return load_float(ctx.output_point_size);
    case IR::Attribute::Layer:
        return load_integer(ctx.layer);
    case IR::Attribute::ViewportIndex:
        return load_integer(ctx.viewport_index);
    default:
        throw InvalidArgument("Attribute {} has no captured value", attr);
    }
}

/// Writes the captured outputs of this vertex to the transform feedback buffers and, when the
/// guest rasterizer is disabled, moves the vertex behind the viewer (transform_feedback_storage.h).
/// Runs before every position transform, so the buffers get the outputs the guest wrote.
void WriteTransformFeedbackStorage(EmitContext& ctx) {
    namespace Xfb = TransformFeedbackStorage;
    if (!Sirit::ValidId(ctx.xfb_storage_constants)) {
        return;
    }
    const Id u32_type{ctx.U32[1]};
    const auto constant = [&](u32 word) {
        return ctx.OpLoad(u32_type, ctx.OpAccessChain(ctx.xfb_storage_constant_element,
                                                      ctx.xfb_storage_constants, ctx.Const(word)));
    };
    const Id vertex{ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.vertex_index),
                               ctx.OpLoad(u32_type, ctx.base_vertex))};
    const Id instance{ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.instance_index),
                                 ctx.OpLoad(u32_type, ctx.base_instance))};
    const Id slot{ctx.OpIAdd(u32_type, vertex,
                             ctx.OpIMul(u32_type, instance, constant(Xfb::VERTEX_COUNT_WORD)))};
    const auto store = [&](u32 attribute, const TransformFeedbackVarying& varying) {
        // Other attributes are not outputs (layout gaps): their words stay as they were.
        if (varying.buffer >= Xfb::NUM_BUFFERS ||
            !IsCopyableCapture(static_cast<IR::Attribute>(attribute))) {
            return;
        }
        std::array<Id, 4> values{};
        for (u32 component = 0; component < varying.components; ++component) {
            values[component] =
                ctx.OpBitcast(u32_type, CapturedComponent(ctx, attribute + component));
        }
        const Id slot_word{ctx.OpIMul(u32_type, slot, ctx.Const(varying.stride / 4))};
        const Id first_word{ctx.OpIAdd(u32_type, slot_word, ctx.Const(varying.offset / 4))};
        const Id end_word{ctx.OpIAdd(u32_type, first_word, ctx.Const(varying.components))};
        const Id size{constant(Xfb::SIZE_WORD + varying.buffer)};
        const Id fits{ctx.OpULessThanEqual(ctx.U1, end_word, size)};
        const Id store_label{ctx.OpLabel()};
        const Id merge_label{ctx.OpLabel()};
        ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(fits, store_label, merge_label);
        ctx.AddLabel(store_label);
        for (u32 component = 0; component < varying.components; ++component) {
            const Id word{ctx.OpIAdd(u32_type, first_word, ctx.Const(component))};
            const Id pointer{ctx.OpAccessChain(ctx.xfb_storage_element,
                                               ctx.xfb_storage_buffers[varying.buffer],
                                               ctx.u32_zero_value, word)};
            ctx.OpStore(pointer, values[component]);
        }
        ctx.OpBranch(merge_label);
        ctx.AddLabel(merge_label);
    };
    for (u32 attribute = 0; attribute < ctx.runtime_info.xfb_count; ++attribute) {
        if (const TransformFeedbackVarying* const varying{ctx.CapturedVarying(attribute)}) {
            store(attribute, *varying);
        }
    }
    for (const TransformFeedbackRepeat& repeat : ctx.runtime_info.xfb_repeats) {
        if (ctx.Captures(repeat.varying)) {
            store(repeat.attribute, repeat.varying);
        }
    }
    // A negative W fails every clip plane test (-w <= x <= w), whatever X, Y and Z are.
    const Id discard{ctx.OpINotEqual(ctx.U1, constant(Xfb::DISCARD_WORD), ctx.u32_zero_value)};
    const Id w_pointer{ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.Const(3U))};
    const Id w{ctx.OpLoad(ctx.F32[1], w_pointer)};
    ctx.OpStore(w_pointer, ctx.OpSelect(ctx.F32[1], discard, ctx.Const(-1.0f), w));
}

/// Stores the slots the control shader reads of this vertex to the tessellation capture, then
/// moves the vertex behind the viewer: the capture draw only writes the buffer
/// (tessellation_emulation.h). Runs before every position transform, like the storage above.
void WriteTessellationCapture(EmitContext& ctx) {
    namespace Tess = TessellationEmulation;
    const RuntimeInfo& info{ctx.runtime_info};
    const Id u32_type{ctx.U32[1]};
    const auto constant = [&](u32 word) {
        return ctx.OpLoad(u32_type, ctx.OpAccessChain(ctx.tess_constant_element, ctx.tess_constants,
                                                      ctx.Const(word)));
    };
    const Id vertex{ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.vertex_index),
                               ctx.OpLoad(u32_type, ctx.base_vertex))};
    const Id instance{ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.instance_index),
                                 ctx.OpLoad(u32_type, ctx.base_instance))};
    const Id slot{ctx.OpIAdd(
        u32_type, vertex,
        ctx.OpIMul(u32_type, instance, constant(Tess::VERTICES_PER_INSTANCE_WORD)))};
    std::vector<Id> values;
    for (u32 group = 0; group < info.tess_input_slots.size(); ++group) {
        if (info.tess_input_slots[group] != 0) {
            for (u32 element = 0; element < 4; ++element) {
                values.push_back(ctx.OpBitcast(u32_type, CapturedComponent(ctx, group * 4 + element)));
            }
        }
    }
    const Id fits{ctx.OpULessThan(ctx.U1, slot, constant(Tess::CAPTURE_VERTICES_WORD))};
    const Id store_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(fits, store_label, merge_label);
    ctx.AddLabel(store_label);
    const Id first_word{
        ctx.OpIMul(u32_type, slot, ctx.Const(info.tess_input_vertex_slots * Tess::SLOT_WORDS))};
    u32 offset{};
    for (u32 group = 0; group < info.tess_input_slots.size(); ++group) {
        const u32 input_slot{info.tess_input_slots[group]};
        if (input_slot == 0) {
            continue;
        }
        for (u32 element = 0; element < 4; ++element) {
            const Id word{ctx.OpIAdd(u32_type, first_word,
                                     ctx.Const((input_slot - 1) * Tess::SLOT_WORDS + element))};
            ctx.OpStore(ctx.TessWord(ctx.tess_output_buffer, word), values[offset++]);
        }
    }
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
    // A negative W fails every clip plane test (-w <= x <= w), whatever X, Y and Z are.
    const Id w_pointer{ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.Const(3U))};
    ctx.OpStore(w_pointer, ctx.Const(-1.0f));
}

/// Native transform feedback outputs of this vertex (DefineTransformFeedbackOutputs). Runs before
/// every position transform, so the copies hold the values the guest wrote.
void WriteTransformFeedbackOutputs(EmitContext& ctx) {
    // The vertex stage prologue already wrote the defaults. Copies may read them, so they go first.
    if (ctx.stage != Stage::VertexB && ctx.xfb_unstored_generics != 0) {
        const Id zero{ctx.Const(0.0f)};
        const Id one{ctx.Const(1.0f)};
        const Id default_vector{ctx.ConstantComposite(ctx.F32[4], zero, zero, zero, one)};
        for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
            if ((ctx.xfb_unstored_generics & (1U << index)) == 0) {
                continue;
            }
            const auto& info{ctx.output_generics[index]};
            u32 element{0};
            while (element < 4) {
                const u32 num{info[element].num_components};
                ctx.OpStore(info[element].id,
                            DefaultVarying(ctx, num, element, zero, one, default_vector));
                element += num;
            }
        }
    }
    for (const TransformFeedbackCopy& copy : ctx.xfb_copies) {
        std::array<Id, 4> components{};
        for (u32 i = 0; i < copy.num_components; ++i) {
            components[i] = CapturedComponent(ctx, copy.attributes[i]);
        }
        const std::span<const Id> values(components.data(), copy.num_components);
        ctx.OpStore(copy.id, copy.num_components == 1
                                 ? components[0]
                                 : ctx.OpCompositeConstruct(ctx.F32[copy.num_components], values));
    }
}
} // Anonymous namespace

namespace {
/// The values the emulated tessellation stages index their buffers with (tessellation_emulation.h).
void TessellationEmulationPrologue(EmitContext& ctx) {
    namespace Tess = TessellationEmulation;
    const RuntimeInfo& info{ctx.runtime_info};
    const Id record_words{ctx.Const(Tess::RecordWords(info.tess_record_patch_slots,
                                                      info.tess_output_vertices,
                                                      info.tess_output_vertex_slots))};
    if (info.tess_emulation == TessEmulation::EvalVertex) {
        // The index is the domain point: u and v in 16.16 fixed point, then the patch.
        const Id point{ctx.OpIMul(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.vertex_index),
                                  ctx.Const(Tess::POINT_WORDS))};
        const auto load_point{[&](u32 word) {
            const Id index{ctx.OpIAdd(ctx.U32[1], point, ctx.Const(word))};
            return ctx.OpLoad(ctx.U32[1], ctx.TessWord(ctx.tess_points_buffer, index));
        }};
        // Exact: the fixed point values have at most 17 significant bits.
        const Id scale{ctx.Const(1.0f / 65536.0f)};
        ctx.tess_coord_u = ctx.OpFMul(ctx.F32[1], ctx.OpConvertUToF(ctx.F32[1], load_point(0)), scale);
        ctx.tess_coord_v = ctx.OpFMul(ctx.F32[1], ctx.OpConvertUToF(ctx.F32[1], load_point(1)), scale);
        ctx.tess_record = ctx.OpIMul(ctx.U32[1], load_point(2), record_words);
        const Id primitive{ctx.OpIAdd(ctx.U32[1], ctx.tess_record, ctx.Const(Tess::PRIMITIVE_WORD))};
        ctx.tess_primitive = ctx.OpLoad(ctx.U32[1], ctx.TessWord(ctx.tess_input_buffer, primitive));
        return;
    }
    // One invocation per output vertex, whole patches per group. Invocations past the last patch
    // read the first and write the trash record, so every invocation reaches the barriers.
    const auto constant{[&](u32 word) {
        return ctx.OpLoad(ctx.U32[1], ctx.OpAccessChain(ctx.tess_constant_element,
                                                        ctx.tess_constants, ctx.Const(word)));
    }};
    const Id output_vertices{ctx.Const(info.tess_output_vertices)};
    const Id local{ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id), 0U)};
    const Id group{ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U)};
    const Id local_patch{ctx.OpUDiv(ctx.U32[1], local, output_vertices)};
    ctx.tess_invocation = ctx.OpISub(ctx.U32[1], local,
                                     ctx.OpIMul(ctx.U32[1], local_patch, output_vertices));
    const Id patch{ctx.OpIAdd(
        ctx.U32[1],
        ctx.OpIMul(ctx.U32[1], group, ctx.Const(Tess::PatchesPerGroup(info.tess_output_vertices))),
        local_patch)};
    const Id valid{ctx.OpULessThan(ctx.U1, patch, constant(Tess::TOTAL_PATCHES_WORD))};
    const Id read_patch{ctx.OpSelect(ctx.U32[1], valid, patch, ctx.u32_zero_value)};
    const Id patches_per_instance{constant(Tess::PATCHES_PER_INSTANCE_WORD)};
    const Id instance{ctx.OpUDiv(ctx.U32[1], read_patch, patches_per_instance)};
    ctx.tess_primitive = ctx.OpISub(ctx.U32[1], read_patch,
                                    ctx.OpIMul(ctx.U32[1], instance, patches_per_instance));
    ctx.tess_input_vertex =
        ctx.OpIMul(ctx.U32[1], instance, constant(Tess::VERTICES_PER_INSTANCE_WORD));
    ctx.tess_patch_position =
        ctx.OpIMul(ctx.U32[1], ctx.tess_primitive, ctx.Const(info.tess_input_vertices));
    const Id write_patch{ctx.OpSelect(ctx.U32[1], valid, patch, constant(Tess::TRASH_RECORD_WORD))};
    ctx.tess_record = ctx.OpIMul(ctx.U32[1], write_patch, record_words);
    // Every invocation of the patch stores the same value.
    const Id primitive{ctx.OpIAdd(ctx.U32[1], ctx.tess_record, ctx.Const(Tess::PRIMITIVE_WORD))};
    ctx.OpStore(ctx.TessWord(ctx.tess_output_buffer, primitive), ctx.tess_primitive);
}
} // Anonymous namespace

void EmitPrologue(EmitContext& ctx) {
    if (ctx.EmulatesTessellation()) {
        TessellationEmulationPrologue(ctx);
    }
    if (Sirit::ValidId(ctx.local_memory_acquire_func)) {
        ctx.local_memory_lane_base = ctx.OpFunctionCall(ctx.U32[1], ctx.local_memory_acquire_func);
        ctx.local_memory_stride = ctx.OpLoad(ctx.U32[1], ctx.local_memory_stride_var);
    }
    if (ctx.stage == Stage::Fragment && ctx.runtime_info.dual_source_blend) {
        // Initialize dual-source blending outputs - prevents MoltenVK crash.
        const Id zero{ctx.Const(0.0f)};
        const Id one{ctx.Const(1.0f)};
        const Id default_color{ctx.ConstantComposite(ctx.F32[4], zero, zero, zero, one)};
        for (u32 i = 0; i < 2; ++i) {
            if (Sirit::ValidId(ctx.frag_color[i])) {
                ctx.OpStore(ctx.frag_color[i], default_color);
            }
        }
    }
    if (ctx.stage == Stage::VertexB) {
        const Id zero{ctx.Const(0.0f)};
        const Id one{ctx.Const(1.0f)};
        const Id default_vector{ctx.ConstantComposite(ctx.F32[4], zero, zero, zero, one)};
        ctx.OpStore(ctx.output_position, default_vector);
        for (const auto& info : ctx.output_generics) {
            if (info[0].num_components == 0) {
                continue;
            }
            u32 element{0};
            while (element < 4) {
                const auto& element_info{info[element]};
                const u32 num{element_info.num_components};
                const Id value{DefaultVarying(ctx, num, element, zero, one, default_vector)};
                ctx.OpStore(element_info.id, value);
                element += num;
            }
        }

        if (Sirit::ValidId(ctx.clip_distances)) {
            for (u32 i = 0; i < ctx.profile.max_user_clip_distances; ++i) {
                if (!clip_distance_written.test(i)) {
                    const Id idx = ctx.Const(i);
                    const Id element = ctx.OpAccessChain(ctx.output_f32, ctx.clip_distances, idx);
                    ctx.OpStore(element, ctx.Const(0.0f));
                }
            }
        }
    }
    if (ctx.stage == Stage::VertexB || ctx.stage == Stage::Geometry) {
        SetFixedPipelinePointSize(ctx);
    }
}

void EmitEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::VertexB) {
        WriteTransformFeedbackStorage(ctx);
        if (ctx.runtime_info.tess_capture) {
            WriteTessellationCapture(ctx);
        }
    }
    if (ctx.stage == Stage::VertexB || ctx.stage == Stage::TessellationEval) {
        WriteTransformFeedbackOutputs(ctx);
    }
    if (ctx.stage == Stage::VertexB && ctx.runtime_info.lower_noperspective_mask != 0) {
        // Read final outputs, after every guest store and before clipping. Doing
        // this at each SetAttribute would depend on the order PositionW is written.
        const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
        const Id w{ctx.OpCompositeExtract(ctx.F32[1], position, 3U)};
        for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
            if ((ctx.runtime_info.lower_noperspective_mask & (1U << index)) == 0) {
                continue;
            }
            const auto& output = ctx.output_generics[index][0];
            // Stream output would capture the lowered value; storage capture runs before this.
            bool captured{};
            if (!ctx.runtime_info.xfb_storage) {
                const u32 base{static_cast<u32>(IR::Attribute::Generic0X) + index * 4};
                for (u32 attribute = base; attribute < base + 4; ++attribute) {
                    captured |= ctx.CapturedVarying(attribute) != nullptr;
                }
            }
            if (output.num_components != 4 || captured) {
                throw NotImplementedException(
                    "NoPerspective lowering requires whole, uncaptured VS varyings");
            }
            const Id value{ctx.OpLoad(ctx.F32[4], output.id)};
            ctx.OpStore(output.id, ctx.OpVectorTimesScalar(ctx.F32[4], value, w));
        }
    }
    if (ctx.stage == Stage::VertexB || ctx.stage == Stage::TessellationEval) {
        ViewportSwizzle(ctx);
    }
    // An emulated evaluation shader is the last stage before the rasterizer, as a vertex shader.
    const bool evaluates_as_vertex{ctx.stage == Stage::TessellationEval &&
                                   ctx.runtime_info.tess_emulation == TessEmulation::EvalVertex};
    if ((ctx.stage == Stage::VertexB || evaluates_as_vertex) &&
        ctx.runtime_info.convert_depth_mode && !ctx.profile.support_native_ndc) {
        ConvertDepthMode(ctx);
    }
    // Before the fragment output passes, which may demote the lanes that still own the slot.
    EmitLocalMemoryRelease(ctx);
    if (ctx.stage == Stage::Fragment) {
        LineStipple(ctx);
        PolygonStipple(ctx);
        AlphaTest(ctx);
        AlphaToCoverage(ctx);
        LogicOp(ctx);
        ShaderBlendConstants(ctx);
        ShaderSampleMask(ctx);
        ShaderDepthBias(ctx);
    }
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    // Snapshot before depth conversion: restoring the unconverted position also
    // prevents applying the conversion twice when it is a per-primitive output.
    std::vector<Id> passthrough_values;
    passthrough_values.reserve(ctx.passthrough_outputs.size());
    for (const auto& [variable, type] : ctx.passthrough_outputs) {
        passthrough_values.push_back(ctx.OpLoad(type, variable));
    }
    WriteTransformFeedbackOutputs(ctx);
    ViewportSwizzle(ctx);
    if (ctx.runtime_info.convert_depth_mode && !ctx.profile.support_native_ndc) {
        ConvertDepthMode(ctx);
    }
    if (!ctx.profile.support_geometry_streams) {
        // GeometryStreams is optional, but ordinary geometry shaders can always emit stream 0.
        // Do not silently redirect another (or a dynamically selected) stream to the rasterizer.
        if (!stream.IsImmediate() || stream.U32() != 0) {
            throw NotImplementedException("Nonzero or dynamic geometry stream");
        }
        ctx.OpEmitVertex();
    } else if (stream.IsImmediate()) {
        ctx.OpEmitStreamVertex(ctx.Def(stream));
    } else {
        LOG_WARNING(Shader_SPIRV, "Stream is not immediate");
        ctx.OpEmitStreamVertex(ctx.u32_zero_value);
    }
    for (size_t i = 0; i < passthrough_values.size(); ++i) {
        ctx.OpStore(ctx.passthrough_outputs[i].first, passthrough_values[i]);
    }
    // Restore fixed pipeline point size after emitting the vertex
    SetFixedPipelinePointSize(ctx);
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    if (!ctx.profile.support_geometry_streams) {
        if (!stream.IsImmediate() || stream.U32() != 0) {
            throw NotImplementedException("Nonzero or dynamic geometry stream");
        }
        ctx.OpEndPrimitive();
    } else if (stream.IsImmediate()) {
        ctx.OpEndStreamPrimitive(ctx.Def(stream));
    } else {
        LOG_WARNING(Shader_SPIRV, "Stream is not immediate");
        ctx.OpEndStreamPrimitive(ctx.u32_zero_value);
    }
}

} // namespace Shader::Backend::SPIRV
