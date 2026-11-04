// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <vector>
#include <queue>

#include "common/settings.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/post_order.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Maxwell {
namespace {
IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& node : syntax_list) {
        if (node.type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks;
    blocks.reserve(num_syntax_blocks);
    u32 order_index{};
    for (const auto& node : syntax_list) {
        if (node.type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(node.data.block);
            blocks.back()->SetOrder(order_index++);
        }
    }
    return blocks;
}

void RemoveUnreachableBlocks(IR::Program& program) {
    // Some blocks might be unreachable if a function call exists unconditionally
    // If this happens the number of blocks and post order blocks will mismatch
    if (program.blocks.size() == program.post_order_blocks.size()) {
        return;
    }
    const auto begin{program.blocks.begin() + 1};
    const auto end{program.blocks.end()};
    const auto pred{[](IR::Block* block) { return block->ImmPredecessors().empty(); }};
    program.blocks.erase(std::remove_if(begin, end, pred), end);
}

void CollectInterpolationInfo(Environment& env, IR::Program& program) {
    if (program.stage != Stage::Fragment) {
        return;
    }
    const ProgramHeader& sph{env.SPH()};
    for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
        std::optional<PixelImap> imap;
        for (const PixelImap value : sph.ps.GenericInputMap(static_cast<u32>(index))) {
            if (value == PixelImap::Unused) {
                continue;
            }
            if (imap && imap != value) {
                throw NotImplementedException("Per component interpolation");
            }
            imap = value;
        }
        if (!imap) {
            continue;
        }
        program.info.interpolation[index] = [&] {
            switch (*imap) {
            case PixelImap::Unused:
            case PixelImap::Perspective:
                return Interpolation::Smooth;
            case PixelImap::Constant:
                return Interpolation::Flat;
            case PixelImap::ScreenLinear:
                return Interpolation::NoPerspective;
            }
            throw NotImplementedException("Unknown interpolation {}", *imap);
        }();
    }
}

void AddNVNStorageBuffers(IR::Program& program) {
    if (!program.info.uses_global_memory) {
        return;
    }
    const u32 driver_cbuf{0};
    const u32 descriptor_size{0x10};
    const u32 num_buffers{16};
    const u32 base{[&] {
        switch (program.stage) {
        case Stage::VertexA:
        case Stage::VertexB:
            return 0x110u;
        case Stage::TessellationControl:
            return 0x210u;
        case Stage::TessellationEval:
            return 0x310u;
        case Stage::Geometry:
            return 0x410u;
        case Stage::Fragment:
            return 0x510u;
        case Stage::Compute:
            return 0x310u;
        }
        throw InvalidArgument("Invalid stage {}", program.stage);
    }()};
    auto& descs{program.info.storage_buffers_descriptors};
    for (u32 index = 0; index < num_buffers; ++index) {
        if (!program.info.nvn_buffer_used[index]) {
            continue;
        }
        const u32 offset{base + index * descriptor_size};
        const auto it{std::ranges::find(descs, offset, &StorageBufferDescriptor::cbuf_offset)};
        if (it != descs.end()) {
            it->is_written |= program.info.stores_global_memory;
            continue;
        }
        descs.push_back({
            .cbuf_index = driver_cbuf,
            .cbuf_offset = offset,
            .count = 1,
            .is_written = program.info.stores_global_memory,
        });
    }
}

using IR::IsLegacyAttribute; //rescoped to attribute.h to make it visible in load_store_attribute.cpp IPA

std::map<IR::Attribute, IR::Attribute> GenerateLegacyToGenericMappings(
    const VaryingState& state, std::queue<IR::Attribute> unused_generics,
    const std::map<IR::Attribute, IR::Attribute>& previous_stage_mapping) {
    std::map<IR::Attribute, IR::Attribute> mapping;
    auto update_mapping = [&mapping, &unused_generics, previous_stage_mapping](IR::Attribute attr,
                                                                               size_t count) {
        if (previous_stage_mapping.find(attr) != previous_stage_mapping.end()) {
            for (size_t i = 0; i < count; ++i) {
                mapping.insert({attr + i, previous_stage_mapping.at(attr + i)});
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                mapping.insert({attr + i, unused_generics.front() + i});
            }
            unused_generics.pop();
        }
    };
    for (size_t index = 0; index < 4; ++index) {
        auto attr = IR::Attribute::ColorFrontDiffuseR + index * 4;
        if (state.AnyComponent(attr)) {
            update_mapping(attr, 4);
        }
    }
    if (state[IR::Attribute::FogCoordinate]) {
        update_mapping(IR::Attribute::FogCoordinate, 1);
    }
    for (size_t index = 0; index < IR::NUM_FIXEDFNCTEXTURE; ++index) {
        auto attr = IR::Attribute::FixedFncTexture0S + index * 4;
        if (state.AnyComponent(attr)) {
            update_mapping(attr, 4);
        }
    }
    return mapping;
}

