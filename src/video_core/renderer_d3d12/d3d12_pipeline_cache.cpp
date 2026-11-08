// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/container/static_vector.hpp>

#include "common/bug_tracker.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/gpu_thread.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/demote_return_pass.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/ir_opt/workgroup_split_pass.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/tessellation_emulation.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/frame_trace.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_async_pipelines.h"
#include "video_core/renderer_d3d12/d3d12_local_memory_scratch.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_memory_reserve.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_fold.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"
#include "video_core/renderer_d3d12/d3d12_pso_blob_cache.h"
#include "video_core/renderer_d3d12/d3d12_dxil_disk_cache.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_stream_output.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_helper.h"
#include "video_core/renderer_d3d12/d3d12_render_corrections.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/shader_environment.h"
#include "video_core/transform_feedback.h"
#include "video_core/shader_notify.h"

namespace D3D12 {

namespace {

std::array<std::atomic<u64>, 4> dumped_shaders{};
std::array<std::atomic<u64>, 4> skipped_shaders{};
std::atomic_bool any_skipped_shader{};
std::array<std::atomic<u64>, 4> gs_dropped_shaders{};

void AddShaderHash(std::array<std::atomic<u64>, 4>& slots, u64 unique_hash) {
    for (auto& slot : slots) {
        u64 expected = 0;
        if (slot.compare_exchange_strong(expected, unique_hash, std::memory_order_relaxed) ||
            expected == unique_hash) {
            return;
        }
    }
}

bool HasShaderHash(const std::array<std::atomic<u64>, 4>& slots, u64 unique_hash) {
    if (unique_hash == 0) {
        return false;
    }
    for (const auto& slot : slots) {
        if (slot.load(std::memory_order_relaxed) == unique_hash) {
            return true;
        }
    }
    return false;
}
std::atomic_bool pipeline_eviction{true};
std::atomic_bool pipeline_prewarm{true};
// EXPERIMENTAL default: 16 is only measured in one game so far, where it took a persistent compute
// dispatch from ~82 ms to ~4.5 ms per frame on Series. If another game shows broken compute
// results (particles, physics, culling) or hangs, try boot.cfg "cs_split=1" first.
std::atomic<u32> compute_workgroup_split{16};

/// Launches with more groups than this fill the GPU without splitting them further.
constexpr u64 MAX_SPLIT_GROUPS = 256;

/// The WorkgroupSplitPass factor for the current launch, or 1 to run it as written. Indirect
/// launches keep their group count on the GPU, where it is not multiplied.
u32 WorkgroupSplitFor(const Tegra::Engines::KeplerCompute& kepler_compute) {
    const u32 factor = compute_workgroup_split.load(std::memory_order_relaxed);
    if (factor <= 1 || kepler_compute.GetIndirectComputeAddress()) {
        return 1;
    }
    const auto& qmd = kepler_compute.launch_description;
    const u64 groups_x = qmd.grid_dim_x;
    const u64 groups = groups_x * qmd.grid_dim_y * qmd.grid_dim_z;
    if (groups == 0 || groups > MAX_SPLIT_GROUPS ||
        groups_x * factor > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
        return 1;
    }
    return factor;
}
std::atomic_bool transform_feedback_storage{false};

/// Writes a translated stage's IR and SPIR-V next to the log (SetDumpedShader).
void DumpShader(u64 unique_hash, size_t stage, const Shader::IR::Program& program,
                std::span<const u32> spirv) {
    const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const std::string base = fmt::format("shader_{:016x}_{}", unique_hash, stage);
    std::ofstream{directory / (base + ".ir.txt"), std::ios::trunc} << Shader::IR::DumpProgram(program);
    std::ofstream{directory / (base + ".spv"), std::ios::binary | std::ios::trunc}.write(
        reinterpret_cast<const char*>(spirv.data()),
        static_cast<std::streamsize>(spirv.size_bytes()));
    LOG_INFO(Render, "D3D12: shader {:016x} stage {} dumped ({})", unique_hash, stage, base);
}

/// Include both stages and the pipeline key: one VS can link to several fragment shaders.
void DumpDxil(u64 vertex_shader, u64 fragment_shader, u64 pipeline,
              std::span<const std::vector<u8>> stages) {
    const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    for (size_t stage = 0; stage < stages.size(); ++stage) {
        if (stages[stage].empty()) {
            continue;
        }
        const std::string name = fmt::format("dxil_{:016x}_{:016x}_{:016x}_{}.dxil",
                                             vertex_shader, fragment_shader, pipeline, stage);
        std::ofstream{directory / name, std::ios::binary | std::ios::trunc}.write(
            reinterpret_cast<const char*>(stages[stage].data()),
            static_cast<std::streamsize>(stages[stage].size()));
    }
    LOG_INFO(Render, "D3D12: pipeline DXIL VS {:016x} PS {:016x} key {:016x} dumped",
             vertex_shader, fragment_shader, pipeline);
}

using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;
using Vulkan::FixedPipelineState;

constexpr u32 CACHE_VERSION = 1;

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

// MaxwellToOutputTopology, MaxwellToCompareFunction, CastAttributeType and MakeRuntimeInfo follow
// vk_pipeline_cache.cpp. D3D12 has no native OpenGL NDC, so the depth conversion always happens in
// the shader. Transform feedback goes out through stream output, declared by spirv_to_dxil from the
// Xfb decorations of the last pre-rasterization stage (CreateGraphicsPipeline sets it up).

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    return Shader::CompareFunction::Always;
}

void ApplyVertexFetchFixups(Shader::IR::Program& program, const GraphicsPipelineCacheKey& key) {
    using Shader::Optimization::VertexFetchFixup;
    std::array<VertexFetchFixup, 32> fixups{};
    bool any = false;
    for (size_t index = 0; index < key.state.attributes.size(); ++index) {
        const auto& attr = key.state.attributes[index];
        if (attr.enabled == 0) {
            continue;
        }
        VertexFetchFixup& fixup = fixups[index];
        fixup.swap_red_blue =
            attr.bgra != 0 &&
            MaxwellToD3D12::FetchesBgraInShader(attr.Type(), attr.Size(), attr.offset,
                                                key.state.vertex_strides[attr.buffer]);
        const u32 components = MaxwellToD3D12::Vertex32BitComponents(attr.Size());
        if (components != 0 && attr.Type() == Maxwell::VertexAttribute::Type::UNorm) {
            fixup.normalize = VertexFetchFixup::Normalize::Unsigned;
        } else if (components != 0 && attr.Type() == Maxwell::VertexAttribute::Type::SNorm) {
            fixup.normalize = VertexFetchFixup::Normalize::Signed;
        }
        fixup.components = static_cast<u8>(components);
        any |= fixup.swap_red_blue || fixup.normalize != VertexFetchFixup::Normalize::None;
    }
    if (any) {
        Shader::Optimization::VertexFetchPass(program, fixups);
    }
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    if (attr.Type() == Maxwell::VertexAttribute::Type::SNorm &&
        attr.Size() == Maxwell::VertexAttribute::Size::Size_A2_B10_G10_R10) {
        // DXGI has no signed 10:10:10:2: fetched as a packed R32_UINT (VertexFormat) and normalized
        // in the shader. Game C packs normals like this; read as floats they were NaN.
        return Shader::AttributeType::SignedNormA2B10G10R10;
    }
    if (MaxwellToD3D12::Vertex32BitComponents(attr.Size()) != 0) {
        // Fetched as integers and normalized by VertexFetchPass.
        if (attr.Type() == Maxwell::VertexAttribute::Type::UNorm) {
            return Shader::AttributeType::UnsignedInt;
        }
        if (attr.Type() == Maxwell::VertexAttribute::Type::SNorm) {
            return Shader::AttributeType::SignedInt;
        }
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

/// How points wider than a pixel, or with coordinates inside them, are drawn: D3D12 points are one
/// pixel, so a generated geometry shader makes quads of them. Decides the generics the size and the
/// coordinates travel in, and rewrites the guest programs to use them.
struct PointGeometry {
    bool enabled{};
    std::optional<Shader::IR::Attribute> size_attr;
    std::optional<Shader::IR::Attribute> coord_attr;
};

/// Primitive state D3D12 lacks, drawn by a generated geometry shader (PlanPrimitiveGeometry).
struct PrimitiveGeometry {
    enum class Kind { None, Lines, PolygonPoints, FillRectangle, ProvokingLast };
    Kind kind{Kind::None};
    /// Generic with the pixels from the line's start, for the fragment program's stipple.
    std::optional<u32> stipple_generic;
};

PointGeometry PlanPointGeometry(std::span<Shader::IR::Program> programs,
                                const GraphicsPipelineCacheKey& key, bool excluded) {
    using Shader::IR::Attribute;
    PointGeometry plan;
    if (excluded || key.state.topology != Maxwell::PrimitiveTopology::Points ||
        key.unique_hashes[1] == 0 || key.unique_hashes[2] != 0 || key.unique_hashes[3] != 0 ||
        key.unique_hashes[4] != 0) {
        return plan;
    }
    Shader::Info& vertex = programs[1].info;
    Shader::Info& fragment = programs[5].info;
    const bool reads_coord =
        fragment.loads[Attribute::PointSpriteS] || fragment.loads[Attribute::PointSpriteT];
    const bool writes_size = vertex.stores[Attribute::PointSize];
    const float fixed_size = std::bit_cast<float>(key.state.point_size);
    if (!reads_coord && !writes_size && fixed_size == 1.0f) {
        return plan;
    }
    // A generated stage forwards position and generics only, so a point sent to another layer or
    // viewport stays the host's.
    if (vertex.stores[Attribute::Layer] || vertex.stores[Attribute::ViewportIndex]) {
        return plan;
    }
    std::vector<Attribute> free_generics;
    for (u32 index = Shader::IR::NUM_GENERICS; index-- > 0;) {
        if (!vertex.stores.Generic(index) && !fragment.loads.Generic(index)) {
            free_generics.push_back(Attribute::Generic0X + index * 4);
        }
    }
    if (free_generics.size() < static_cast<size_t>(writes_size) + (reads_coord ? 1 : 0)) {
        return plan;
    }
    plan.enabled = true;
    if (writes_size) {
        plan.size_attr = free_generics.back();
        free_generics.pop_back();
        vertex.stores.Set(Attribute::PointSize, false);
        vertex.stores.Set(*plan.size_attr, true);
        for (Shader::IR::Block* const block : programs[1].post_order_blocks) {
            for (Shader::IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == Shader::IR::Opcode::SetAttribute &&
                    inst.Arg(0).Attribute() == Attribute::PointSize) {
                    inst.SetArg(0, Shader::IR::Value(*plan.size_attr));
                }
            }
        }
    }
    if (reads_coord) {
        plan.coord_attr = free_generics.back();
        fragment.loads.Set(Attribute::PointSpriteS, false);
        fragment.loads.Set(Attribute::PointSpriteT, false);
        for (Shader::IR::Block* const block : programs[5].post_order_blocks) {
            for (Shader::IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != Shader::IR::Opcode::GetAttribute) {
                    continue;
                }
                const Attribute attr = inst.Arg(0).Attribute();
                if (attr == Attribute::PointSpriteS || attr == Attribute::PointSpriteT) {
                    const Attribute generic =
                        *plan.coord_attr + (attr == Attribute::PointSpriteS ? 0 : 1);
                    fragment.loads.Set(generic, true);
                    inst.SetArg(0, Shader::IR::Value(generic));
                }
            }
        }
    }
    return plan;
}

/// The swizzle a shader has to apply, or 0: a negated y is the viewport's job (SetViewports).
u16 EmulatedViewportSwizzle(u16 swizzle) {
    constexpr u16 IDENTITY = 0 | 2 << 4 | 4 << 8 | 6 << 12;
    constexpr u16 Y_MASK = 7 << 4;
    swizzle &= 0x7777;
    if (((swizzle >> 4) & 7) == 3) {
        swizzle = static_cast<u16>((swizzle & ~Y_MASK) | 2 << 4);
    }
    return swizzle == IDENTITY ? 0 : swizzle;
}

PrimitiveGeometry PlanPrimitiveGeometry(std::span<Shader::IR::Program> programs,
                                        const GraphicsPipelineCacheKey& key, bool excluded) {
    using Shader::IR::Attribute;
    using Kind = PrimitiveGeometry::Kind;
    PrimitiveGeometry plan;
    if (excluded || key.unique_hashes[1] == 0 || key.unique_hashes[2] != 0 ||
        key.unique_hashes[3] != 0 || key.unique_hashes[4] != 0) {
        return plan;
    }
    Shader::Info& vertex = programs[1].info;
    Shader::Info& fragment = programs[5].info;
    // Generated stages forward position and generics only.
    if (vertex.stores[Attribute::Layer] || vertex.stores[Attribute::ViewportIndex]) {
        return plan;
    }
    for (u32 index = 0; index < 8; ++index) {
        if (vertex.stores[Attribute::ClipDistance0 + index]) {
            return plan;
        }
    }
    const auto& dynamic = key.state.dynamic_state;
    if (dynamic.generated_lines != 0) {
        plan.kind = Kind::Lines;
        if (dynamic.line_stipple_enable != 0) {
            for (u32 index = Shader::IR::NUM_GENERICS; index-- > 0;) {
                if (!vertex.stores.Generic(index) && !fragment.loads.Generic(index)) {
                    plan.stipple_generic = index;
                    fragment.loads.Set(Attribute::Generic0X + index * 4, true);
                    fragment.interpolation[index] = Shader::Interpolation::NoPerspective;
                    break;
                }
            }
        }
    } else if (dynamic.generated_polygon_points != 0) {
        plan.kind = Kind::PolygonPoints;
    } else if (dynamic.generated_fill_rectangle != 0) {
        plan.kind = Kind::FillRectangle;
    } else if (key.state.provoking_vertex_last != 0) {
        for (u32 index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
            if (fragment.interpolation[index] == Shader::Interpolation::Flat &&
                fragment.loads.Generic(index) && vertex.stores.Generic(index)) {
                plan.kind = Kind::ProvokingLast;
                break;
            }
        }
    }
    return plan;
}

/// Gives each attribute group (attribute / 4) that stores writes and loads reads a slot of an
/// emulated tessellation vertex (RuntimeInfo::tess_input_slots), position first, then the
/// generics in order. Legacy loads take every group stores writes. Returns the slots taken.
u32 AssignTessSlots(const Shader::VaryingState& stores, const Shader::VaryingState& loads,
                    std::array<u8, 128>& slots) {
    constexpr u32 POSITION_GROUP = static_cast<u32>(Shader::IR::Attribute::PositionX) / 4;
    constexpr u32 FIRST_GENERIC_GROUP = static_cast<u32>(Shader::IR::Attribute::Generic0X) / 4;
    const bool all = loads.Legacy();
    u32 count = 0;
    const auto assign = [&](u32 group) {
        const auto base = static_cast<Shader::IR::Attribute>(group * 4);
        if (stores.AnyComponent(base) && (all || loads.AnyComponent(base))) {
            slots[group] = static_cast<u8>(++count);
        }
    };
    assign(POSITION_GROUP);
    for (u32 index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
        assign(FIRST_GENERIC_GROUP + index);
    }
    return count;
}

/// The generic patches both the control shader writes and the evaluation shader reads get a slot
/// of the record. Returns the slots taken.
u32 AssignTessPatchSlots(const Shader::Info& control, const Shader::Info& eval,
                         std::array<u8, 30>& slots) {
    u32 count = 0;
    for (size_t index = 0; index < slots.size(); ++index) {
        if (control.uses_patches[index] && eval.uses_patches[index]) {
            slots[index] = static_cast<u8>(++count);
        }
    }
    return count;
}

/// The vertex layouts the stages of an emulated tessellation agree on, filled stage by stage.
struct TessLayout {
    TessEmulationPlan plan;
    std::array<u8, 128> input_slots{};
    std::array<u8, 128> output_slots{};
    std::array<u8, 30> patch_slots{};
    u32 input_vertex_slots{};
    u32 output_vertex_slots{};
    u32 record_patch_slots{};

