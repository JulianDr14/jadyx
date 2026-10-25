// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
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
#include "shader_recompiler/ir_opt/workgroup_split_pass.h"
#include "shader_recompiler/program_header.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/frame_trace.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_local_memory_scratch.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_stream_output.h"
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
std::atomic<u32> compute_workgroup_split{1};

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

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
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
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].UsesNativeGeometryPassthrough()};
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
    case Shader::Stage::TessellationEval:
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
/// the console's game partition has 5 GB in total.
size_t GetTotalPipelineWorkers() {
    const size_t threads = std::max<size_t>(std::thread::hardware_concurrency(), 2) - 1;
    return std::min<size_t>(threads, 4);
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
        // Quad ops (QuadReadAcross*, QuadReadLaneAt) include helper lanes; wave shuffles do not
        // before SM 6.7, so manual derivatives (SHFL + FSWZADD) read garbage at triangle edges.
        .support_quad_shuffles = caps.wave_ops,
        .support_vote = caps.wave_ops,
        .supported_subgroup_stages = caps.wave_ops ? 0x7Fu : 0u,
        .support_viewport_index_layer_non_geometry = caps.vp_and_rt_index_from_any_stage,
        .support_viewport_mask = false,
        .support_typeless_image_loads = caps.typed_uav_load_additional_formats,
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
        .has_broken_robust = false,
        // D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT: raw views start on 16 bytes.
        .min_ssbo_alignment = 16,
        .max_user_clip_distances = 8,
    };
    profile.fragment_local_memory_scratch_lanes =
        SelectFragmentScratchLanes(device, compiler, caps.wave_ops, caps.wave_lane_count_max);
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

void PipelineCache::TickResidency(const CacheMemorySnapshot& snapshot, Scheduler& scheduler) {
    using Policy = PipelineResidencyPolicy;
    ++residency_frame;
    const u64 idle_frames = Policy::IdleFrames(snapshot.app_used, snapshot.app_limit);
    const bool evict = pipeline_eviction.load(std::memory_order_relaxed) ||
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
        LOG_INFO(Render,
                 "D3D12: {} graphics pipelines, {} PSOs resident; {} released and {} rebuilt so "
                 "far; {} drawn in recent sessions",
                 residency_sweep.size(), resident.objects, pipelines_evicted, pipelines_woken,
                 hot_pipelines.Size());
        LogPipelineStats("session");
    }
}