void EmitGeometryPassthrough(IR::IREmitter& ir, const IR::Program& program,
                             const Shader::VaryingState& passthrough_mask,
                             bool passthrough_position,
                             std::optional<IR::Attribute> passthrough_layer_attr) {
    for (u32 i = 0; i < program.output_vertices; i++) {
        // Assign generics from input
        for (u32 j = 0; j < 32; j++) {
            if (!passthrough_mask.Generic(j)) {
                continue;
            }

            const IR::Attribute attr = IR::Attribute::Generic0X + (j * 4);
            for (u32 component = 0; component < 4; ++component) {
                if (passthrough_mask[attr + component]) {
                    ir.SetAttribute(attr + component,
                                    ir.GetAttribute(attr + component, ir.Imm32(i)), ir.Imm32(0));
                }
            }
        }

        if (passthrough_position) {
            const IR::Attribute attr = IR::Attribute::PositionX;
            for (u32 component = 0; component < 4; ++component) {
                if (!program.is_geometry_passthrough || passthrough_mask[attr + component]) {
                    ir.SetAttribute(attr + component,
                                    ir.GetAttribute(attr + component, ir.Imm32(i)), ir.Imm32(0));
                }
            }
        }

        if (passthrough_layer_attr) {
            // Assign layer
            ir.SetAttribute(IR::Attribute::Layer, ir.GetAttribute(*passthrough_layer_attr),
                            ir.Imm32(0));
        }

        // Emit vertex
        ir.EmitVertex(ir.Imm32(0));
    }
    ir.EndPrimitive(ir.Imm32(0));
}

u32 GetOutputTopologyVertices(OutputTopology output_topology) {
    switch (output_topology) {
    case OutputTopology::PointList:
        return 1;
    case OutputTopology::LineStrip:
        return 2;
    default:
        return 3;
    }
}

} // Anonymous namespace

void LowerGeometryPassthrough(IR::Program& program) {
    if (!program.is_geometry_passthrough || program.geometry_passthrough_lowered) {
        return;
    }
    program.output_vertices = GetOutputTopologyVertices(program.output_topology);
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Epilogue) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitGeometryPassthrough(
                    ir, program, program.info.passthrough,
                    program.info.passthrough.AnyComponent(IR::Attribute::PositionX), {});
            }
        }
    }
    program.geometry_passthrough_lowered = true;
}

IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
                             Environment& env, Flow::CFG& cfg, const HostTranslateInfo& host_info) {
    HostTranslateInfo normalized_host_info{host_info};
    normalized_host_info.ApplyDescriptorLimitPolicy();

    IR::Program program;
    program.syntax_list = BuildASL(inst_pool, block_pool, env, cfg, normalized_host_info);
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = PostOrder(program.syntax_list.front());
    program.stage = env.ShaderStage();
    program.local_memory_size = env.LocalMemorySize();
    switch (program.stage) {
    case Stage::TessellationControl: {
        const ProgramHeader& sph{env.SPH()};
        program.invocations = sph.common2.threads_per_input_primitive;
        break;
    }
    case Stage::Geometry: {
        const ProgramHeader& sph{env.SPH()};
        program.output_topology = sph.common3.output_topology;
        program.output_vertices = sph.common4.max_output_vertices;
        program.invocations = sph.common2.threads_per_input_primitive;
        program.is_geometry_passthrough = sph.common0.geometry_passthrough != 0;
        if (program.is_geometry_passthrough) {
            const auto& mask{env.GpPassthroughMask()};
            for (size_t i = 0; i < mask.size() * 32; ++i) {
                program.info.passthrough.mask[i] = ((mask[i / 32] >> (i % 32)) & 1) == 0;
            }

            if (!normalized_host_info.support_geometry_shader_passthrough) {
                LowerGeometryPassthrough(program);
            }
        }
        break;
    }
    case Stage::Compute:
        program.workgroup_size = env.WorkgroupSize();
        program.shared_memory_size = env.SharedMemorySize();
        break;
    default:
        break;
    }
    RemoveUnreachableBlocks(program);

    // Replace instructions before the SSA rewrite
    if (!normalized_host_info.support_float64) {
        Optimization::LowerFp64ToFp32(program);
    }
    if (!normalized_host_info.support_float16) {
        Optimization::LowerFp16ToFp32(program);
    }
    if (!normalized_host_info.support_int64) {
        Optimization::LowerInt64ToInt32(program);
    }
    if (!normalized_host_info.support_conditional_barrier) {
        Optimization::ConditionalBarrierPass(program);
    }
    Optimization::SsaRewritePass(program);

    Optimization::ConstantPropagationPass(env, program);
    Optimization::QuadShufflePass(env, program);

    Optimization::PositionPass(env, program);

    Optimization::GlobalMemoryToStorageBufferPass(program, normalized_host_info);
    Optimization::TexturePass(env, program, normalized_host_info);

    if (Settings::values.resolution_info.active || Settings::values.rescale_hack.GetValue()) {
        Optimization::RescalingPass(program);
    }
    Optimization::DeadCodeEliminationPass(program);
    if (Settings::values.renderer_debug) {
        Optimization::VerificationPass(program);
    }
    Optimization::CollectShaderInfoPass(env, program);
    Optimization::LayerPass(program, normalized_host_info);
    Optimization::VendorWorkaroundPass(program);

    CollectInterpolationInfo(env, program);
    AddNVNStorageBuffers(program);
    return program;
}

IR::Program MergeDualVertexPrograms(IR::Program& vertex_a, IR::Program& vertex_b,
                                    Environment& env_vertex_b) {
    IR::Program result{};
    Optimization::VertexATransformPass(vertex_a);
    Optimization::VertexBTransformPass(vertex_b);
    for (const auto& term : vertex_a.syntax_list) {
        if (term.type != IR::AbstractSyntaxNode::Type::Return) {
            result.syntax_list.push_back(term);
        }
    }
    result.syntax_list.insert(result.syntax_list.end(), vertex_b.syntax_list.begin(),
                              vertex_b.syntax_list.end());
    result.blocks = GenerateBlocks(result.syntax_list);
    result.post_order_blocks = vertex_b.post_order_blocks;
    for (const auto& block : vertex_a.post_order_blocks) {
        result.post_order_blocks.push_back(block);
    }
    result.stage = Stage::VertexB;
    result.info = vertex_a.info;
    result.local_memory_size = (std::max)(vertex_a.local_memory_size, vertex_b.local_memory_size);
    result.info.loads.mask |= vertex_b.info.loads.mask;
    result.info.stores.mask |= vertex_b.info.stores.mask;

    Optimization::JoinTextureInfo(result.info, vertex_b.info);
    Optimization::JoinStorageInfo(result.info, vertex_b.info);
    Optimization::DeadCodeEliminationPass(result);
    if (Settings::values.renderer_debug) {
        Optimization::VerificationPass(result);
    }
    Optimization::CollectShaderInfoPass(env_vertex_b, result);
    return result;
}