    /// Called with each stage, after ConvertLegacyToGeneric, so the producers' stores are generics.
    void Apply(std::span<const Shader::IR::Program> programs, const GraphicsPipelineCacheKey& key,
               const Shader::IR::Program& program, bool generates_tess_control,
               bool has_any_geometry, Shader::RuntimeInfo& info) {
        namespace Tess = Shader::TessellationEmulation;
        const u32 control_points = key.state.patch_control_points_minus_one + 1;
        switch (program.stage) {
        case Shader::Stage::VertexB: {
            // The generated control shader, made later from this stage, passes on what the
            // evaluation shader reads.
            const Shader::Info& consumer = generates_tess_control ? programs[3].info
                                                                  : programs[2].info;
            input_vertex_slots = AssignTessSlots(program.info.stores, consumer.loads, input_slots);
            if (input_vertex_slots == 0) {
                // The capture keeps one slot, so its buffer and stride are never empty.
                input_slots[static_cast<u32>(Shader::IR::Attribute::PositionX) / 4] = 1;
                input_vertex_slots = 1;
            }
            // The tessellated vertices go through the viewport, not these, and the guest's
            // transform feedback captures the evaluation shader's.
            info.convert_depth_mode = false;
            info.xfb_varyings = {};
            info.xfb_repeats.clear();
            info.xfb_count = 0;
            info.xfb_storage = false;
            info.xfb_copy_outputs = false;
            info.tess_capture = true;
            info.tess_input_slots = input_slots;
            info.tess_input_vertex_slots = input_vertex_slots;
            plan.input_vertices = control_points;
            plan.input_vertex_bytes = input_vertex_slots * Tess::SLOT_WORDS * 4;
            break;
        }
        case Shader::Stage::TessellationControl: {
            const Shader::IR::Program& eval = programs[3];
            output_vertex_slots =
                AssignTessSlots(program.info.stores, eval.info.loads, output_slots);
            record_patch_slots = AssignTessPatchSlots(program.info, eval.info, patch_slots);
            info.tess_emulation = Shader::TessEmulation::ControlCompute;
            info.tess_input_slots = input_slots;
            info.tess_input_vertex_slots = input_vertex_slots;
            info.tess_input_vertices = control_points;
            plan.output_vertices = std::max(program.invocations, 1U);
            plan.patches_per_group = Tess::PatchesPerGroup(plan.output_vertices);
            plan.record_words =
                Tess::RecordWords(record_patch_slots, plan.output_vertices, output_vertex_slots);
            break;
        }
        case Shader::Stage::TessellationEval:
            info.tess_emulation = Shader::TessEmulation::EvalVertex;
            info.convert_depth_mode = key.state.ndc_minus_one_to_one != 0 && !has_any_geometry;
            plan.domain = static_cast<u32>(info.tess_primitive);
            switch (info.tess_spacing) {
            case Shader::TessSpacing::FractionalOdd:
                plan.partitioning = 0;
                break;
            case Shader::TessSpacing::FractionalEven:
                plan.partitioning = 1;
                break;
            case Shader::TessSpacing::Equal:
                plan.partitioning = 2;
                break;
            }
            plan.ccw = !info.tess_clockwise;
            break;
        default:
            return;
        }
        // Both tessellation stages read a record laid out the same way.
        info.tess_output_slots = output_slots;
        info.tess_patch_slots = patch_slots;
        info.tess_output_vertex_slots = output_vertex_slots;
        info.tess_record_patch_slots = record_patch_slots;
        info.tess_output_vertices = plan.output_vertices;
    }
};

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program,
                                    bool point_geometry) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{point_geometry || (key.unique_hashes[4] != 0 &&
                                               !programs[4].UsesNativeGeometryPassthrough())};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{std::bit_cast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            info.convert_depth_mode = gl_ndc;
        }
        std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                               &CastAttributeType);
        for (size_t index = 0; index < key.state.attributes.size(); ++index) {
            const auto& attr = key.state.attributes[index];
            if (attr.enabled != 0) {
                const MaxwellToD3D12::AttributeFetch fetch = MaxwellToD3D12::SplitAttributeFetch(
                    attr.Type(), attr.Size(), attr.offset, key.state.vertex_strides[attr.buffer]);
                info.generic_input_parts[index] = static_cast<u8>(fetch.parts);
                info.generic_input_part_components[index] =
                    static_cast<u8>(fetch.part_components);
                info.generic_input_components[index] =
                    static_cast<u8>(MaxwellToD3D12::WidenedAttributeComponents(attr.Size()));
            }
        }
        break;
    case Shader::Stage::TessellationControl:
        info.patch_vertices = key.state.patch_control_points_minus_one + 1;
        break;
    case Shader::Stage::TessellationEval:
        // The control stage's output patch.
        if (previous_program && previous_program->stage == Shader::Stage::TessellationControl) {
            info.patch_vertices = previous_program->invocations;
        }
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            switch (static_cast<Maxwell::Tessellation::DomainType>(
                key.state.tessellation_primitive.Value())) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&key] {
            switch (static_cast<Maxwell::Tessellation::Spacing>(
                key.state.tessellation_spacing.Value())) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = std::bit_cast<float>(key.state.alpha_test_ref);
        info.dual_source_blend = key.state.attachment0_dual_source_blend != 0;
        if (GetUnormHalfDownRounding()) {
            for (size_t index = 0; index < key.state.color_formats.size(); ++index) {
                if (IsUnorm8RenderTarget(key.state.color_formats[index]) &&
                    key.state.attachments[index].enable == 0) {
                    info.unorm8_output_mask |= static_cast<u8>(1U << index);
                }
            }
        }
        if (EmulatesLogicOp(key.state)) {
            info.logic_op_enable = true;
            info.logic_op = static_cast<u8>(key.state.dynamic_state.logic_op.Value());
            info.logic_op_bits = LogicOpChannelBits(key.state.color_formats[0]);
        }
        info.alpha_to_coverage_footprint = key.ShaderAlphaToCoverage();
        info.sample_mask_footprint = key.SampleMaskFootprint();
        info.blend_constant_factors = PlanBlendConstants(key.state).shader_factors;
        if ((key.shader_flags & GraphicsPipelineCacheKey::SHADER_DEPTH_BIAS) != 0) {
            // Maxwell's constant bias counts twice D3D12's units.
            info.depth_bias_in_shader = true;
            info.depth_bias_unit = DepthBiasUnit(key.state.depth_format.Value());
            info.depth_bias_constant = std::bit_cast<f32>(key.depth_bias) / 2.0f;
            info.depth_bias_slope = std::bit_cast<f32>(key.slope_scaled_depth_bias);
            info.depth_bias_clamp = std::bit_cast<f32>(key.depth_bias_clamp);
        }
        break;
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

/// Pipeline workers. Capped: each one holds a recompiler and a translator's worth of memory, and
/// the console's game partition has 5 GB in total. "pso_workers=" sets them for A/B runs.
size_t GetTotalPipelineWorkers() {
    if (const u32 configured = GetPipelineWorkers(); configured != 0) {
        return configured;
    }
    const size_t threads = std::max<size_t>(std::thread::hardware_concurrency(), 2) - 1;
    return std::min<size_t>(threads, 4);
}

/// Workers for the disk cache on the loading screen. Nothing else runs then but the loading
/// animation, so every logical CPU builds; at low priority, the animation still goes first. They
/// are gone once the game starts, which keeps the capped workers above. "pso_boot_workers=".
size_t GetTotalPipelineBootWorkers() {
    if (const u32 configured = GetPipelineBootWorkers(); configured != 0) {
        return configured;
    }
    return std::max<size_t>(std::thread::hardware_concurrency(), 2);
}

/// Shader capabilities from D3D12's feature queries (logged at boot by Device::LogCapabilities).
struct ShaderCaps {
    bool wave_ops{};
    u32 wave_lane_count_max{};
    bool int64{};
    bool float64{};
    bool vp_and_rt_index_from_any_stage{};
    bool typed_uav_load_additional_formats{};
};

ShaderCaps QueryShaderCaps(const Device& device) {
    ShaderCaps caps;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device.Get()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                    sizeof(options)))) {
        caps.float64 = options.DoublePrecisionFloatShaderOps != FALSE;
        caps.vp_and_rt_index_from_any_stage =
            options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation !=
            FALSE;
        caps.typed_uav_load_additional_formats = options.TypedUAVLoadAdditionalFormats != FALSE;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device.Get()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                                                    sizeof(options1)))) {
        caps.wave_ops = options1.WaveOps != FALSE;
        caps.wave_lane_count_max = options1.WaveLaneCountMax;
        caps.int64 = options1.Int64ShaderOps != FALSE;
    }
    return caps;
}

} // Anonymous namespace

