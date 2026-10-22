// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
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

    const Id type{ctx.F32[1]};
    const Id rt0_color{ctx.OpLoad(ctx.F32[4], ctx.frag_color[0])};
    const Id alpha{ctx.OpCompositeExtract(type, rt0_color, 3u)};

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

void EmitPrologue(EmitContext& ctx) {
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
    if (ctx.stage == Stage::VertexB && ctx.runtime_info.convert_depth_mode &&
        !ctx.profile.support_native_ndc) {
        ConvertDepthMode(ctx);
    }
    if (ctx.stage == Stage::Fragment) {
        EmitLocalMemoryRelease(ctx);
        AlphaTest(ctx);
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