void ConvertLegacyToGeneric(IR::Program& program, const Shader::RuntimeInfo& runtime_info) {
    auto& stores = program.info.stores;
    if (stores.Legacy()) {
        std::queue<IR::Attribute> unused_output_generics{};
        for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
            if (!stores.Generic(index)) {
                unused_output_generics.push(IR::Attribute::Generic0X + index * 4);
            }
        }
        program.info.legacy_stores_mapping =
            GenerateLegacyToGenericMappings(stores, unused_output_generics, {});
        for (IR::Block* const block : program.post_order_blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::SetAttribute: {
                    const auto attr = inst.Arg(0).Attribute();
                    if (IsLegacyAttribute(attr)) {
                        stores.Set(program.info.legacy_stores_mapping[attr], true);
                        inst.SetArg(0, Shader::IR::Value(program.info.legacy_stores_mapping[attr]));
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    auto& loads = program.info.loads;
    if (loads.Legacy()) {
        std::queue<IR::Attribute> unused_input_generics{};
        for (size_t index = 0; index < IR::NUM_GENERICS; ++index) {
            const AttributeType input_type{runtime_info.generic_input_types[index]};
            if (!runtime_info.previous_stage_stores.Generic(index) || !loads.Generic(index) ||
                input_type == AttributeType::Disabled) {
                unused_input_generics.push(IR::Attribute::Generic0X + index * 4);
            }
        }
        auto mappings = GenerateLegacyToGenericMappings(
            loads, unused_input_generics, runtime_info.previous_stage_legacy_stores_mapping);
        for (IR::Block* const block : program.post_order_blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::GetAttribute: {
                    const auto attr = inst.Arg(0).Attribute();
                    if (IsLegacyAttribute(attr)) {
                        loads.Set(mappings[attr], true);
                        inst.SetArg(0, Shader::IR::Value(mappings[attr]));
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
}

IR::Program GenerateGeometryPassthrough(ObjectPool<IR::Inst>& inst_pool,
                                        ObjectPool<IR::Block>& block_pool,
                                        const HostTranslateInfo& host_info,
                                        IR::Program& source_program,
                                        Shader::OutputTopology output_topology) {
    IR::Program program;
    program.stage = Stage::Geometry;
    program.output_topology = output_topology;
    program.output_vertices = GetOutputTopologyVertices(output_topology);

    program.is_geometry_passthrough = false;
    program.info.loads.mask = source_program.info.stores.mask;
    program.info.stores.mask = source_program.info.stores.mask;
    program.info.stores.Set(IR::Attribute::Layer, true);
    program.info.stores.Set(source_program.info.emulated_layer, false);

    IR::Block* current_block = block_pool.Create(inst_pool);
    auto& node{program.syntax_list.emplace_back()};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = current_block;

    IR::IREmitter ir{*current_block};
    EmitGeometryPassthrough(ir, program, program.info.stores, true,
                            source_program.info.emulated_layer);

    IR::Block* return_block{block_pool.Create(inst_pool)};
    IR::IREmitter{*return_block}.Epilogue();
    current_block->AddBranch(return_block);

    auto& merge{program.syntax_list.emplace_back()};
    merge.type = IR::AbstractSyntaxNode::Type::Block;
    merge.data.block = return_block;
    program.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;

    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = PostOrder(program.syntax_list.front());
    Optimization::SsaRewritePass(program);

    return program;
}

namespace {
// Generics and position the vertex program writes, minus an attribute that is not to be forwarded.
std::vector<IR::Attribute> ForwardedAttributes(const IR::Program& vertex_program,
                                               std::optional<IR::Attribute> except) {
    std::vector<IR::Attribute> attributes;
    for (u32 component = 0; component < 4; ++component) {
        attributes.push_back(IR::Attribute::PositionX + component);
    }
    for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
        for (u32 component = 0; component < 4; ++component) {
            const IR::Attribute attr{IR::Attribute::Generic0X + index * 4 + component};
            if (vertex_program.info.stores[attr] && attr != except) {
                attributes.push_back(attr);
            }
        }
    }
    return attributes;
}

template <typename Emit>
void BuildGeneratedProgram(IR::Program& program, ObjectPool<IR::Inst>& inst_pool,
                           ObjectPool<IR::Block>& block_pool, Emit&& emit) {
    IR::Block* current_block = block_pool.Create(inst_pool);
    auto& node{program.syntax_list.emplace_back()};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = current_block;

    IR::IREmitter ir{*current_block};
    // Backends set up per-invocation state here (tessellation emulation, local memory lanes)
    ir.Prologue();
    emit(ir);

    IR::Block* return_block{block_pool.Create(inst_pool)};
    IR::IREmitter{*return_block}.Epilogue();
    current_block->AddBranch(return_block);

    auto& merge{program.syntax_list.emplace_back()};
    merge.type = IR::AbstractSyntaxNode::Type::Block;
    merge.data.block = return_block;
    program.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;

    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = PostOrder(program.syntax_list.front());
    Optimization::SsaRewritePass(program);
}
} // Anonymous namespace

IR::Program GeneratePointGeometry(ObjectPool<IR::Inst>& inst_pool,
                                  ObjectPool<IR::Block>& block_pool,
                                  const IR::Program& vertex_program,
                                  std::optional<IR::Attribute> size_attr,
                                  std::optional<IR::Attribute> coord_attr, f32 fixed_size) {
    IR::Program program;
    program.stage = Stage::Geometry;
    program.output_topology = OutputTopology::TriangleStrip;
    program.output_vertices = 4;
    program.invocations = 1;
    program.info.uses_stage_constants = true;
    // The fragment program resolves its legacy inputs against what the vertex program mapped them to
    program.info.legacy_stores_mapping = vertex_program.info.legacy_stores_mapping;

    const std::vector<IR::Attribute> forwarded{ForwardedAttributes(vertex_program, size_attr)};
    for (const IR::Attribute attr : forwarded) {
        program.info.loads.Set(attr, true);
        program.info.stores.Set(attr, true);
    }
    if (size_attr) {
        program.info.loads.Set(*size_attr, true);
    }
    if (coord_attr) {
        for (u32 component = 0; component < 4; ++component) {
            program.info.stores.Set(*coord_attr + component, true);
        }
    }

    BuildGeneratedProgram(program, inst_pool, block_pool, [&](IR::IREmitter& ir) {
        const IR::U32 first{ir.Imm32(0)};
        const IR::F32 w{ir.GetAttribute(IR::Attribute::PositionW, first)};
        const IR::F32 size{size_attr ? ir.GetAttribute(*size_attr, first) : ir.Imm32(fixed_size)};
        const IR::F32 half_x{ir.FPMul(ir.FPMul(size, w), ir.FPRecip(ir.StageConstant(4)))};
        const IR::F32 half_y{ir.FPMul(ir.FPMul(size, w), ir.FPRecip(ir.StageConstant(5)))};
        for (u32 vertex = 0; vertex < 4; ++vertex) {
            const bool right{(vertex & 1) != 0};
            const bool bottom{(vertex & 2) != 0};
            for (const IR::Attribute attr : forwarded) {
                IR::F32 value{ir.GetAttribute(attr, first)};
                if (attr == IR::Attribute::PositionX) {
                    value = ir.FPAdd(value, right ? half_x : IR::F32{ir.FPNeg(half_x)});
                } else if (attr == IR::Attribute::PositionY) {
                    value = ir.FPAdd(value, bottom ? half_y : IR::F32{ir.FPNeg(half_y)});
                }
                ir.SetAttribute(attr, value, first);
            }
            if (coord_attr) {
                ir.SetAttribute(*coord_attr + 0, ir.Imm32(right ? 1.0f : 0.0f), first);
                ir.SetAttribute(*coord_attr + 1, ir.Imm32(bottom ? 1.0f : 0.0f), first);
                ir.SetAttribute(*coord_attr + 2, ir.Imm32(0.0f), first);
                ir.SetAttribute(*coord_attr + 3, ir.Imm32(1.0f), first);
            }
            ir.EmitVertex(first);
        }
        ir.EndPrimitive(first);
    });
    return program;
}

namespace {
IR::F32 Subtract(IR::IREmitter& ir, const IR::F32& lhs, const IR::F32& rhs) {
    return ir.FPAdd(lhs, ir.FPNeg(rhs));
}

/// A geometry program forwarding forwarded, with room for vertices.
IR::Program MakeForwardingGeometry(const IR::Program& vertex_program,
                                   const std::vector<IR::Attribute>& forwarded, u32 vertices) {
    IR::Program program;
    program.stage = Stage::Geometry;
    program.output_topology = OutputTopology::TriangleStrip;
    program.output_vertices = vertices;
    program.invocations = 1;
    program.info.uses_stage_constants = true;
    program.info.legacy_stores_mapping = vertex_program.info.legacy_stores_mapping;
    for (const IR::Attribute attr : forwarded) {
        program.info.loads.Set(attr, true);
        program.info.stores.Set(attr, true);
    }
    return program;
}
} // Anonymous namespace

IR::Program GenerateLineGeometry(ObjectPool<IR::Inst>& inst_pool,
                                 ObjectPool<IR::Block>& block_pool,
                                 const IR::Program& vertex_program,
                                 std::optional<IR::Attribute> stipple_attr) {
    const std::vector<IR::Attribute> forwarded{ForwardedAttributes(vertex_program, std::nullopt)};
    IR::Program program{MakeForwardingGeometry(vertex_program, forwarded, 4)};
    if (stipple_attr) {
        for (u32 component = 0; component < 4; ++component) {
            program.info.stores.Set(*stipple_attr + component, true);
        }
    }

    BuildGeneratedProgram(program, inst_pool, block_pool, [&](IR::IREmitter& ir) {
        const IR::U32 stream{ir.Imm32(0)};
        const IR::U32 v0{ir.Imm32(0)};
        const IR::U32 v1{ir.Imm32(1)};
        const IR::F32 w0{ir.GetAttribute(IR::Attribute::PositionW, v0)};
        const IR::F32 w1{ir.GetAttribute(IR::Attribute::PositionW, v1)};
        const IR::F32 viewport_x{ir.StageConstant(4)};
        const IR::F32 viewport_y{ir.StageConstant(5)};
        const auto ndc{[&](IR::Attribute attr, const IR::U32& vertex, const IR::F32& w) {
            return IR::F32{ir.FPMul(ir.GetAttribute(attr, vertex), ir.FPRecip(w))};
        }};
        // In pixels, which keeps the width the same whichever way the line runs.
        const IR::F32 half{ir.Imm32(0.5f)};
        const IR::F32 dx{ir.FPMul(ir.FPMul(Subtract(ir, ndc(IR::Attribute::PositionX, v1, w1),
                                                    ndc(IR::Attribute::PositionX, v0, w0)),
                                           viewport_x),
                                  half)};
        const IR::F32 dy{ir.FPMul(ir.FPMul(Subtract(ir, ndc(IR::Attribute::PositionY, v1, w1),
                                                    ndc(IR::Attribute::PositionY, v0, w0)),
                                           viewport_y),
                                  half)};
        const IR::F32 length{
            ir.FPSqrt(ir.FPMax(ir.FPAdd(ir.FPMul(dx, dx), ir.FPMul(dy, dy)), ir.Imm32(1e-8f)))};
        // A width in pixels is width * 2 / viewport in NDC for the whole quad, so width / viewport
        // to each side.
        const IR::F32 width_over_length{ir.FPMul(ir.StageConstant(0), ir.FPRecip(length))};
        const IR::F32 offset_x{
            ir.FPMul(ir.FPMul(ir.FPNeg(dy), width_over_length), ir.FPRecip(viewport_x))};
        const IR::F32 offset_y{ir.FPMul(ir.FPMul(dx, width_over_length), ir.FPRecip(viewport_y))};
        for (u32 corner = 0; corner < 4; ++corner) {
            const IR::U32 vertex{ir.Imm32((corner & 1) != 0 ? 1 : 0)};
            const IR::F32 w{(corner & 1) != 0 ? w1 : w0};
            const bool negative{(corner & 2) != 0};
            for (const IR::Attribute attr : forwarded) {
                IR::F32 value{ir.GetAttribute(attr, vertex)};
                if (attr == IR::Attribute::PositionX || attr == IR::Attribute::PositionY) {
                    const IR::F32 axis{attr == IR::Attribute::PositionX ? offset_x : offset_y};
                    const IR::F32 offset{ir.FPMul(axis, w)};
                    value = ir.FPAdd(value, negative ? IR::F32{ir.FPNeg(offset)} : offset);
                }
                ir.SetAttribute(attr, value, stream);
            }
            if (stipple_attr) {
                ir.SetAttribute(*stipple_attr + 0, (corner & 1) != 0 ? length : ir.Imm32(0.0f),
                                stream);
                for (u32 component = 1; component < 4; ++component) {
                    ir.SetAttribute(*stipple_attr + component, ir.Imm32(0.0f), stream);
                }
            }
            ir.EmitVertex(stream);
        }
        ir.EndPrimitive(stream);
    });
    return program;
}

IR::Program GeneratePolygonPointGeometry(ObjectPool<IR::Inst>& inst_pool,
                                         ObjectPool<IR::Block>& block_pool,
                                         const IR::Program& vertex_program, f32 fixed_size) {
    const std::vector<IR::Attribute> forwarded{ForwardedAttributes(vertex_program, std::nullopt)};
    IR::Program program{MakeForwardingGeometry(vertex_program, forwarded, 12)};

    BuildGeneratedProgram(program, inst_pool, block_pool, [&](IR::IREmitter& ir) {
        const IR::U32 stream{ir.Imm32(0)};
        for (u32 index = 0; index < 3; ++index) {
            const IR::U32 vertex{ir.Imm32(index)};
            const IR::F32 w{ir.GetAttribute(IR::Attribute::PositionW, vertex)};
            const IR::F32 size{ir.FPMul(ir.Imm32(fixed_size), w)};
            const IR::F32 half_x{ir.FPMul(size, ir.FPRecip(ir.StageConstant(4)))};
            const IR::F32 half_y{ir.FPMul(size, ir.FPRecip(ir.StageConstant(5)))};
            for (u32 corner = 0; corner < 4; ++corner) {
                for (const IR::Attribute attr : forwarded) {
                    IR::F32 value{ir.GetAttribute(attr, vertex)};
                    if (attr == IR::Attribute::PositionX) {
                        value = ir.FPAdd(value, (corner & 1) != 0 ? half_x
                                                                  : IR::F32{ir.FPNeg(half_x)});
                    } else if (attr == IR::Attribute::PositionY) {
                        value = ir.FPAdd(value, (corner & 2) != 0 ? half_y
                                                                  : IR::F32{ir.FPNeg(half_y)});
                    }
                    ir.SetAttribute(attr, value, stream);
                }
                ir.EmitVertex(stream);
            }
            ir.EndPrimitive(stream);
        }
    });
    return program;
}

IR::Program GenerateFillRectangleGeometry(ObjectPool<IR::Inst>& inst_pool,
                                          ObjectPool<IR::Block>& block_pool,
                                          const IR::Program& vertex_program,
                                          const IR::Program& fragment_program, u32 flat_vertex) {
    const std::vector<IR::Attribute> forwarded{ForwardedAttributes(vertex_program, std::nullopt)};
    IR::Program program{MakeForwardingGeometry(vertex_program, forwarded, 4)};
    const auto is_flat{[&](IR::Attribute attr) {
        return IR::IsGeneric(attr) &&
               fragment_program.info.interpolation[IR::GenericAttributeIndex(attr)] ==
                   Interpolation::Flat;
    }};

    BuildGeneratedProgram(program, inst_pool, block_pool, [&](IR::IREmitter& ir) {
        const IR::U32 stream{ir.Imm32(0)};
        std::array<IR::F32, 3> x;
        std::array<IR::F32, 3> y;
        for (u32 index = 0; index < 3; ++index) {
            const IR::U32 vertex{ir.Imm32(index)};
            const IR::F32 inverse_w{ir.FPRecip(ir.GetAttribute(IR::Attribute::PositionW, vertex))};
            x[index] = ir.FPMul(ir.GetAttribute(IR::Attribute::PositionX, vertex), inverse_w);
            y[index] = ir.FPMul(ir.GetAttribute(IR::Attribute::PositionY, vertex), inverse_w);
        }
        const IR::F32 min_x{ir.FPMin(ir.FPMin(x[0], x[1]), x[2])};
        const IR::F32 max_x{ir.FPMax(ir.FPMax(x[0], x[1]), x[2])};
        const IR::F32 min_y{ir.FPMin(ir.FPMin(y[0], y[1]), y[2])};
        const IR::F32 max_y{ir.FPMax(ir.FPMax(y[0], y[1]), y[2])};
        const IR::F32 y1_y2{Subtract(ir, y[1], y[2])};
        const IR::F32 x2_x1{Subtract(ir, x[2], x[1])};
        const IR::F32 y2_y0{Subtract(ir, y[2], y[0])};
        const IR::F32 x0_x2{Subtract(ir, x[0], x[2])};
        const IR::F32 inverse_area{ir.FPRecip(
            ir.FPAdd(ir.FPMul(y1_y2, x0_x2), ir.FPMul(x2_x1, Subtract(ir, y[0], y[2]))))};
        for (u32 corner = 0; corner < 4; ++corner) {
            const IR::F32 corner_x{(corner & 1) != 0 ? max_x : min_x};
            const IR::F32 corner_y{(corner & 2) != 0 ? max_y : min_y};
            const IR::F32 to_x{Subtract(ir, corner_x, x[2])};
            const IR::F32 to_y{Subtract(ir, corner_y, y[2])};
            const IR::F32 b0{
                ir.FPMul(ir.FPAdd(ir.FPMul(y1_y2, to_x), ir.FPMul(x2_x1, to_y)), inverse_area)};
            const IR::F32 b1{
                ir.FPMul(ir.FPAdd(ir.FPMul(y2_y0, to_x), ir.FPMul(x0_x2, to_y)), inverse_area)};
            const IR::F32 b2{Subtract(ir, Subtract(ir, ir.Imm32(1.0f), b0), b1)};
            const auto interpolate{[&](IR::Attribute attr) {
                const IR::F32 a{ir.FPMul(ir.GetAttribute(attr, ir.Imm32(0)), b0)};
                const IR::F32 b{ir.FPMul(ir.GetAttribute(attr, ir.Imm32(1)), b1)};
                const IR::F32 c{ir.FPMul(ir.GetAttribute(attr, ir.Imm32(2)), b2)};
                return IR::F32{ir.FPAdd(ir.FPAdd(a, b), c)};
            }};
            const IR::F32 w{interpolate(IR::Attribute::PositionW)};
            for (const IR::Attribute attr : forwarded) {
                IR::F32 value;
                if (attr == IR::Attribute::PositionX) {
                    value = ir.FPMul(corner_x, w);
                } else if (attr == IR::Attribute::PositionY) {
                    value = ir.FPMul(corner_y, w);
                } else if (is_flat(attr)) {
                    value = ir.GetAttribute(attr, ir.Imm32(flat_vertex));
                } else {
                    value = interpolate(attr);
                }
                ir.SetAttribute(attr, value, stream);
            }
            ir.EmitVertex(stream);
        }
        ir.EndPrimitive(stream);
    });
    return program;
}

IR::Program GenerateProvokingLastGeometry(ObjectPool<IR::Inst>& inst_pool,
                                          ObjectPool<IR::Block>& block_pool,
                                          const IR::Program& vertex_program, u32 input_vertices) {
    const std::vector<IR::Attribute> forwarded{ForwardedAttributes(vertex_program, std::nullopt)};
    IR::Program program{MakeForwardingGeometry(vertex_program, forwarded, input_vertices)};
    program.info.uses_stage_constants = false;
    program.output_topology =
        input_vertices == 2 ? OutputTopology::LineStrip : OutputTopology::TriangleStrip;

    BuildGeneratedProgram(program, inst_pool, block_pool, [&](IR::IREmitter& ir) {
        const IR::U32 stream{ir.Imm32(0)};
        for (u32 index = 0; index < input_vertices; ++index) {
            const IR::U32 vertex{ir.Imm32((index + input_vertices - 1) % input_vertices)};
            for (const IR::Attribute attr : forwarded) {
                ir.SetAttribute(attr, ir.GetAttribute(attr, vertex), stream);
            }
            ir.EmitVertex(stream);
        }
        ir.EndPrimitive(stream);
    });
    return program;
}

IR::Program GenerateTessellationControl(ObjectPool<IR::Inst>& inst_pool,
                                        ObjectPool<IR::Block>& block_pool,
                                        const IR::Program& vertex_program,
                                        const IR::Program& eval_program, u32 patch_vertices) {
    IR::Program program;
    program.stage = Stage::TessellationControl;
    program.invocations = patch_vertices;
    program.info.uses_invocation_id = true;
    program.info.uses_stage_constants = true;
    program.info.stores_tess_level_outer = true;
    program.info.stores_tess_level_inner = true;
    program.info.legacy_stores_mapping = vertex_program.info.legacy_stores_mapping;

    // Only what the next stage reads: D3D12 rejects a hull shader with outputs for a domain shader
    // without inputs.
    std::vector<IR::Attribute> forwarded{ForwardedAttributes(vertex_program, std::nullopt)};
    if (!eval_program.info.loads_indexed_attributes) {
        std::erase_if(forwarded, [&](IR::Attribute attr) { return !eval_program.info.loads[attr]; });
    }
    for (const IR::Attribute attr : forwarded) {
        program.info.loads.Set(attr, true);
        program.info.stores.Set(attr, true);
    }

    BuildGeneratedProgram(program, inst_pool, block_pool, [&](IR::IREmitter& ir) {
        const IR::U32 invocation{ir.InvocationId()};
        for (const IR::Attribute attr : forwarded) {
            ir.SetAttribute(attr, ir.GetAttribute(attr, invocation), invocation);
        }
        ir.SetPatch(IR::Patch::TessellationLodLeft, ir.StageConstant(0));
        ir.SetPatch(IR::Patch::TessellationLodRight, ir.StageConstant(1));
        ir.SetPatch(IR::Patch::TessellationLodTop, ir.StageConstant(2));
        ir.SetPatch(IR::Patch::TessellationLodBottom, ir.StageConstant(3));
        ir.SetPatch(IR::Patch::TessellationLodInteriorU, ir.StageConstant(4));
        ir.SetPatch(IR::Patch::TessellationLodInteriorV, ir.StageConstant(5));
    });
    return program;
}

} // namespace Shader::Maxwell