void SetDumpedShader(u64 unique_hash) {
    AddShaderHash(dumped_shaders, unique_hash);
}

bool IsDumpedShader(u64 unique_hash) {
    return HasShaderHash(dumped_shaders, unique_hash);
}

void SetSkippedShader(u64 unique_hash) {
    AddShaderHash(skipped_shaders, unique_hash);
    any_skipped_shader.store(true, std::memory_order_relaxed);
}

bool UsesSkippedShader(const std::array<u64, 6>& unique_hashes) {
    if (!any_skipped_shader.load(std::memory_order_relaxed)) {
        return false;
    }
    for (const u64 hash : unique_hashes) {
        if (HasShaderHash(skipped_shaders, hash)) {
            return true;
        }
    }
    return false;
}

void SetGeometryDroppedShader(u64 unique_hash) {
    AddShaderHash(gs_dropped_shaders, unique_hash);
}

void SetPipelineEviction(bool enabled) {
    pipeline_eviction.store(enabled, std::memory_order_relaxed);
}

void SetTransformFeedbackStorage(bool enabled) {
    transform_feedback_storage.store(enabled, std::memory_order_relaxed);
}

bool PipelineCache::NativeStreamOutput() const {
    return compiler.SupportsStreamOutput() &&
           !transform_feedback_storage.load(std::memory_order_relaxed);
}

void SetPipelinePrewarm(bool enabled) {
    pipeline_prewarm.store(enabled, std::memory_order_relaxed);
}

void SetComputeWorkgroupSplit(u32 factor) {
    compute_workgroup_split.store(factor, std::memory_order_relaxed);
}

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, const ShaderCompiler& compiler_,
                             const TextureCacheRuntime& texture_runtime_,
                             VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, compiler{compiler_},
      texture_runtime{texture_runtime_}, shader_notify{shader_notify_},
      root_signatures{device_},
      workers(GetTotalPipelineWorkers(), "D3D12PipelineBuilder", {},
              Common::ThreadPlacement::Background),
      serialization_thread(1, "D3D12PipelineSerialization", {},
                           Common::ThreadPlacement::Background),
      prewarm_thread(1, "D3D12PipelinePrewarm", {}, Common::ThreadPlacement::Background) {
    const ShaderCaps caps = QueryShaderCaps(device);
    profile = Shader::Profile{
        .supported_spirv = 0x00010300,
        .unified_descriptor_binding = true,
        .descriptor_arrays_use_count = true,
        // One SPIR-V variable per binding: DXIL rejects two resources on one register.
        .support_descriptor_aliasing = false,
        .support_int8 = false,
        .support_uniform_and_storage_buffer_8bit = false,
        .support_storage_buffer_8bit = false,
        .support_int16 = false,
        .support_uniform_and_storage_buffer_16bit = false,
        .support_storage_buffer_16bit = false,
        .support_int64 = caps.int64,
        .support_vertex_instance_id = false,
        // Without a denorm mode DXIL leaves it to the driver ("any"); Maxwell flushes, and the
        // FMZ emulation (x * 0 == 0) compares against zero, so flush everywhere.
        .support_float_controls = true,
        .support_fp32_denorm_flush = true,
        .force_fp32_denorm_flush = true,
        .support_explicit_workgroup_layout = false,
        .support_shader_quad_control = false,
        // Wave shuffles skip helper lanes before SM 6.7, so manual derivatives (SHFL + FSWZADD)
        // read garbage at triangle edges; quad shuffles avoid them (has_helperless_quad_shuffles).
        .support_quad_shuffles = caps.wave_ops,
        .support_vote = caps.wave_ops,
        .supported_subgroup_stages = caps.wave_ops ? 0x7Fu : 0u,
        .support_viewport_index_layer_non_geometry = caps.vp_and_rt_index_from_any_stage,
        .support_viewport_mask = false,
        .support_typeless_image_loads = caps.typed_uav_load_additional_formats,
        .repack_raw_image_loads = true,
        // Terminal discard arms may continue as helpers after DemoteReturnPass.
        // OpKill would terminate those lanes before their derivative calculations.
        .support_demote_to_helper_invocation = true,
        .support_int64_atomics = false,
        .support_shared_int64_atomics = false,
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = false,
        .support_native_ndc = false,
        .support_scaled_attributes = false,
        .support_multi_viewport = true,
        .support_geometry_streams = false,
        .support_sampled_image_array_nonuniform_indexing = true,
        .support_storage_image_array_nonuniform_indexing = false,
        .support_uniform_texel_buffer_array_nonuniform_indexing = false,
        .support_storage_texel_buffer_array_nonuniform_indexing = false,
        // The Series GPU (RDNA 2) runs waves of 64 lanes; Maxwell warps are 32.
        .warp_size_potentially_larger_than_guest = caps.wave_lane_count_max > 32,
        .lower_left_origin_mode = false,
        .need_declared_frag_colors = true,
        .need_gather_subpixel_offset = true,
        .has_broken_spirv_clamp = false,
        .has_broken_spirv_position_input = false,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = false,
        .has_broken_fp32_denorm_flush = false,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic = false,
        // A vertex shader whose loop exit ANDs the counter with the guest condition hung the
        // Series GPU on its first draw, with valid DXIL; the same shader without it draws fine.
        .has_broken_loop_safety_counter = true,
        // SM 6.4 on the Series: quad ops in a shader without derivatives read helper lanes that
        // never ran, so a quad leader outside the triangle gives garbage along its edges.
        .has_helperless_quad_shuffles = true,
        .has_broken_robust = false,
        // D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT: raw views start on 16 bytes.
        .min_ssbo_alignment = 16,
        .max_user_clip_distances = 8,
    };
    const LocalMemoryScratchSelection scratch =
        SelectLocalMemoryScratch(device, compiler, caps.wave_ops, caps.wave_lane_count_max);
    profile.local_memory_scratch_lanes = scratch.lanes;
    profile.local_memory_scratch_stages = scratch.stages;
    profile.local_memory_private_words = LocalMemoryPrivateWords();
    if (scratch.stages != 0) {
        LOG_INFO(Render,
                 "D3D12: indexed local memory above {} words in the UAV pool (stages {:#x})",
                 profile.local_memory_private_words, scratch.stages);
    }
    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = 16,
        .max_per_stage_descriptor_sampled_images = 128,
        .max_per_stage_resources = 128,
        .max_descriptor_set_samplers = 2048,
        .max_descriptor_set_uniform_buffers = 14,
        .max_descriptor_set_uniform_buffers_dynamic = 0,
        .max_descriptor_set_storage_buffers = 64,
        .max_descriptor_set_storage_buffers_dynamic = 0,
        .max_descriptor_set_sampled_images = 128,
        .max_descriptor_set_storage_images = 64,
        .max_descriptor_set_input_attachements = 0,
        .support_float64 = caps.float64,
        .support_float16 = false,
        .support_int64 = caps.int64,
        .needs_demote_reorder = true,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = caps.vp_and_rt_index_from_any_stage,
        .support_geometry_shader_passthrough = false,
        .support_conditional_barrier = false,
    };
    host_info.ApplyDescriptorLimitPolicy();
    profile.sanitize_cube_directions = GetCubeDirectionSanitize();
    profile.flush_lowered_fp16_denorms = GetHalfFloatFlush();

    LOG_INFO(Render,
             "D3D12: pipeline cache ready ({} workers, waves up to {} lanes, int64 {}, "
             "VP/RT index outside GS {}, dependent draws preserved)",
             GetTotalPipelineWorkers(), caps.wave_lane_count_max, caps.int64,
             caps.vp_and_rt_index_from_any_stage);
}

PipelineCache::~PipelineCache() {
    if (hot_pipelines_dirty) {
        SaveHotPipelines();
        serialization_thread.WaitForRequests();
    }
}

void PipelineCache::NoteFoldCheck(GraphicsPipeline* pipeline) {
    const auto [first, is_first] = fold_check_first.try_emplace(
        PipelineKeyFold::Fold(pipeline->Key(), PipelineKeyFold::ActiveRules()), pipeline);
    if (!is_first) {
        fold_check_pending.emplace_back(first->second, pipeline);
    }
}

void PipelineCache::TickFoldCheck() {
    // Most pairs may stay dormant for long: each frame looks at a few and moves on.
    static constexpr size_t PAIRS_PER_FRAME = 8;
    for (size_t step = 0; step < PAIRS_PER_FRAME && !fold_check_pending.empty(); ++step) {
        const auto [a, b] = fold_check_pending.front();
        fold_check_pending.pop_front();
        if (!a->IsBuilt() || !b->IsBuilt()) {
            fold_check_pending.emplace_back(a, b);
            continue;
        }
        if (!a->Handle() || !b->Handle()) {
            continue;
        }
        const auto identity_a = a->FoldIdentity();
        const auto identity_b = b->FoldIdentity();
        if (!identity_a || !identity_b) {
            continue;
        }
        ++fold_check_compared;
        if (*identity_a == *identity_b) {
            continue;
        }
        ++fold_check_mismatches;
        const bool same_shaders = std::ranges::equal(
            identity_a->shaders, identity_b->shaders,
            [](const auto& x, const auto& y) { return std::ranges::equal(x, y); });
        std::string rules;
        for (size_t index = 0; index < PipelineKeyFold::NUM_RULES; ++index) {
            const u32 bit = 1U << index;
            if (PipelineKeyFold::Fold(a->Key(), bit) != a->Key() ||
                PipelineKeyFold::Fold(b->Key(), bit) != b->Key()) {
                rules += fmt::format(" {}", PipelineKeyFold::RULE_NAMES[index]);
            }
        }
        LOG_ERROR(Render,
                  "D3D12: folding would merge pipelines {:016x} and {:016x} (VS {:016x} PS "
                  "{:016x}) whose {} differ; rules that change them:{}",
                  a->KeyHash(), b->KeyHash(), a->Key().unique_hashes[1], a->Key().unique_hashes[5],
                  same_shaders ? "descriptions" : "shaders", rules);
    }
}