void PipelineCache::TickPrewarm(const CacheMemorySnapshot& snapshot) {
    if (prewarm_cursor >= prewarm_order.size() || (prewarm_last && !prewarm_last->IsBuilt()) ||
        !pipeline_prewarm.load(std::memory_order_relaxed) ||
        !PipelineResidencyPolicy::MayPrewarm(snapshot.app_used, snapshot.app_limit)) {
        return;
    }
    prewarm_last = nullptr;
    while (prewarm_cursor < prewarm_order.size()) {
        GraphicsPipeline* const pipeline = prewarm_order[prewarm_cursor++];
        // Pipelines drawn meanwhile were woken by their draw and are no longer dormant; one
        // released while idle since is left to its next draw.
        if (!pipeline->WasEvicted() && pipeline->Wake(prewarm_thread)) {
            // Counted as used now, so eviction does not take it back before its first draw.
            pipeline->Touch(residency_frame);
            pipeline->MarkPrewarmed();
            prewarm_last = pipeline;
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
    }
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
            {PipelineStage::PsoCreate, "pso"},
            {PipelineStage::WaitNew, "wait-new"},
            {PipelineStage::WaitDormant, "wait-dormant"},
            {PipelineStage::WaitEvicted, "wait-evicted"},
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
    graphics_key.emulate_reduction = UsesReductionShader(graphics_key.unique_hashes) ? 1 : 0;

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
    const u32 workgroup_split = WorkgroupSplitFor(*kepler_compute);
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
    if (!pipeline) {
        BUG_TRACK_KEY(DrawSkipped, key.unique_hash,
                      "dispatches skipped: no pipeline for compute {:016x} (shader build failed)",
                      key.unique_hash);
        return nullptr;
    }
    if (!pipeline->IsBuilt()) {
        // Compute always waits, even with asynchronous shaders: a skipped dispatch leaves the
        // buffers or images it writes stale for the rest of the frame and beyond.
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs,
                                           VideoCore::Perf::Counter::PipelineStalls};
        VideoCore::Perf::ScopedTimer wait{VideoCore::Perf::Counter::PipelineWaitUs};
        pipeline->WaitBuilt();
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

    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        workers.QueueWork([this, key, env_ = std::move(env), &state, &callback]() mutable {
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
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        // Every pipeline gets its DXIL; only the hot ones a PSO, the rest when first drawn.
        const size_t file_index = state.graphics++;
        const bool build_pso = hot_pipelines.Prewarm(key.Hash(), file_index);
        state.prewarmed += build_pso ? 1 : 0;

        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback, build_pso,
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

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);
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
        }
    }
    // The background prewarm's order: drawn in a kept session but past the boot cap first, then
    // the rest in the order the game first drew them, which is roughly the order it goes through
    // its areas. Pipelines whose DXIL failed are Ready, not dormant, and are skipped when reached.
    std::ranges::sort(state.dormant, [this](const auto& a, const auto& b) {
        const bool known_a = hot_pipelines.Known(a.second->KeyHash());
        const bool known_b = hot_pipelines.Known(b.second->KeyHash());
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
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        // Translating the shaders runs here, on the GPU thread, before the build is queued.
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs};
        VideoCore::Perf::ScopedTimer frontend{VideoCore::Perf::Counter::PipelineFrontendUs};
        const u64 frontend_start = SteadyMicros();
        pipeline = CreateGraphicsPipeline();
        GetPipelineStats().Record(PipelineStage::Frontend, SteadyMicros() - frontend_start);
        if (pipeline) {
            residency_sweep.push_back(pipeline.get());
        }
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
        current_pipeline->AddTransition(pipeline.get());
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
    // Its PSO was never built (not drawn in recent sessions) or was released while idle. Queued
    // like any build, but WaitBuilt below claims it unless a worker already started it.
    if (pipeline->Wake(workers)) {
        ++pipelines_woken;
    }
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
    return pipeline;
}

namespace {
// A shader that fails to build: whether its code came from the disk cache or from guest memory,
// and from where in guest memory.
void LogShaderSources(std::span<Shader::Environment* const> envs) {
    for (size_t i = 0; i < envs.size(); ++i) {
        LOG_ERROR(Render, "D3D12: failed shader stage {}: {}", i, envs[i]->DescribeCode());
    }
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
    const bool native_xfb{captures_xfb && compiler.SupportsStreamOutput() &&
                          !transform_feedback_storage.load(std::memory_order_relaxed)};
    VideoCommon::TransformFeedbackVaryings xfb_varyings;
    if (captures_xfb) {
        xfb_varyings = VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
        if (!native_xfb && xfb_program != 1) {
            LOG_WARNING(Render, "D3D12: transform feedback after tessellation or geometry "
                                "shaders needs stream output; it is not written");
        }
    }

    // Series corrupts native NoPerspective coordinates when clipping volumes
    // cross W=0. Preserve screen-linear semantics via smooth(a*clipW)/clipW.
    // Only direct VS -> FS is covered here: intermediate stages/indexed loads
    // need their own output/input handling, and must retain the original route.
    u32 lower_noperspective_mask{};
    if (key.unique_hashes[1] && key.unique_hashes[5] &&
        !key.unique_hashes[2] && !key.unique_hashes[3] && !key.unique_hashes[4] &&
        layer_source_program == nullptr && !programs[5].info.loads_indexed_attributes) {
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
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
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
        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage)};
        runtime_info.lower_noperspective_mask = lower_noperspective_mask;
        if (captures_xfb && index == xfb_program && (native_xfb || index == 1)) {
            xfb_varyings.Apply(runtime_info);
            runtime_info.xfb_storage = !native_xfb;
            runtime_info.xfb_copy_outputs = native_xfb;
        }
        ConvertLegacyToGeneric(program, runtime_info);
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
    const bool emulate_reduction = key.emulate_reduction != 0 && compiler.EmulatesReduction();
    const PipelineLayout& layout =
        root_signatures.Get(infos, false, native_xfb, emulate_reduction);

    // SPIR-V to DXIL goes with the PSO build, on a worker: done here it stalled the GPU thread for
    // 150-490 ms per 300 frames when entering new areas (0.2.59 profile). Translating the guest
    // shaders stays here, since it reads guest memory through the environments.
    auto compile_dxil = [this, spirv = std::move(spirv), stage_indices, dumps_dxil,
                         vertex_shader = key.unique_hashes[1],
                         fragment_shader = key.unique_hashes[5],
                         pipeline_key = key.Hash(), native_xfb,
                         emulate_reduction](u64 trace_pipeline, bool flatten_vertex) {
        boost::container::static_vector<ShaderCompiler::PipelineStage, Maxwell::MaxShaderStage>
            stages;
        for (const size_t stage_index : stage_indices) {
            // Stage indices 0-4 (VS, TCS, TES, GS, FS) match dxil_spirv_shader_stage's values.
            stages.push_back({.spirv = spirv[stage_index],
                              .stage = static_cast<dxil_spirv_shader_stage>(stage_index)});
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
        const std::array<u32, 7> key_options{
            static_cast<u32>(options.yz_flip), options.y_flip_mask, options.z_flip_mask,
            static_cast<u32>(options.first_vertex_and_base_instance),
            static_cast<u32>(options.flatten_vertex), static_cast<u32>(native_xfb),
            static_cast<u32>(options.emulate_reduction)};
        auto key = MakeLinkedShaderKey(std::span(key_stages.data(), key_stages.size()), key_options);
        const auto cached = linked_shaders.Get(std::move(key), [&] {
            std::unique_ptr<eden_spirv_to_dxil_stream_output> stream_output;
            ShaderCompiler::PipelineOptions linked_options = options;
            if (native_xfb) {
                stream_output = std::make_unique<eden_spirv_to_dxil_stream_output>();
                linked_options.stream_output = stream_output.get();
            }
            auto compiled = compiler.CompilePipeline(std::span(stages.data(), stages.size()),
                                                     linked_options, trace_pipeline);
            ChargedDxilStages dxil;
            u64 bytes = 0;
            for (size_t i = 0; i < compiled.size(); ++i) {
                bytes += compiled[i].dxil.capacity();
                dxil.linked.stages[stage_indices[i]] = std::move(compiled[i].dxil);
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
            dxil.charge = Common::MemoryCharge(Common::MemoryAccount::ShaderBytecode, bytes);
            return dxil;
        }, [](const ChargedDxilStages& dxil) {
            size_t bytes = sizeof(dxil);
            for (const auto& stage : dxil.linked.stages) {
                bytes += stage.capacity();
            }
            return bytes + dxil.linked.stream_output.capacity() * sizeof(D3D12_SO_DECLARATION_ENTRY);
        });
        // Export the actual linked bytes even when another pipeline populated this cache entry.
        if (dumps_dxil) [[unlikely]] {
            DumpDxil(vertex_shader, fragment_shader, pipeline_key, cached.value->linked.stages);
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
                                                       lower_noperspective_mask);
    pipeline->SetFragmentHelpers(fragment_helpers);
    return pipeline;

} catch (const Shader::Exception& exception) {
    BUG_TRACK_KEY(ShaderCompile, key.unique_hashes[1] ^ std::rotl(key.unique_hashes[5], 1),
                  "recompiling VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
                  key.unique_hashes[5], exception.what());
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "D3D12: recompiling VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
              key.unique_hashes[5], exception.what());
    LogShaderSources(envs);
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

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    VideoCore::FrameTrace::ScopedSpan frontend_span{
        VideoCore::FrameTrace::Event::PipelineFrontendLong, graphics_key.Hash()};
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);

    main_pools.ReleaseContents();
    auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key = graphics_key, envs = std::move(environments.envs)] {
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
        }
    }
    const std::vector<u32> code{EmitSPIRV(profile, program)};
    const bool dumped{IsDumpedShader(key.unique_hash)};
    if (dumped) [[unlikely]] {
        DumpShader(key.unique_hash, 0, program, code);
    }

    const std::array<ShaderCompiler::PipelineStage, 1> stages{{
        {.spirv = code, .stage = DXIL_SPIRV_SHADER_COMPUTE},
    }};
    auto compiled = compiler.CompilePipeline(stages, {.emulate_reduction = emulate_reduction});
    if (dumped) [[unlikely]] {
        // The emulating build overwrites the plain one: the DXIL that runs is the last dumped.
        DumpDxil(0, key.unique_hash, key.Hash(), std::span{&compiled[0].dxil, 1});
    }
    const std::array<const Shader::Info*, 1> infos{&program.info};
    const PipelineLayout& layout = root_signatures.Get(infos, true, false, emulate_reduction);

    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<ComputePipeline>(device, &shader_notify, thread_worker,
                                             key.unique_hash, std::move(compiled[0].dxil),
                                             program.info, layout, emulate_reduction, split);

} catch (const Shader::Exception& exception) {
    BUG_TRACK_KEY(ShaderCompile, key.unique_hash, "recompiling compute {:016x} failed: {}",
                  key.unique_hash, exception.what());
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "D3D12: recompiling compute {:016x} failed: {}", key.unique_hash,
              exception.what());
    Shader::Environment* const compute_env[]{&env};
    LogShaderSources(compute_env);
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