void PipelineCache::TickResidency(const CacheMemorySnapshot& snapshot, Scheduler& scheduler) {
    using Policy = PipelineResidencyPolicy;
    ++residency_frame;
    if (!fold_check_pending.empty()) {
        TickFoldCheck();
    }
    u64 idle_frames = Policy::IdleFrames(snapshot.app_used, snapshot.app_limit);
    const u64 ledger = Common::MemoryLedgerCommittedBytes();
    const auto verdict_before = release_probe.GetVerdict();
    release_probe.Tick(residency_frame, pipelines_evicted,
                       snapshot.app_used > ledger ? snapshot.app_used - ledger : 0,
                       snapshot.app_limit != 0);
    if (release_probe.GetVerdict() != verdict_before) {
        const bool pays = release_probe.GetVerdict() == PipelineReleaseProbe::Verdict::Pays;
        LOG_INFO(Render,
                 "D3D12: releasing {} idle PSOs freed {} KiB of untracked commit; idle PSOs {}",
                 release_probe.Probed(), release_probe.Freed() / 1024,
                 pays ? "released under pressure"
                      : "kept under pressure (they cost no commit), released only in an emergency");
    }
    // A pipeline object that ran out of memory: idle ones are released as in an emergency for a
    // while, and the memory reserve is committed again once the app is no longer short of it.
    const u64 oom_events = MemoryReserve::OutOfMemoryEvents();
    if (oom_events != seen_oom_events) {
        seen_oom_events = oom_events;
        oom_frame = residency_frame;
        LOG_WARNING(Render, "D3D12: {} pipeline objects ran out of memory so far; releasing idle "
                            "pipelines",
                    oom_events);
    }
    const bool after_oom = oom_events != 0 && residency_frame - oom_frame < OOM_RETRY_FRAMES;
    if (after_oom) {
        idle_frames = Policy::EMERGENCY_IDLE_FRAMES;
    } else if (oom_events != 0 && idle_frames >= Policy::IDLE_FRAMES &&
               residency_frame % OOM_REARM_FRAMES == 0 && MemoryReserve::Held() == 0) {
        MemoryReserve::Rearm();
    }
    const bool evict =
        (pipeline_eviction.load(std::memory_order_relaxed) && release_probe.MayRelease()) ||
        idle_frames == Policy::EMERGENCY_IDLE_FRAMES;
    const size_t checks = evict ? (std::min)(Policy::SWEEP_PER_FRAME, residency_sweep.size()) : 0;
    for (size_t i = 0; i < checks; ++i) {
        if (residency_cursor >= residency_sweep.size()) {
            residency_cursor = 0;
        }
        GraphicsPipeline* const pipeline = residency_sweep[residency_cursor++];
        if (residency_frame - pipeline->LastUsedFrame() > idle_frames &&
            pipeline->Evict(scheduler)) {
            ++pipelines_evicted;
        }
    }
    TickPrewarm(snapshot);
    // A session closed from the console never reaches the destructor, and the set changes most at
    // the start. A minute at 60 FPS is two or three at the frame rates seen, longer than many test
    // sessions, so save what changed every 10 seconds' worth of frames.
    constexpr u64 HOT_SAVE_FRAMES = 10 * 60;
    if (hot_pipelines_dirty && residency_frame - last_hot_save_frame >= HOT_SAVE_FRAMES) {
        last_hot_save_frame = residency_frame;
        SaveHotPipelines();
    }
    constexpr u64 STATS_FRAMES = 60 * 60;
    if (residency_frame - last_stats_frame >= STATS_FRAMES) {
        last_stats_frame = residency_frame;
        const auto resident = Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates);
        const auto dxil_release = GraphicsPipeline::GetDxilReleaseStats();
        LOG_INFO(Render,
                 "D3D12: {} graphics pipelines, {} PSOs resident; {} released and {} rebuilt so "
                 "far; {} drawn in recent sessions; dormant DXIL left on disk {} ({} KiB), read "
                 "back {}, failed {}",
                 residency_sweep.size(), resident.objects, pipelines_evicted, pipelines_woken,
                 hot_pipelines.Size(), dxil_release.released, dxil_release.released_bytes / 1024,
                 dxil_release.reloaded, dxil_release.failed);
        LogPipelineStats("session");
        if (PsoBlobCache::Enabled()) {
            const PsoBlobCache::Stats blobs = GetPsoBlobCache().GetStats();
            LOG_INFO(Render,
                     "D3D12: driver PSO blobs: {} created from disk, {} without, {} stored "
                     "({} KiB), {} refused by the driver",
                     blobs.hits, blobs.misses, blobs.stored, blobs.bytes / 1024, blobs.rejected);
        }
        if (GetAsyncPipelines() != AsyncPipelines::Off ||
            GetPsoBudget() != VideoCore::FramePipeline::Mode::Off) {
            const PendingDrawStats pending = GetPendingDrawStats();
            LOG_INFO(Render,
                     "D3D12: draws while their pipeline was built (since the run began): {} "
                     "skipped; waited {} small, {} full-screen, {} first to a target, {} within "
                     "the frame's budget; pso_budget=check: {} the budget would skip, {} it "
                     "would wait for",
                     pending.skipped, pending.waited_small, pending.waited_full_screen,
                     pending.waited_new_target, pending.waited_budget, pending.budget_would_skip,
                     pending.budget_would_wait);
        }
    }
}

void PipelineCache::TickPrewarm(const CacheMemorySnapshot& snapshot) {
    if (prewarm_cursor >= prewarm_order.size() || (prewarm_last && !prewarm_last->IsBuilt()) ||
        !pipeline_prewarm.load(std::memory_order_relaxed) ||
        !PipelineResidencyPolicy::MayPrewarm(snapshot.app_used, snapshot.app_limit)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (prewarm_last && !prewarm_pacer.Rested(now)) {
        return;
    }
    prewarm_last = nullptr;
    while (prewarm_cursor < prewarm_order.size()) {
        GraphicsPipeline* const pipeline = prewarm_order[prewarm_cursor++];
        // Pipelines drawn meanwhile were woken by their draw and are no longer dormant; one
        // released while idle since is left to its next draw, unless a loading screen queued it
        // to be built again.
        const bool rebuild = prewarm_cursor > prewarm_rebuild_from;
        if ((rebuild || !pipeline->WasEvicted()) && pipeline->Wake(prewarm_thread)) {
            // Counted as used now, so eviction does not take it back before its first draw.
            pipeline->Touch(residency_frame);
            pipeline->MarkPrewarmed();
            prewarm_last = pipeline;
            prewarm_pacer.Queued(now);
            GetPipelineStats().prewarm_queued.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    if (prewarm_cursor == prewarm_order.size()) {
        LOG_INFO(Render, "D3D12: background prewarm done ({} dormant PSOs queued, {} drawn so far)",
                 GetPipelineStats().prewarm_queued.load(std::memory_order_relaxed),
                 GetPipelineStats().prewarm_hits.load(std::memory_order_relaxed));
        prewarm_order = {};
        prewarm_cursor = 0;
        prewarm_rebuild_from = SIZE_MAX;
    }
}

void PipelineCache::SetLoadingScreen(bool loading) {
    using VideoCore::FramePipeline::Mode;
    const Mode mode = GetPsoLoading();
    if (mode == Mode::Off || loading == loading_screen) {
        return;
    }
    loading_screen = loading;
    if (mode == Mode::On) {
        prewarm_pacer.rest = loading ? 0 : PrewarmPacer::REST;
    }
    if (!loading) {
        LOG_INFO(Render, "D3D12: loading screen {} ended", loading_screens);
        return;
    }
    ++loading_screens;
    // Released pipelines the game drew in a kept session, unless the last loading screen's are
    // still queued.
    const bool queued = prewarm_rebuild_from != SIZE_MAX;
    size_t released = 0;
    if (!queued) {
        if (mode == Mode::On) {
            prewarm_rebuild_from = prewarm_order.size();
        }
        for (GraphicsPipeline* const pipeline : residency_sweep) {
            if (pipeline->WasEvicted() && pipeline->IsDormant() &&
                hot_pipelines.Known(pipeline->KeyHash())) {
                ++released;
                if (mode == Mode::On) {
                    prewarm_order.push_back(pipeline);
                }
            }
        }
        if (mode == Mode::On && released == 0) {
            prewarm_rebuild_from = SIZE_MAX;
        }
    }
    LOG_INFO(Render,
             "D3D12: loading screen {}: {} released pipelines of earlier sessions {}, {} dormant "
             "PSOs left to prewarm{}",
             loading_screens, released,
             mode == Mode::On ? "queued to build again" : "would be built again (pso_loading=check)",
             prewarm_order.size() - (std::min)(prewarm_cursor, prewarm_order.size()),
             mode == Mode::On ? ", built back to back" : "");
}

void PipelineCache::LogPipelineStats(std::string_view when) const {
    const PipelineStats& stats = GetPipelineStats();
    const auto ms = [](u64 us) { return static_cast<double>(us) / 1000.0; };
    constexpr std::array<std::pair<PipelineStage, const char*>,
                         static_cast<size_t>(PipelineStage::Count)>
        names{{
            {PipelineStage::Frontend, "frontend"},
            {PipelineStage::QueueWait, "queue"},
            {PipelineStage::Dxil, "dxil"},
            {PipelineStage::Translate, "mesa"},
            {PipelineStage::SignLock, "sign-lock"},
            {PipelineStage::Sign, "sign"},
            {PipelineStage::PsoCreate, "pso"},
            {PipelineStage::WaitNew, "wait-new"},
            {PipelineStage::WaitDormant, "wait-dormant"},
            {PipelineStage::WaitEvicted, "wait-evicted"},
            {PipelineStage::WaitCompute, "wait-compute"},
        }};
    std::string line;
    for (const auto& [stage, name] : names) {
        const auto summary = stats.Read(stage);
        if (summary.count == 0) {
            continue;
        }
        line += fmt::format("; {} n={} p50={:.1f} p95={:.1f} p99={:.1f} max={:.1f} total={:.0f}",
                            name, summary.count, ms(summary.p50), ms(summary.p95),
                            ms(summary.p99), ms(summary.max), ms(summary.total));
    }
    LOG_INFO(Render,
             "D3D12 pipeline stages ({}, ms): {} inline builds, {} prewarmed, {} of them drawn{}",
             when, stats.inline_builds.load(std::memory_order_relaxed),
             stats.prewarm_queued.load(std::memory_order_relaxed),
             stats.prewarm_hits.load(std::memory_order_relaxed), line);
    if (PipelineKeyFold::GetMode() != PipelineKeyFold::Mode::Off) {
        LOG_INFO(Render,
                 "D3D12 pipeline key folding ({}): {} keys found a pipeline through it; checks: {} "
                 "pairs compared, {} differ, {} waiting",
                 when, fold_aliases, fold_check_compared, fold_check_mismatches,
                 fold_check_pending.size());
    }
    LogDxilDigestChecks();
    LogTranslatorPhases(when);
}

void PipelineCache::SaveHotPipelines() {
    hot_pipelines_dirty = false;
    if (hot_pipelines_filename.empty()) {
        return;
    }
    serialization_thread.QueueWork([bytes = hot_pipelines.Serialize(),
                                    path = hot_pipelines_filename] {
        // Written aside and renamed over the old one, so a session killed mid-write keeps it.
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                LOG_WARNING(Render, "D3D12: could not write {}", temporary.string());
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            LOG_WARNING(Render, "D3D12: could not replace {}: {}", path.string(), error.message());
        }
    });
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    if (VideoCommon::GPUThread::HasExceptionReported())
        return nullptr;
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        key_reuse.Forget();
        return nullptr;
    }
    // No register the key reads changed and the rest of its inputs are the same: the previous
    // draw's pipeline stands without refreshing and comparing the key.
    const PipelineKeyTracking::Inputs inputs{
        .engine = maxwell3d,
        .topology = maxwell3d->draw_manager.draw_state.topology,
        .engine_state = maxwell3d->engine_state,
        .unique_hashes = graphics_key.unique_hashes,
    };
    auto& flags = maxwell3d->dirty.flags;
    const PipelineKeyTracking::Mode mode = PipelineKeyTracking::GetMode();
    const bool reusable = mode != PipelineKeyTracking::Mode::Off &&
                          key_reuse.Allows(inputs, flags[PipelineKeyTracking::KeyRegisters]);
    flags[PipelineKeyTracking::KeyRegisters] = false;
    if (reusable && mode == PipelineKeyTracking::Mode::On) {
        VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::PipelineKeyReuses, 1);
        return BuiltPipeline(current_pipeline);
    }
    GraphicsPipeline* const previous = current_pipeline;
    GraphicsPipeline* const pipeline = LookupGraphicsPipeline();
    if (pipeline) {
        key_reuse.Remember(inputs);
    } else {
        key_reuse.Forget();
    }
    if (reusable) {
        // Counted without detailed profiling so a check run alone shows how much it verified.
        VideoCore::Perf::Add(VideoCore::Perf::Counter::PipelineKeyChecks, 1);
        if (pipeline != previous) {
            ReportKeyReuseMismatch(previous);
        }
    }
    return pipeline;
}

GraphicsPipeline* PipelineCache::LookupGraphicsPipeline() {
    // The D3D12 rasterizer installs Vulkan's fixed-state dirty tables for the channel. Refresh
    // only rereads blocks whose Maxwell registers changed.
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    graphics_key.RefreshStatic(maxwell3d->regs);
    graphics_key.state.dynamic_state.shader_mirror_border.Assign(
        !mirror_border_shaders.empty() &&
        mirror_border_shaders.contains(graphics_key.unique_hashes[5]));
    if (UsesReductionShader(graphics_key.unique_hashes)) {
        graphics_key.shader_flags |= GraphicsPipelineCacheKey::SHADER_REDUCTION;
    }

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::PipelineFastHits, 1);
            current_pipeline = next;
            return BuiltPipeline(current_pipeline);
        }
    }
    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::PipelineFastMisses, 1);
    return CurrentGraphicsPipelineSlowPath();
}

bool PipelineCache::UsesReductionShader(const std::array<u64, 6>& unique_hashes) const {
    if (reduction_shaders.empty()) [[likely]] {
        return false;
    }
    // VertexA (index 0) is translated together with VertexB.
    for (size_t index = 1; index < unique_hashes.size(); ++index) {
        if (unique_hashes[index] != 0 && reduction_shaders.contains(unique_hashes[index])) {
            return true;
        }
    }
    return false;
}

void PipelineCache::EmulateReduction(const GraphicsPipelineCacheKey& key, u8 stages) {
    if (!compiler.EmulatesReduction()) {
        return; // older spirv_to_dxil.dll: the samples keep filtering normally
    }
    bool added = false;
    for (size_t stage = 0; stage < Maxwell::MaxShaderStage; ++stage) {
        const u64 hash = key.unique_hashes[stage + 1];
        if (((stages >> stage) & 1) != 0 && hash != 0 && reduction_shaders.insert(hash).second) {
            added = true;
            LOG_INFO(Render, "D3D12: shader {:016x} (stage {}) samples with a filtered MIN/MAX "
                             "reduction; using its emulating pipelines",
                     hash, stage);
        }
    }
    if (added) {
        // The previous draw's pipeline must not be reused without the new key bit.
        key_reuse.Forget();
    }
}

void PipelineCache::EmulateMirrorBorder(const GraphicsPipelineCacheKey& key) {
    if (mirror_border_shaders.insert(key.unique_hashes[5]).second) {
        LOG_INFO(Render, "D3D12: shader {:016x} samples with a sampler that mirrors once to the "
                         "border; using its emulating pipelines",
                 key.unique_hashes[5]);
        key_reuse.Forget();
    }
}

void PipelineCache::EmulateReduction(u64 compute_hash) {
    if (compiler.EmulatesReduction() && reduction_shaders.insert(compute_hash).second) {
        LOG_INFO(Render, "D3D12: compute shader {:016x} samples with a filtered MIN/MAX "
                         "reduction; using its emulating pipeline",
                 compute_hash);
    }
}

void PipelineCache::ReportKeyReuseMismatch(const GraphicsPipeline* reused) {
    // Check mode: the key changed although no tracked register did, so some register the key
    // reads is in PipelineKeyTracking::IgnoredRegisters or was written past the dirty tables.
    VideoCore::Perf::Add(VideoCore::Perf::Counter::PipelineKeyMismatches, 1);
    const GraphicsPipelineCacheKey& before = reused->Key();
    const auto* const a = reinterpret_cast<const u8*>(&before);
    const auto* const b = reinterpret_cast<const u8*>(&graphics_key);
    size_t byte = 0;
    while (byte < sizeof(GraphicsPipelineCacheKey) && a[byte] == b[byte]) {
        ++byte;
    }
    if (++key_reuse_mismatches <= 32) {
        LOG_ERROR(Render, "D3D12: pipeline key changed with no tracked register write (first "
                          "differing byte {} of {})",
                  byte, sizeof(GraphicsPipelineCacheKey));
    }
    BUG_TRACK_KEY(Renderer, byte, "pipeline key changed with no tracked register write (byte {})",
                  byte);
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {
    if (VideoCommon::GPUThread::HasExceptionReported())
        return nullptr;
    const VideoCommon::ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const bool emulate_reduction =
        !reduction_shaders.empty() && reduction_shaders.contains(key.unique_hash);
    // The two variants are independent: the emulation rewrites samples, the split ids and loops.
    const u32 workgroup_split =
        compute_not_split.contains(key.Hash()) ? 1 : WorkgroupSplitFor(*kepler_compute);
    auto& cache = emulate_reduction
                      ? (workgroup_split > 1 ? compute_reduction_split_cache
                                             : compute_reduction_cache)
                  : workgroup_split > 1 ? compute_split_cache
                                        : compute_cache;
    const auto [pair, is_new]{cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (is_new) {
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs};
        VideoCore::Perf::ScopedTimer frontend{VideoCore::Perf::Counter::PipelineFrontendUs};
        pipeline = CreateComputePipeline(key, shader, emulate_reduction, workgroup_split);
    }
    if (is_new && !pipeline && workgroup_split > 1 && compute_not_split.contains(key.Hash())) {
        cache.erase(pair);
        return CurrentComputePipeline();
    }
    if (!pipeline) {
        BUG_TRACK_KEY(DrawSkipped, key.unique_hash,
                      "dispatches skipped: no pipeline for compute {:016x} (shader build failed)",
                      key.unique_hash);
        return nullptr;
    }
    // Still building, it is not waited for here: a skipped dispatch would leave the buffers or
    // images it writes stale, so the dispatch is recorded anyway and the command recorder waits
    // for the PSO (RasterizerD3D12::DispatchCompute).
    if (pipeline->OutOfMemory() && residency_frame >= pipeline->retry_frame &&
        !pipeline->RetryOutOfMemory()) {
        pipeline->retry_frame = residency_frame + OOM_RETRY_FRAMES;
    }
    return pipeline.get();
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    GetPipelineStats().Reset();
    if (!pipeline_cache_filename.empty()) {
        serialization_thread.WaitForRequests();
    }
    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "d3d12.bin";
    hot_pipelines_filename = base_dir / "d3d12_hot.bin";
    if (IsDxilDiskCacheEnabled() && compiler.TranslatorGeneration() != 0) {
        GetDxilDiskCache().Open(base_dir / "d3d12_dxil.bin", compiler.TranslatorGeneration());
    } else {
        GetDxilDiskCache().Close();
    }
    GetPsoBlobCache().Open(PsoBlobCache::Enabled() ? base_dir / "d3d12_pso_blobs"
                                                   : std::filesystem::path{});
    {
        std::ifstream file(hot_pipelines_filename, std::ios::binary);
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(file),
                                    std::istreambuf_iterator<char>()};
        hot_pipelines.Load(bytes);
    }

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        size_t graphics{};   ///< Graphics pipelines read, in file order (HotPipelineSet::Prewarm).
        size_t prewarmed{};
        /// Left without a PSO, by position in the file, for the background prewarm.
        std::vector<std::pair<size_t, GraphicsPipeline*>> dormant;
        bool has_loaded{};
    } state;
    const size_t boot_worker_count = GetTotalPipelineBootWorkers();
    Common::ThreadWorker boot_workers(boot_worker_count, "D3D12PipelineBoot", {},
                                      Common::ThreadPlacement::Background);

    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        boot_workers.QueueWork([this, key, env_ = std::move(env), &state, &callback]() mutable {
            ShaderPools pools;
            auto pipeline{CreateComputePipeline(pools, key, env_, false)};
            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                compute_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    const PipelineKeyFold::Mode fold_mode = PipelineKeyFold::GetMode();
    const u32 fold_rules = PipelineKeyFold::ActiveRules();
    PipelineKeyFold::Census fold_census{fold_rules};
    std::unordered_set<u64> folded_keys;
    // Pipelines with a stored key in the hot set that was not the first to fold into them.
    std::unordered_set<u64> hot_through_fold;
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        if (fold_mode != PipelineKeyFold::Mode::Off) {
            fold_census.Add(key);
        }
        const u64 stored_hash = key.Hash();
        if (fold_mode == PipelineKeyFold::Mode::On) {
            key = PipelineKeyFold::Fold(key, fold_rules);
            if (!folded_keys.insert(key.Hash()).second) {
                if (hot_pipelines.Known(stored_hash)) {
                    hot_through_fold.insert(key.Hash());
                }
                return; // Its pipeline is on its way, and nothing is translated for this one.
            }
        }
        // Every pipeline gets its DXIL; only the hot ones a PSO, the rest when first drawn.
        const size_t file_index = state.graphics++;
        const bool build_pso = hot_pipelines.Prewarm(stored_hash, file_index) ||
                               hot_pipelines.Prewarm(key.Hash(), file_index);
        state.prewarmed += build_pso ? 1 : 0;

        boot_workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback, build_pso,
                                file_index]() mutable {
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{
                CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), false, build_pso)};

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                if (!build_pso) {
                    state.dormant.emplace_back(file_index, pipeline.get());
                }
                graphics_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);

    LOG_INFO(Render,
             "D3D12: {} pipelines in the disk cache; PSOs at boot for {} of {} graphics pipelines "
             "({})",
             state.total, state.prewarmed, state.graphics,
             hot_pipelines.HasHistory()
                 ? fmt::format("drawn in the last sessions, {} known", hot_pipelines.Size())
                 : std::string("no history yet: the first ones of the cache"));
    if (fold_mode != PipelineKeyFold::Mode::Off) {
        LOG_INFO(Render, "D3D12: pipeline cache: {}", fold_census.Describe());
    }

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    const auto build_start = std::chrono::steady_clock::now();
    boot_workers.WaitForRequests(stop_loading);
    LOG_INFO(Render, "D3D12: disk cache built by {} loading screen workers in {} ms",
             boot_worker_count,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - build_start)
                 .count());
    const auto preparation = VideoCore::Perf::Read();
    LOG_INFO(Render,
             "D3D12 PSO preparation after disk load (session totals): {} requests, {} hits, "
             "{} shared in-flight, {} capacity bypasses, {} driver creates / {:.1f} ms "
             "accumulated worker wall time; {} resident objects",
             VideoCore::Perf::Get(preparation, VideoCore::Perf::Counter::PsoRequests),
             VideoCore::Perf::Get(preparation, VideoCore::Perf::Counter::PsoHits),
             VideoCore::Perf::Get(preparation, VideoCore::Perf::Counter::PsoShared),
             VideoCore::Perf::Get(preparation, VideoCore::Perf::Counter::PsoBypasses),
             VideoCore::Perf::Get(preparation, VideoCore::Perf::Counter::PsoCreates),
             VideoCore::Perf::Get(preparation, VideoCore::Perf::Counter::PsoCreateUs) / 1000.0,
             Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects);
    residency_sweep.reserve(graphics_cache.size());
    for (const auto& [key, pipeline] : graphics_cache) {
        if (pipeline) {
            residency_sweep.push_back(pipeline.get());
            if (fold_mode == PipelineKeyFold::Mode::Check) {
                NoteFoldCheck(pipeline.get());
            }
        }
    }
    // The background prewarm's order: drawn in a kept session but past the boot cap first, then
    // the rest in the order the game first drew them, which is roughly the order it goes through
    // its areas. Pipelines whose DXIL failed are Ready, not dormant, and are skipped when reached.
    std::ranges::sort(state.dormant, [this, &hot_through_fold](const auto& a, const auto& b) {
        const auto known = [&](const GraphicsPipeline* pipeline) {
            return hot_pipelines.Known(pipeline->KeyHash()) ||
                   hot_through_fold.contains(pipeline->KeyHash());
        };
        const bool known_a = known(a.second);
        const bool known_b = known(b.second);
        return known_a != known_b ? known_a : a.first < b.first;
    });
    prewarm_order.clear();
    prewarm_order.reserve(state.dormant.size());
    for (const auto& entry : state.dormant) {
        prewarm_order.push_back(entry.second);
    }
    prewarm_cursor = 0;
    prewarm_last = nullptr;
    LogPipelineStats("boot");
    GetDxilDiskCache().LogStats("boot");
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    // Only here, past every faster way to find a pipeline: the draws that find one pay nothing,
    // and graphics_key stays as the registers give it, since Refresh rewrites only what changed.
    const PipelineKeyFold::Mode fold_mode = PipelineKeyFold::GetMode();
    const GraphicsPipelineCacheKey key = fold_mode == PipelineKeyFold::Mode::On
                                             ? PipelineKeyFold::Fold(graphics_key,
                                                                     PipelineKeyFold::ActiveRules())
                                             : graphics_key;
    const auto [pair, is_new]{graphics_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (is_new) {
        // Translating the shaders runs here, on the GPU thread, before the build is queued.
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs};
        VideoCore::Perf::ScopedTimer frontend{VideoCore::Perf::Counter::PipelineFrontendUs};
        const u64 frontend_start = SteadyMicros();
        pipeline = CreateGraphicsPipeline(key);
        GetPipelineStats().Record(PipelineStage::Frontend, SteadyMicros() - frontend_start);
        if (pipeline) {
            residency_sweep.push_back(pipeline.get());
            if (fold_mode == PipelineKeyFold::Mode::Check) {
                NoteFoldCheck(pipeline.get());
            }
        }
    } else if (key != graphics_key) {
        ++fold_aliases;
    }
    if (!pipeline) {
        // Its shaders failed to recompile or translate (reported once, below): every draw with
        // this state is lost.
        BUG_TRACK_KEY(DrawSkipped,
                      graphics_key.unique_hashes[1] ^ std::rotl(graphics_key.unique_hashes[5], 1),
                      "draws skipped: no pipeline for VS {:016x} PS {:016x} (shader build failed)",
                      graphics_key.unique_hashes[1], graphics_key.unique_hashes[5]);
        return nullptr;
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(graphics_key, pipeline.get());
    }
    current_pipeline = pipeline.get();
    return BuiltPipeline(current_pipeline);
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) {
    pipeline->Touch(residency_frame);
    if (pipeline->MarkDrawn()) {
        if (pipeline->Prewarmed()) {
            GetPipelineStats().prewarm_hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (hot_pipelines.MarkUsed(pipeline->KeyHash())) {
            hot_pipelines_dirty = true;
        }
    }
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (residency_frame < pipeline->RetryFrame()) {
        return nullptr; // its PSO ran out of memory: draws skipped until the retry is due
    }
    if (pipeline->IsDormant() && pipeline->TakeOutOfMemory()) {
        // A build nobody waited for (async_pipelines) ran out of memory.
        pipeline->SetRetryFrame(residency_frame + OOM_RETRY_FRAMES);
        return nullptr;
    }
    // Its PSO was never built (not drawn in recent sessions) or was released while idle. Queued
    // like any build, but WaitBuilt claims it unless a worker already started it.
    if (pipeline->Wake(workers)) {
        ++pipelines_woken;
    }
    if (GetAsyncPipelines() != AsyncPipelines::Off) {
        return pipeline; // the draw skips it or waits (WaitPending)
    }
    return WaitPending(pipeline);
}

GraphicsPipeline* PipelineCache::WaitPending(GraphicsPipeline* pipeline) {
    const PipelineStage cause = pipeline->WasEvicted() ? PipelineStage::WaitEvicted
                                : pipeline->FromDisk() ? PipelineStage::WaitDormant
                                                       : PipelineStage::WaitNew;
    const u64 wait_start = SteadyMicros();
    // A valid draw has no semantically equivalent substitute while its exact PSO is pending.
    // Compilation stays on workers, but readiness never discards guest work.
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs,
                                       VideoCore::Perf::Counter::PipelineStalls};
    VideoCore::Perf::ScopedTimer wait{VideoCore::Perf::Counter::PipelineWaitUs};
    pipeline->WaitBuilt();
    GetPipelineStats().Record(cause, SteadyMicros() - wait_start);
    if (!pipeline->IsBuilt()) {
        // Out of memory: idle pipelines are released meanwhile (TickResidency), and the draws
        // skipped until this one is tried again.
        (void)pipeline->TakeOutOfMemory();
        pipeline->SetRetryFrame(residency_frame + OOM_RETRY_FRAMES);
        return nullptr;
    }
    return pipeline;
}

bool PipelineCache::WaitPendingFor(GraphicsPipeline* pipeline, u64 microseconds) {
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs,
                                       VideoCore::Perf::Counter::PipelineStalls};
    VideoCore::Perf::ScopedTimer wait{VideoCore::Perf::Counter::PipelineWaitUs};
    return pipeline->WaitBuiltFor(microseconds);
}

namespace {
// A shader that fails to build: whether its code came from the disk cache or from guest memory,
// and from where in guest memory. The first failures also keep their guest code in the log folder
// (guest_fault_<first>_<second>_<stage>.ash), to read offline what the recompiler did not handle.
void LogShaderSources(std::span<Shader::Environment* const> envs, u64 first_hash,
                      u64 second_hash) {
    constexpr int MAX_DUMPED_SOURCES = 16;
    static std::atomic_int dumped;
    const bool dump{dumped.fetch_add(1, std::memory_order_relaxed) < MAX_DUMPED_SOURCES};
    for (size_t i = 0; i < envs.size(); ++i) {
        LOG_ERROR(Render, "D3D12: failed shader stage {}: {}", i, envs[i]->DescribeCode());
        if (dump) {
            envs[i]->DumpToLogFolder(
                fmt::format("guest_fault_{:016x}_{:016x}_{}.ash", first_hash, second_hash, i));
        }
    }
    if (dump) {
        LOG_ERROR(Render, "D3D12: guest code of {:016x} {:016x} dumped to the log folder",
                  first_hash, second_hash);
    }
}

// The two shaders an emulated tessellation runs before the draw (TessEmulationPlan): the vertex
// shader alone, storing to the capture and unflipped since the evaluation shader flips, and the
// control shader as compute. Returns the bytes added to linked.
u64 CompileTessEmulation(const ShaderCompiler& compiler,
                         std::span<const std::vector<u32>, Maxwell::MaxShaderStage> spirv,
                         bool emulate_reduction, u64 trace_pipeline,
                         GraphicsPipeline::LinkedDxil& linked) {
    const std::array<ShaderCompiler::PipelineStage, 1> capture{{
        {.spirv = spirv[0], .stage = DXIL_SPIRV_SHADER_VERTEX},
    }};
    auto vertex = compiler.CompilePipeline(capture,
                                           {.yz_flip = DXIL_SPIRV_YZ_FLIP_NONE,
                                            .first_vertex_and_base_instance =
                                                DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA,
                                            .emulate_reduction = emulate_reduction},
                                           trace_pipeline);
    linked.tess_capture = std::move(vertex[0].dxil);
    const std::array<ShaderCompiler::PipelineStage, 1> control{{
        {.spirv = spirv[1], .stage = DXIL_SPIRV_SHADER_COMPUTE},
    }};
    auto compute = compiler.CompilePipeline(control, {.emulate_reduction = emulate_reduction},
                                            trace_pipeline);
    linked.tess_control = std::move(compute[0].dxil);
    return linked.tess_capture.capacity() + linked.tess_control.capacity();
}

/// Where the fragment shader's per-primitive texture handle comes from: the vertex attribute the
/// vertex shader copies to that output. Throws what cannot be drawn, like any unsupported shader.
std::optional<GraphicsPipeline::VertexTextureHandle> FindVertexTextureHandle(
    const std::array<Shader::IR::Program, Maxwell::MaxShaderProgram>& programs,
    const GraphicsPipelineCacheKey& key) {
    using Shader::IR::Attribute;
    using Shader::IR::Opcode;
    if (key.unique_hashes[5] == 0) {
        return std::nullopt;
    }
    std::optional<u32> output;
    for (const Shader::TextureDescriptor& desc : programs[5].info.texture_descriptors) {
        if (!desc.varying_handle) {
            continue;
        }
        if (output && *output != desc.cbuf_offset) {
            throw Shader::NotImplementedException("Textures with two per-primitive handles");
        }
        output = desc.cbuf_offset;
    }
    if (!output) {
        return std::nullopt;
    }
    if (key.unique_hashes[2] != 0 || key.unique_hashes[3] != 0 || key.unique_hashes[4] != 0) {
        throw Shader::NotImplementedException("Per-primitive handle after the vertex shader");
    }
    const Shader::IR::Inst* source{};
    for (const Shader::IR::Block* const block : programs[1].post_order_blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != Opcode::SetAttribute || !inst.Arg(0).IsImmediate() ||
                static_cast<u32>(inst.Arg(0).Attribute()) != *output) {
                continue;
            }
            if (source != nullptr || inst.Arg(1).IsImmediate()) {
                throw Shader::NotImplementedException("Computed per-primitive handle");
            }
            source = inst.Arg(1).InstRecursive();
        }
    }
    while (source != nullptr && (source->GetOpcode() == Opcode::BitCastF32U32 ||
                                 source->GetOpcode() == Opcode::BitCastU32F32)) {
        source = source->Arg(0).IsImmediate() ? nullptr : source->Arg(0).InstRecursive();
    }
    if (source == nullptr || source->GetOpcode() != Opcode::GetAttribute ||
        !source->Arg(0).IsImmediate() || !Shader::IR::IsGeneric(source->Arg(0).Attribute())) {
        throw Shader::NotImplementedException("Computed per-primitive handle");
    }
    const Attribute input{source->Arg(0).Attribute()};
    return GraphicsPipeline::VertexTextureHandle{
        .attribute = Shader::IR::GenericAttributeIndex(input),
        .component = Shader::IR::GenericAttributeElement(input),
    };
}
} // namespace

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, bool build_in_parallel, bool build_pso) try {
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};
    const bool drops_geometry{HasShaderHash(gs_dropped_shaders, key.unique_hashes[1])};
    const bool dumps_dxil{IsDumpedShader(key.unique_hashes[1]) ||
                          IsDumpedShader(key.unique_hashes[5])};

    // Layer passthrough generation for devices that cannot write the layer outside a GS
    Shader::IR::Program* layer_source_program{};

    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env);
        }
        if (programs[index].info.global_memory_fallbacks != 0 ||
            programs[index].info.global_memory_unresolved != 0) {
            LOG_WARNING(Render,
                        "D3D12: shader {:016x} (stage {}) uses global memory fallbacks: {} untracked, "
                        "{} unresolved, stores {}",
                        key.unique_hashes[index], index,
                        programs[index].info.global_memory_fallbacks,
                        programs[index].info.global_memory_unresolved,
                        programs[index].info.stores_global_memory);
        }
        if (Settings::values.dump_guest_shaders) {
            env.Dump(key.Hash(), key.unique_hashes[index]);
        }
        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }

    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> spirv;
    boost::container::static_vector<size_t, Maxwell::MaxShaderStage> stage_indices;

    // Transform feedback is captured where Maxwell captures it, after the last pre-rasterization
    // stage: through stream output, in primitive order, with the counters carrying the buffer
    // offsets across draws. Storage buffer writes from the vertex shader are the fallback, for an
    // older spirv_to_dxil.dll or boot.cfg "xfb=storage"; they cover the vertex shader alone.
    const bool has_geometry_stage{(key.unique_hashes[4] != 0 || layer_source_program != nullptr) &&
                                  !drops_geometry};
    const size_t xfb_program = has_geometry_stage          ? 4
                               : key.unique_hashes[3] != 0 ? 3
                                                           : 1;
    const bool captures_xfb{key.state.xfb_enabled != 0};
    // The console's hull and domain stages emit nothing, so tessellation always runs through
    // compute (tessellation_emulation.h), whose capture takes the stream output.
    const bool emulates_tess{key.unique_hashes[3] != 0};
    if (emulates_tess && !compiler.SupportsStreamOutput()) {
        throw std::runtime_error("emulated tessellation needs stream output");
    }
    const bool native_xfb{captures_xfb && !emulates_tess && compiler.SupportsStreamOutput() &&
                          !transform_feedback_storage.load(std::memory_order_relaxed)};
    VideoCommon::TransformFeedbackVaryings xfb_varyings;
    if (captures_xfb) {
        xfb_varyings = VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
        if (emulates_tess) {
            LOG_WARNING(Render, "D3D12: transform feedback after emulated tessellation is not "
                                "written");
        } else if (!native_xfb && xfb_program != 1) {
            LOG_WARNING(Render, "D3D12: transform feedback after tessellation or geometry "
                                "shaders needs stream output; it is not written");
        }
    }

    const auto vertex_texture_handle = FindVertexTextureHandle(programs, key);
    const PointGeometry point_geometry = PlanPointGeometry(
        programs, key, drops_geometry || captures_xfb || layer_source_program != nullptr);
    const bool generates_tess_control{key.unique_hashes[2] == 0 && key.unique_hashes[3] != 0};
    if (key.unique_hashes[1] != 0) {
        ApplyVertexFetchFixups(programs[1], key);
    }
    if (key.state.dynamic_state.shader_mirror_border != 0 && key.unique_hashes[5] != 0) {
        Shader::Optimization::MirrorBorderPass(programs[5]);
    }
    if (EmulatesLogicOp(key.state) && key.unique_hashes[5] != 0) {
        programs[5].info.uses_destination_read =
            LogicOpReadsDestination(key.state.dynamic_state.logic_op);
    }
    if (key.state.dynamic_state.shader_polygon_stipple != 0 && key.unique_hashes[5] != 0) {
        programs[5].info.uses_polygon_stipple = true;
        programs[5].info.uses_stage_constants = true;
    }
    const PrimitiveGeometry primitive_geometry = PlanPrimitiveGeometry(
        programs, key,
        point_geometry.enabled || drops_geometry || captures_xfb ||
            layer_source_program != nullptr);
    const bool generates_geometry{point_geometry.enabled ||
                                  primitive_geometry.kind != PrimitiveGeometry::Kind::None};
    const bool has_any_geometry{has_geometry_stage || generates_geometry};

    // Series corrupts native NoPerspective coordinates when clipping volumes
    // cross W=0. Preserve screen-linear semantics via smooth(a*clipW)/clipW.
    // Only direct VS -> FS is covered here: intermediate stages/indexed loads
    // need their own output/input handling, and must retain the original route.
    u32 lower_noperspective_mask{};
    if (key.unique_hashes[1] && key.unique_hashes[5] &&
        !key.unique_hashes[2] && !key.unique_hashes[3] && !key.unique_hashes[4] &&
        layer_source_program == nullptr && !generates_geometry &&
        !programs[5].info.loads_indexed_attributes) {
        const auto& vertex_info = programs[1].info;
        const auto& fragment_info = programs[5].info;
        for (u32 index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
            // Stream output reads the varying itself: a captured one keeps native NoPerspective.
            if (fragment_info.interpolation[index] == Shader::Interpolation::NoPerspective &&
                fragment_info.loads.Generic(index) && vertex_info.stores.Generic(index) &&
                !(native_xfb && xfb_varyings.CapturesGeneric(index))) {
                lower_noperspective_mask |= 1U << index;
            }
        }
    }
    if (dumps_dxil && lower_noperspective_mask != 0) {
        LOG_INFO(Render, "D3D12: NoPerspective lowering VS={:016x} PS={:016x} mask={:08x}",
                 key.unique_hashes[1], key.unique_hashes[5], lower_noperspective_mask);
    }

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    GraphicsPipeline::FragmentHelpers fragment_helpers{};
    TessLayout tess_layout;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        const bool is_generated_stage =
            (generates_tess_control &&
             index == static_cast<u32>(Maxwell::ShaderType::TessellationInit)) ||
            (generates_geometry && index == static_cast<u32>(Maxwell::ShaderType::Geometry));
        if (key.unique_hashes[index] == 0 && !is_emulated_stage && !is_generated_stage) {
            continue;
        }
        if (drops_geometry && index == static_cast<u32>(Maxwell::ShaderType::Geometry))
            [[unlikely]] {
            LOG_INFO(Render, "D3D12: VS {:016x} built without its GS {:016x}",
                     key.unique_hashes[1], key.unique_hashes[4]);
            continue;
        }
        if (index == 0) {
            BUG_TRACK(Unimplemented, "VertexA without VertexB (VS A {:016x})", key.unique_hashes[0]);
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_ERROR(Render, "D3D12: VertexA without VertexB is not implemented");
            return nullptr;
        }
        if (is_generated_stage) {
            // Here and not with the guest stages: they read what the vertex program ended up
            // writing, legacy varyings already mapped to generics.
            using Kind = PrimitiveGeometry::Kind;
            if (index != static_cast<u32>(Maxwell::ShaderType::Geometry)) {
                programs[index] = Shader::Maxwell::GenerateTessellationControl(
                    pools.inst, pools.block, *previous_stage, programs[index + 1],
                    key.state.patch_control_points_minus_one + 1);
            } else if (primitive_geometry.kind == Kind::Lines) {
                programs[index] = Shader::Maxwell::GenerateLineGeometry(
                    pools.inst, pools.block, *previous_stage,
                    primitive_geometry.stipple_generic
                        ? std::optional{Shader::IR::Attribute::Generic0X +
                                        *primitive_geometry.stipple_generic * 4}
                        : std::nullopt);
            } else if (primitive_geometry.kind == Kind::PolygonPoints) {
                programs[index] = Shader::Maxwell::GeneratePolygonPointGeometry(
                    pools.inst, pools.block, *previous_stage,
                    std::bit_cast<float>(key.state.point_size));
            } else if (primitive_geometry.kind == Kind::FillRectangle) {
                programs[index] = Shader::Maxwell::GenerateFillRectangleGeometry(
                    pools.inst, pools.block, *previous_stage, programs[5],
                    key.state.provoking_vertex_last != 0 ? 2 : 0);
            } else if (primitive_geometry.kind == Kind::ProvokingLast) {
                programs[index] = Shader::Maxwell::GenerateProvokingLastGeometry(
                    pools.inst, pools.block, *previous_stage,
                    MaxwellToD3D12::PrimitiveTopologyType(key.state.topology) ==
                            D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                        ? 2
                        : 3);
            } else {
                programs[index] = Shader::Maxwell::GeneratePointGeometry(
                    pools.inst, pools.block, *previous_stage, point_geometry.size_attr,
                    point_geometry.coord_attr, std::bit_cast<float>(key.state.point_size));
            }
        }
        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        auto runtime_info{
            MakeRuntimeInfo(programs, key, program, previous_stage, generates_geometry)};
        runtime_info.lower_noperspective_mask = lower_noperspective_mask;
        if (index == (has_any_geometry ? 4U : key.unique_hashes[3] != 0 ? 3U : 1U)) {
            runtime_info.viewport_swizzle = EmulatedViewportSwizzle(key.state.viewport_swizzles[0]);
        }
        if (program.stage == Shader::Stage::Fragment) {
            if (primitive_geometry.stipple_generic) {
                runtime_info.line_stipple_generic = *primitive_geometry.stipple_generic + 1;
                // The register holds the repeat count minus one.
                runtime_info.line_stipple_factor = key.state.line_stipple_factor + 1;
                runtime_info.line_stipple_pattern = key.state.line_stipple_pattern;
            }
            runtime_info.polygon_stipple = key.state.dynamic_state.shader_polygon_stipple != 0;
        }
        if (captures_xfb && index == xfb_program && (native_xfb || index == 1)) {
            xfb_varyings.Apply(runtime_info);
            runtime_info.xfb_storage = !native_xfb;
            runtime_info.xfb_copy_outputs = native_xfb;
        }
        ConvertLegacyToGeneric(program, runtime_info);
        if (emulates_tess) {
            tess_layout.Apply(programs, key, program, generates_tess_control, has_any_geometry,
                              runtime_info);
        }
        Shader::Optimization::DemoteReturnVerdict demote_return{};
        const auto merged_demote_returns =
            Shader::Optimization::DemoteReturnPass(program, &demote_return);
        if (merged_demote_returns && IsDumpedShader(key.unique_hashes[index])) {
            LOG_INFO(Render, "D3D12 helper-preserving discard: PS={:016x} terminal_returns={} (compile-time)",
                     key.unique_hashes[index], merged_demote_returns);
        }
        if (program.stage == Shader::Stage::Fragment) {
            fragment_helpers = {demote_return, program.info.uses_quad_shuffles,
                                program.info.uses_derivatives};
            // A terminal discard the pass cannot rewrite stops its lanes before the derivatives
            // of their quad neighbours: wrong mips or normals along triangle edges.
            if (Shader::Optimization::IsRejected(demote_return)) {
                BUG_TRACK_KEY(UnsupportedState, key.unique_hashes[index],
                              "PS {:016x} keeps a terminal discard return ({}): helper lanes "
                              "end before derivatives (quad_shuffles={} derivatives={})",
                              key.unique_hashes[index],
                              Shader::Optimization::Name(demote_return),
                              program.info.uses_quad_shuffles, program.info.uses_derivatives);
            }
        }
        spirv[stage_index] = EmitSPIRV(profile, runtime_info, program, binding);
        if (IsDumpedShader(key.unique_hashes[index])) [[unlikely]] {
            DumpShader(key.unique_hashes[index], stage_index, program, spirv[stage_index]);
        }
        stage_indices.push_back(stage_index);
        previous_stage = &program;
    }
    // Disk-cache keys may carry the bit from a session whose DLL emulated reduction.
    const bool emulate_reduction =
        (key.shader_flags & GraphicsPipelineCacheKey::SHADER_REDUCTION) != 0 &&
        compiler.EmulatesReduction();
    const PipelineLayout& layout =
        root_signatures.Get(infos, false, native_xfb || emulates_tess, emulate_reduction,
                            PlanBlendConstants(key.state).InShader(), emulates_tess);
    const TessEmulationPlan tess_plan = emulates_tess ? tess_layout.plan : TessEmulationPlan{};

    // SPIR-V to DXIL goes with the PSO build, on a worker: done here it stalled the GPU thread for
    // 150-490 ms per 300 frames when entering new areas (0.2.59 profile). Translating the guest
    // shaders stays here, since it reads guest memory through the environments.
    auto compile_dxil = [this, spirv = std::move(spirv), stage_indices, dumps_dxil,
                         vertex_shader = key.unique_hashes[1],
                         fragment_shader = key.unique_hashes[5],
                         pipeline_key = key.Hash(), native_xfb, emulates_tess,
                         emulate_reduction](u64 trace_pipeline, bool flatten_vertex) {
        boost::container::static_vector<ShaderCompiler::PipelineStage, Maxwell::MaxShaderStage>
            stages;
        // The stage_indices entry of each stage, where its DXIL goes.
        boost::container::static_vector<size_t, Maxwell::MaxShaderStage> dxil_indices;
        for (const size_t stage_index : stage_indices) {
            if (emulates_tess && stage_index < 2) {
                // The vertex and control shaders run before, on their own (TessEmulationPlan).
                continue;
            }
            // Stage indices 0-4 (VS, TCS, TES, GS, FS) match dxil_spirv_shader_stage's values. An
            // emulated evaluation shader is the vertex stage, and its DXIL goes in that slot.
            const bool eval_as_vertex = emulates_tess && stage_index == 2;
            stages.push_back({.spirv = spirv[stage_index],
                              .stage = eval_as_vertex
                                           ? DXIL_SPIRV_SHADER_VERTEX
                                           : static_cast<dxil_spirv_shader_stage>(stage_index)});
            dxil_indices.push_back(eval_as_vertex ? 0 : stage_index);
        }
        // The guest viewport transform decides the flips per draw, through the runtime data
        // (RasterizerD3D12::UpdateViewports).
        const ShaderCompiler::PipelineOptions options{
            .yz_flip = DXIL_SPIRV_YZ_FLIP_CONDITIONAL,
            .first_vertex_and_base_instance = DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA,
            .flatten_vertex = flatten_vertex,
            .emulate_reduction = emulate_reduction,
        };
        boost::container::static_vector<LinkedShaderStage, Maxwell::MaxShaderStage> key_stages;
        for (const auto& stage : stages) {
            key_stages.push_back({stage.spirv, static_cast<u32>(stage.stage)});
        }
        if (emulates_tess) {
            // The capture and control shaders are not in the stage list: the key holds them.
            key_stages.push_back({spirv[0], static_cast<u32>(DXIL_SPIRV_SHADER_VERTEX)});
            key_stages.push_back({spirv[1], static_cast<u32>(DXIL_SPIRV_SHADER_COMPUTE)});
        }
        const std::array<u32, 8> key_options{
            static_cast<u32>(options.yz_flip), options.y_flip_mask, options.z_flip_mask,
            static_cast<u32>(options.first_vertex_and_base_instance),
            static_cast<u32>(options.flatten_vertex), static_cast<u32>(native_xfb),
            static_cast<u32>(options.emulate_reduction), static_cast<u32>(emulates_tess)};
        auto key = MakeLinkedShaderKey(std::span(key_stages.data(), key_stages.size()), key_options);
        const auto cached = linked_shaders.Get(std::move(key), [&] {
            std::unique_ptr<eden_spirv_to_dxil_stream_output> stream_output;
            ShaderCompiler::PipelineOptions linked_options = options;
            if (native_xfb) {
                stream_output = std::make_unique<eden_spirv_to_dxil_stream_output>();
                linked_options.stream_output = stream_output.get();
            }
            ChargedDxilStages dxil;
            // The disk cache records plain stage lists only.
            const bool disk_cached = !native_xfb && !emulates_tess;
            auto compiled = compiler.CompilePipeline(std::span(stages.data(), stages.size()),
                                                     linked_options, trace_pipeline,
                                                     disk_cached ? &dxil.linked.disk_key : nullptr);
            u64 bytes = 0;
            for (size_t i = 0; i < compiled.size(); ++i) {
                bytes += compiled[i].dxil.capacity();
                dxil.linked.stages[dxil_indices[i]] = std::move(compiled[i].dxil);
            }
            if (emulates_tess) {
                bytes += CompileTessEmulation(compiler, spirv, emulate_reduction, trace_pipeline,
                                              dxil.linked);
            }
            if (stream_output) {
                dxil.linked.stream_output = MakeStreamOutputDeclaration(*stream_output);
                bytes += dxil.linked.stream_output.capacity() * sizeof(D3D12_SO_DECLARATION_ENTRY);
                if (stream_output->missing_outputs != 0 || stream_output->dropped_entries != 0) {
                    BUG_TRACK_KEY(UnsupportedState, pipeline_key,
                                  "VS {:016x} stream output leaves {} captured outputs unwritten "
                                  "and drops {} entries",
                                  vertex_shader, stream_output->missing_outputs,
                                  stream_output->dropped_entries);
                }
            }
            dxil.linked.PreparePsoHashes();
            dxil.charge = Common::MemoryCharge(Common::MemoryAccount::ShaderBytecode, bytes);
            return dxil;
        }, [](const ChargedDxilStages& dxil) {
            size_t bytes = sizeof(dxil);
            for (const auto& stage : dxil.linked.stages) {
                bytes += stage.capacity();
            }
            return bytes + dxil.linked.tess_capture.capacity() + dxil.linked.tess_control.capacity() +
                   dxil.linked.stream_output.capacity() * sizeof(D3D12_SO_DECLARATION_ENTRY);
        });
        // Export the actual linked bytes even when another pipeline populated this cache entry.
        if (dumps_dxil) [[unlikely]] {
            const auto& linked = cached.value->linked;
            DumpDxil(vertex_shader, fragment_shader, pipeline_key, linked.stages);
            if (emulates_tess) {
                // After the five stages: 6 the vertex capture, 7 the control as compute.
                const std::array<std::vector<u8>, 8> tess{
                    {{}, {}, {}, {}, {}, {}, linked.tess_capture, linked.tess_control}};
                DumpDxil(vertex_shader, fragment_shader, pipeline_key, tess);
            }
        }
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::PipelineCacheResult,
                                   static_cast<u64>(cached.outcome), trace_pipeline);
        // The pipeline keeps the whole entry, charge included, alive through its stages.
        return GraphicsPipeline::SharedDxilStages(cached.value, &cached.value->linked);
    };

    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    auto pipeline = std::make_unique<GraphicsPipeline>(device, texture_runtime, &shader_notify,
                                                       thread_worker, key, std::move(compile_dxil),
                                                       infos, layout, build_pso, &graphics_psos,
                                                       lower_noperspective_mask, tess_plan);
    pipeline->SetFragmentHelpers(fragment_helpers);
    pipeline->SetGeneratesGeometry(generates_geometry);
    pipeline->SetVertexTextureHandle(vertex_texture_handle);
    return pipeline;

} catch (const Shader::Exception& exception) {
    BUG_TRACK_KEY(ShaderCompile, key.unique_hashes[1] ^ std::rotl(key.unique_hashes[5], 1),
                  "recompiling VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
                  key.unique_hashes[5], exception.what());
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "D3D12: recompiling VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
              key.unique_hashes[5], exception.what());
    LogShaderSources(envs, key.unique_hashes[1], key.unique_hashes[5]);
    return nullptr;
} catch (const std::exception& exception) {
    BUG_TRACK_KEY(ShaderCompile, key.unique_hashes[1] ^ std::rotl(key.unique_hashes[5], 1),
                  "building the pipeline for VS {:016x} PS {:016x} failed: {}",
                  key.unique_hashes[1], key.unique_hashes[5], exception.what());
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "D3D12: building the pipeline for VS {:016x} PS {:016x} failed: {}",
              key.unique_hashes[1], key.unique_hashes[5], exception.what());
    return nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    const GraphicsPipelineCacheKey& key) {
    VideoCore::FrameTrace::ScopedSpan frontend_span{
        VideoCore::FrameTrace::Event::PipelineFrontendLong, key.Hash()};
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, key.unique_hashes);

    main_pools.ReleaseContents();
    auto pipeline{CreateGraphicsPipeline(main_pools, key, environments.Span(), true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key, envs = std::move(environments.envs)] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    const ComputePipelineCacheKey& key, const VideoCommon::ShaderInfo* shader,
    bool emulate_reduction, u32 workgroup_split) {
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start};
    env.SetCachedSize(shader->size_bytes);

    main_pools.ReleaseContents();
    auto pipeline{
        CreateComputePipeline(main_pools, key, env, true, emulate_reduction, workgroup_split)};
    // Variants are not stored: the disk cache rebuilds the plain pipeline, and the first dispatch
    // that needs a variant builds it again.
    if (!pipeline || pipeline_cache_filename.empty() || emulate_reduction ||
        workgroup_split > 1) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    bool build_in_parallel, bool emulate_reduction, u32 workgroup_split) try {
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
    if (Settings::values.dump_guest_shaders) {
        env.Dump(key.Hash(), key.unique_hash);
    }
    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    if (program.info.global_memory_fallbacks != 0 || program.info.global_memory_unresolved != 0) {
        LOG_WARNING(Render,
                    "D3D12: compute shader {:016x} uses global memory fallbacks: {} untracked, "
                    "{} unresolved, stores {}",
                    key.unique_hash, program.info.global_memory_fallbacks,
                    program.info.global_memory_unresolved, program.info.stores_global_memory);
    }
    u32 split{1};
    if (workgroup_split > 1) {
        using Shader::Optimization::WorkgroupSplitVerdict;
        const WorkgroupSplitVerdict verdict{
            Shader::Optimization::WorkgroupSplitPass(program, workgroup_split)};
        if (verdict == WorkgroupSplitVerdict::Split) {
            split = workgroup_split;
            LOG_INFO(Render, "D3D12: compute shader {:016x} split: each group of {} runs as {}",
                     key.unique_hash, key.workgroup_size[0], split);
        } else {
            LOG_INFO(Render, "D3D12: compute shader {:016x} not split ({})", key.unique_hash,
                     Shader::Optimization::Name(verdict));
            // Building it would repeat the plain pipeline: 130-190 ms in the driver, with the
            // recorder waiting for it. The caller takes the plain one instead.
            compute_not_split.insert(key.Hash());
            return nullptr;
        }
    }
    std::vector<u32> code{EmitSPIRV(profile, program)};
    const bool dumped{IsDumpedShader(key.unique_hash)};
    if (dumped) [[unlikely]] {
        DumpShader(key.unique_hash, 0, program, code);
    }

    // SPIR-V to DXIL goes with the PSO build, on a worker, as for graphics pipelines: the GPU
    // thread only translates the guest shader, since that reads guest memory.
    auto compile_dxil = [this, code = std::move(code), dumped, emulate_reduction,
                         unique_hash = key.unique_hash, key_hash = key.Hash()] {
        const std::array<ShaderCompiler::PipelineStage, 1> stages{{
            {.spirv = code, .stage = DXIL_SPIRV_SHADER_COMPUTE},
        }};
        auto compiled = compiler.CompilePipeline(stages, {.emulate_reduction = emulate_reduction});
        if (dumped) [[unlikely]] {
            // The emulating build overwrites the plain one: the DXIL that runs is the last dumped.
            DumpDxil(0, unique_hash, key_hash, std::span{&compiled[0].dxil, 1});
        }
        return std::move(compiled[0].dxil);
    };
    const std::array<const Shader::Info*, 1> infos{&program.info};
    const PipelineLayout& layout = root_signatures.Get(infos, true, false, emulate_reduction);

    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<ComputePipeline>(device, &shader_notify, thread_worker,
                                             key.unique_hash, std::move(compile_dxil),
                                             program.info, layout, emulate_reduction, split);

} catch (const Shader::Exception& exception) {
    BUG_TRACK_KEY(ShaderCompile, key.unique_hash, "recompiling compute {:016x} failed: {}",
                  key.unique_hash, exception.what());
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "D3D12: recompiling compute {:016x} failed: {}", key.unique_hash,
              exception.what());
    Shader::Environment* const compute_env[]{&env};
    LogShaderSources(compute_env, key.unique_hash, 0);
    return nullptr;
} catch (const std::exception& exception) {
    BUG_TRACK_KEY(ShaderCompile, key.unique_hash, "building compute pipeline {:016x} failed: {}",
                  key.unique_hash, exception.what());
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "D3D12: building compute pipeline {:016x} failed: {}", key.unique_hash,
              exception.what());
    return nullptr;
}

} // namespace D3D12
