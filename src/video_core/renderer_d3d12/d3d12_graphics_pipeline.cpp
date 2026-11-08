// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <optional>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/diagnostics/full_capture.h"
#include "video_core/gpu_thread.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "video_core/frame_trace.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_render_corrections.h"
#include "video_core/renderer_d3d12/d3d12_draw_targets.h"
#include "video_core/renderer_d3d12/d3d12_draw_metrics.h"
#include "video_core/renderer_d3d12/d3d12_driver_guard.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_texture_replay.h"
#include "video_core/renderer_d3d12/d3d12_local_memory_scratch.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_memory_reserve.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_helper.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"
#include "video_core/renderer_d3d12/d3d12_pso_blob_cache.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_stream_output.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_pipeline_diagnostics.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

#include <psapi.h>

namespace D3D12 {

namespace {
/// Log names of the pipeline stages, VertexA excluded.
constexpr std::array<const char*, GraphicsPipeline::NUM_STAGES> STAGE_NAMES{"VS", "HS", "DS", "GS",
                                                                      "PS"};

std::atomic_bool stage_view_cache{};
std::atomic_bool draw_sampler_reuse{};
std::atomic<TextureReplayMode> texture_replay_mode{TextureReplayMode::Off};
std::atomic<u32> texture_replay_logged{};

std::atomic<u64> dxil_released{};
std::atomic<u64> dxil_released_bytes{};
std::atomic<u64> dxil_reloaded{};
std::atomic<u64> dxil_reload_failed{};

/// DXIL read back from disk, charged like the linked shader cache's entries.
struct ReloadedDxil {
    GraphicsPipeline::LinkedDxil linked;
    Common::MemoryCharge charge;
};
}
void SetStageViewCache(bool enabled) {
    stage_view_cache.store(enabled, std::memory_order_relaxed);
}

struct GraphicsPipeline::StageViewState {
    VideoCommon::GraphicsBindingStamp stamp;
    std::array<bool, NUM_STAGES> valid{};
    std::array<std::vector<StageViewEntry>, NUM_STAGES> entries;
};

void SetTextureReplay(TextureReplayMode mode) {
    texture_replay_mode.store(mode, std::memory_order_relaxed);
}

constexpr size_t FRAGMENT_STAGE = 4;

/// The view's image as it is now, in a copy a draw can read while it writes the image.
/// The emulated logic op reads its destination through it.
[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE ColorSnapshotHandle(const ImageView& view,
                                                              Shader::TextureType type) {
    Image* const source = view.SourceImage();
    if (!source) {
        return view.Handle(type);
    }
    using SnapshotCopy = Image::SnapshotCopy;
    const SnapshotCopy copy = source->PrepareColorSnapshot(
        view.ReadsReinterpretedCopy(), view.range.base.level, view.range.extent.levels);
    switch (copy) {
    case SnapshotCopy::Failed:
        return view.Handle(type);
    case SnapshotCopy::Reused:
        VideoCore::Perf::Add(VideoCore::Perf::Counter::SnapshotReuses, 1);
        break;
    case SnapshotCopy::Partial:
        VideoCore::Perf::Add(VideoCore::Perf::Counter::LogicOpCopies, 1);
        VideoCore::Perf::Add(VideoCore::Perf::Counter::SnapshotLevelCopies, 1);
        break;
    case SnapshotCopy::Full:
        VideoCore::Perf::Add(VideoCore::Perf::Counter::LogicOpCopies, 1);
        VideoCore::Perf::Add(VideoCore::Perf::Counter::LogicOpCopyBytes, source->guest_size_bytes);
        break;
    }
    return view.DepthFeedbackHandle(type);
}

struct GraphicsPipeline::TextureReplayState {
    TextureReplayKey key;
    /// Per texture descriptor, in binding order: its SRV and whether PrepareRead has work.
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
    std::vector<u8> prepare;
    u8 unemulated_reduction_stages{};
    std::array<u32, 2> mirror_border_masks{};
    /// Not stored when a texture aliases a colour target, which this keeps true on replay.
    const Framebuffer* framebuffer{};
    IntegerSamplerTable integer_samplers;
    /// The rows' upload, bound again within the submission it was made for.
    u64 integer_tick{~u64{0}};
    D3D12_GPU_VIRTUAL_ADDRESS integer_address{};
};

void SetDrawSamplerReuse(bool enabled) {
    draw_sampler_reuse.store(enabled, std::memory_order_relaxed);
}
bool DrawSamplerReuseEnabled() {
    return draw_sampler_reuse.load(std::memory_order_relaxed);
}

namespace {

// pso_probe=1: what the driver commits for each graphics PSO. Creation is serialized and measured
// by the process's private commit around it; other threads' allocations land in some samples, so
// the median is the cost and the mean only a check.
std::atomic<bool> pso_probe_enabled{};
std::mutex pso_probe_mutex;
std::vector<s64> pso_probe_samples;

s64 PrivateCommit() {
    PROCESS_MEMORY_COUNTERS_EX counters{.cb = sizeof(counters)};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))) {
        return 0;
    }
    return static_cast<s64>(counters.PrivateUsage);
}

using Vulkan::FixedPipelineState;

constexpr size_t KEY_EXTRA_OFFSET = offsetof(GraphicsPipelineCacheKey, depth_bias);
constexpr size_t KEY_EXTRA_SIZE = sizeof(GraphicsPipelineCacheKey) - KEY_EXTRA_OFFSET;

/// Bytes of the key that count: Vulkan's state is only valid up to FixedPipelineState::Size().
size_t PrefixSize(const GraphicsPipelineCacheKey& key) {
    return sizeof(key.unique_hashes) + key.state.Size();
}

/// A generated line's stipple is compiled into its shaders, yet lies past the prefix unless the
/// pipeline captures transform feedback. Other keys ignore it, as it has no effect on them.
bool ComparesStippleApart(const GraphicsPipelineCacheKey& key) {
    const auto& dynamic = key.state.dynamic_state;
    return dynamic.generated_lines != 0 && dynamic.line_stipple_enable != 0 &&
           key.state.xfb_enabled == 0;
}

size_t NumAttachments(const FixedPipelineState& state) {
    size_t num{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
        const auto format{static_cast<Tegra::RenderTargetFormat>(state.color_formats[index])};
        if (format != Tegra::RenderTargetFormat::NONE) {
            num = index + 1;
        }
    }
    return num;
}

D3D12_SHADER_BYTECODE Bytecode(const std::vector<u8>& code) {
    return {.pShaderBytecode = code.empty() ? nullptr : code.data(),
            .BytecodeLength = code.size()};
}

Shader::BlendConstantFactor ShaderBlendFactor(Maxwell::Blend::Factor factor) {
    using Factor = Maxwell::Blend::Factor;
    using Shader::BlendConstantFactor;
    switch (factor) {
    case Factor::BlendFactor_D3D:
    case Factor::ConstantColor_GL:
        return BlendConstantFactor::Color;
    case Factor::OneMinusBlendFactor_D3D:
    case Factor::OneMinusConstantColor_GL:
        return BlendConstantFactor::OneMinusColor;
    case Factor::BothSourceAlpha_D3D:
    case Factor::ConstantAlpha_GL:
        return BlendConstantFactor::Alpha;
    case Factor::OneMinusBothSourceAlpha_D3D:
    case Factor::OneMinusConstantAlpha_GL:
        return BlendConstantFactor::OneMinusAlpha;
    default:
        return BlendConstantFactor::None;
    }
}

D3D12_DEPTH_STENCILOP_DESC StencilFace(const auto& face) {
    return {
        .StencilFailOp = MaxwellToD3D12::StencilOp(face.ActionStencilFail()),
        .StencilDepthFailOp = MaxwellToD3D12::StencilOp(face.ActionDepthFail()),
        .StencilPassOp = MaxwellToD3D12::StencilOp(face.ActionDepthPass()),
        .StencilFunc = MaxwellToD3D12::ComparisonFunc(face.TestFunc()),
    };
}

D3D12_RENDER_TARGET_BLEND_DESC BlendTarget(const FixedPipelineState::BlendingAttachment& blend,
                                           bool source_in_shader) {
    const std::array mask = blend.Mask();
    const UINT8 write_mask = static_cast<UINT8>(
        (mask[0] ? D3D12_COLOR_WRITE_ENABLE_RED : 0) |
        (mask[1] ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        (mask[2] ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) |
        (mask[3] ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    if (blend.enable == 0) {
        return {
            .BlendEnable = FALSE,
            .LogicOpEnable = FALSE,
            .SrcBlend = D3D12_BLEND_ONE,
            .DestBlend = D3D12_BLEND_ZERO,
            .BlendOp = D3D12_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D12_BLEND_ONE,
            .DestBlendAlpha = D3D12_BLEND_ZERO,
            .BlendOpAlpha = D3D12_BLEND_OP_ADD,
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask = write_mask,
        };
    }
    return {
        .BlendEnable = TRUE,
        .LogicOpEnable = FALSE,
        .SrcBlend = source_in_shader
                        ? D3D12_BLEND_ONE
                        : MaxwellToD3D12::BlendFactor(blend.SourceRGBFactor(), false),
        .DestBlend = MaxwellToD3D12::BlendFactor(blend.DestRGBFactor(), false),
        .BlendOp = MaxwellToD3D12::BlendOp(blend.EquationRGB()),
        .SrcBlendAlpha = MaxwellToD3D12::BlendFactor(blend.SourceAlphaFactor(), true),
        .DestBlendAlpha = MaxwellToD3D12::BlendFactor(blend.DestAlphaFactor(), true),
        .BlendOpAlpha = MaxwellToD3D12::BlendOp(blend.EquationAlpha()),
        .LogicOp = D3D12_LOGIC_OP_NOOP,
        .RenderTargetWriteMask = write_mask,
    };
}

/// Warns once per process about state D3D12 cannot express; the bug tracker counts every
/// occurrence (keyed by the flag, one per feature).
void WarnOnce(std::atomic_bool& flag, const char* what) {
    BUG_TRACK_KEY(UnsupportedState, reinterpret_cast<std::uintptr_t>(&flag),
                  "{} is not supported and is ignored", what);
    WarnOnceLog(flag, "{} is not supported and is ignored", what);
}
std::atomic_bool warned_logic_op;
std::atomic_bool warned_depth_bounds;
std::atomic_bool warned_point_fill;
std::atomic_bool warned_vertex_format;
std::atomic_bool warned_depth_feedback;
std::atomic_bool warned_conservative;

} // Anonymous namespace

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 prefix = Common::CityHash64(reinterpret_cast<const char*>(this), PrefixSize(*this));
    const u64 extra = Common::CityHash64(reinterpret_cast<const char*>(this) + KEY_EXTRA_OFFSET,
                                         KEY_EXTRA_SIZE);
    const u64 hash = prefix ^ (extra + 0x9e3779b97f4a7c15ULL + (prefix << 6) + (prefix >> 2));
    if (ComparesStippleApart(*this)) {
        const u64 stipple = (static_cast<u64>(state.line_stipple_factor) << 32) |
                            state.line_stipple_pattern;
        return static_cast<size_t>(hash ^ (stipple + 0x9e3779b97f4a7c15ULL + (hash << 6) +
                                           (hash >> 2)));
    }
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    const size_t prefix = PrefixSize(*this);
    return prefix == PrefixSize(rhs) && std::memcmp(this, &rhs, prefix) == 0 &&
           std::memcmp(reinterpret_cast<const char*>(this) + KEY_EXTRA_OFFSET,
                       reinterpret_cast<const char*>(&rhs) + KEY_EXTRA_OFFSET,
                       KEY_EXTRA_SIZE) == 0 &&
           (!ComparesStippleApart(*this) ||
            (state.line_stipple_factor == rhs.state.line_stipple_factor &&
             state.line_stipple_pattern == rhs.state.line_stipple_pattern));
}

void GraphicsPipelineCacheKey::RefreshStatic(const Maxwell& regs) noexcept {
    const auto& dynamic = state.dynamic_state;
    const bool bias = dynamic.depth_bias_enable != 0;
    depth_bias = bias ? std::bit_cast<u32>(regs.depth_bias) : 0;
    depth_bias_clamp = bias ? std::bit_cast<u32>(regs.depth_bias_clamp) : 0;
    slope_scaled_depth_bias = bias ? std::bit_cast<u32>(regs.slope_scale_depth_bias) : 0;
    // A fragment shader that adds the bias keeps the fractional constant D3D12 truncates; it
    // writes its depth, so the guest's early depth test cannot run with it.
    const bool shader_bias = bias && GetShaderDepthBias() && state.depth_enabled != 0 &&
                             state.early_z == 0 && unique_hashes[5] != 0 &&
                             MaxwellToD3D12::SampleCount(state.msaa_mode) == 1 &&
                             (regs.depth_bias != 0.0f || regs.slope_scale_depth_bias != 0.0f);
    if (bias && GetDepthBiasRounding()) {
        if (!shader_bias) {
            depth_bias = std::bit_cast<u32>(EffectiveDepthBias(regs.depth_bias));
        }
        depth_bias_clamp = std::bit_cast<u32>(RoundDepthBiasStep(regs.depth_bias_clamp));
        slope_scaled_depth_bias =
            std::bit_cast<u32>(RoundDepthBiasStep(regs.slope_scale_depth_bias));
    }
    const bool stencil = dynamic.stencil_enable != 0;
    stencil_read_mask = stencil ? static_cast<u8>(regs.stencil_front_func_mask) : 0;
    stencil_write_mask = stencil ? static_cast<u8>(regs.stencil_front_mask) : 0;
    strip_cut = 0;
    if (dynamic.primitive_restart_enable != 0) {
        strip_cut = static_cast<u8>(regs.index_buffer.format == Maxwell::IndexFormat::UnsignedInt
                                        ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                        : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    }
    shader_flags = 0;
    // A single sample has no coverage to dither: the shader discards with the guest's footprint.
    if (state.alpha_to_coverage_enabled != 0 && GetShaderAlphaToCoverage() &&
        MaxwellToD3D12::SampleCount(state.msaa_mode) == 1) {
        const u32 footprint = static_cast<u32>(regs.alpha_to_coverage_dither) + 1;
        shader_flags |= static_cast<u8>(std::min(footprint, 3U) << SHADER_A2C_SHIFT);
    }
    if (shader_bias) {
        shader_flags |= SHADER_DEPTH_BIAS;
    }
    // The mask is per position of a 2x2 footprint, which D3D12 lacks: positions that differ go to
    // the fragment shader, except with alpha-to-coverage in the PSO, which D3D12 does not combine
    // with a shader's coverage; the first position then stands for all. Registers never written
    // read 0, which keeps every sample rather than none.
    const auto samples_off = [](u32 mask) { return mask != 0 ? ~mask & 0xF : 0U; };
    u32 mask_rest = 0;
    if (MaxwellToD3D12::SampleCount(state.msaa_mode) > 1) {
        const u32 first = samples_off(regs.sample_mask_pos.x0_y0);
        shader_flags |= static_cast<u8>(first << SAMPLE_MASK_SHIFT);
        if (state.alpha_to_coverage_enabled == 0) {
            mask_rest = (samples_off(regs.sample_mask_pos.x1_y0) ^ first) |
                        (samples_off(regs.sample_mask_pos.x0_y1) ^ first) << 4 |
                        (samples_off(regs.sample_mask_pos.x1_y1) ^ first) << 8;
        }
    }
    state.sample_mask_rest.Assign(mask_rest);

    // What D3D12 has no state for goes to generated stages and the fragment shader.
    auto& emulated = state.dynamic_state;
    const auto topology_type = MaxwellToD3D12::PrimitiveTopologyType(state.topology);
    const bool lines = topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
                       state.topology != Maxwell::PrimitiveTopology::LinesAdjacency &&
                       state.topology != Maxwell::PrimitiveTopology::LineStripAdjacency;
    const bool triangles = topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
                           state.topology != Maxwell::PrimitiveTopology::TrianglesAdjacency &&
                           state.topology != Maxwell::PrimitiveTopology::TriangleStripAdjacency;
    const float line_width =
        regs.line_anti_alias_enable != 0 ? regs.line_width_smooth : regs.line_width_aliased;
    emulated.generated_lines.Assign(
        lines && (line_width > 1.0f || emulated.line_stipple_enable != 0) ? 1 : 0);
    emulated.generated_polygon_points.Assign(
        triangles && regs.polygon_mode_front == Maxwell::PolygonMode::Point ? 1 : 0);
    emulated.generated_fill_rectangle.Assign(
        triangles && regs.fill_via_triangle_mode != Maxwell::FillViaTriangleMode::Disabled ? 1 : 0);
    emulated.shader_polygon_stipple.Assign(
        triangles && regs.polygon_stipple_enabled != 0 && unique_hashes[5] != 0 ? 1 : 0);
    // Whether a geometry shader is needed for it depends on the fragment program's flat inputs.
    state.provoking_vertex_last.Assign(regs.provoking_vertex == Maxwell::ProvokingVertex::Last &&
                                               (lines || triangles) && unique_hashes[5] != 0
                                           ? 1
                                           : 0);
}

BlendConstantPlan PlanBlendConstants(const FixedPipelineState& state) {
    using MaxwellToD3D12::BlendConstantRead;
    using Equation = Maxwell::Blend::Equation;
    const u8 writable = WritableColorTargetMask(state.attachments);
    // Min and max ignore their factors; integer targets do not blend.
    const auto reads_factors = [&](size_t index) {
        const auto format = static_cast<Tegra::RenderTargetFormat>(state.color_formats[index]);
        const auto& blend = state.attachments[index];
        const Equation equation = blend.EquationRGB();
        return blend.enable != 0 && (writable & (1U << index)) != 0 &&
               format != Tegra::RenderTargetFormat::NONE &&
               !VideoCore::Surface::IsPixelFormatInteger(
                   VideoCore::Surface::PixelFormatFromRenderTargetFormat(format)) &&
               equation != Equation::Min_D3D && equation != Equation::Max_D3D &&
               equation != Equation::Min_GL && equation != Equation::Max_GL;
    };
    bool alpha = false;
    bool color = false;
    bool dest_alpha = false;
    bool dest_color = false;
    for (size_t index = 0; index < NumAttachments(state); ++index) {
        if (!reads_factors(index)) {
            continue;
        }
        const auto& blend = state.attachments[index];
        const BlendConstantRead source = MaxwellToD3D12::ColorBlendConstantRead(blend.SourceRGBFactor());
        const BlendConstantRead dest = MaxwellToD3D12::ColorBlendConstantRead(blend.DestRGBFactor());
        dest_alpha |= dest == BlendConstantRead::Alpha;
        dest_color |= dest == BlendConstantRead::Color;
        alpha |= source == BlendConstantRead::Alpha || dest == BlendConstantRead::Alpha;
        color |= source == BlendConstantRead::Color || dest == BlendConstantRead::Color;
    }
    BlendConstantPlan plan;
    if (!alpha || !color) {
        plan.hardware_alpha = alpha;
        return plan;
    }
    if (dest_alpha && dest_color) {
        BUG_TRACK(UnsupportedState, "destination blend factors of constant color and constant "
                                    "alpha in one pipeline; constant alpha reads the color");
        return plan;
    }
    plan.hardware_alpha = dest_alpha;
    const BlendConstantRead hardware =
        dest_alpha ? BlendConstantRead::Alpha : BlendConstantRead::Color;
    for (size_t index = 0; index < NumAttachments(state); ++index) {
        if (!reads_factors(index)) {
            continue;
        }
        const auto factor = state.attachments[index].SourceRGBFactor();
        const BlendConstantRead source = MaxwellToD3D12::ColorBlendConstantRead(factor);
        if (source != BlendConstantRead::None && source != hardware) {
            plan.shader_factors[index] = ShaderBlendFactor(factor);
        }
    }
    return plan;
}

GraphicsPipeline::GraphicsPipeline(const Device& device_,
                                   const TextureCacheRuntime& texture_runtime_,
                                   VideoCore::ShaderNotify* shader_notify,
                                   Common::ThreadWorker* worker_thread,
                                   const GraphicsPipelineCacheKey& key_,
                                   CompileDxil compile_dxil,
                                   const std::array<const Shader::Info*, NUM_STAGES>& infos,
                                   const PipelineLayout& layout_, bool build_pso,
                                   GraphicsPsoCache* pso_cache_, u32 lower_noperspective_mask_,
                                   const TessEmulationPlan& tess_plan_)
    : device{device_}, texture_runtime{texture_runtime_}, key{key_}, key_hash{key_.Hash()},
      layout{layout_}, lower_noperspective_mask{lower_noperspective_mask_}, tess_plan{tess_plan_},
      pso_cache{pso_cache_},
      from_disk{worker_thread == nullptr} {
    u8 valid_targets{};
    for (size_t index = 0; index < key.state.color_formats.size(); ++index) {
        if (static_cast<Tegra::RenderTargetFormat>(key.state.color_formats[index]) !=
            Tegra::RenderTargetFormat::NONE) {
            valid_targets |= static_cast<u8>(1U << index);
        }
    }
    bound_color_targets = WritableColorTargetMask(key.state.attachments) & valid_targets;
    // Alpha-to-coverage consumes SV_Target0 alpha even with its color writes masked.
    if (key.state.alpha_to_coverage_enabled) {
        bound_color_targets |= valid_targets & 1U;
    }
    // Keep one masked physical target for rasterization when every color write is disabled.
    if (!bound_color_targets && valid_targets) {
        bound_color_targets = valid_targets & static_cast<u8>(-valid_targets);
    }
    depth_bounds = device.SupportsDepthBounds() && key.state.depth_enabled != 0 &&
                   key.state.dynamic_state.depth_bounds_enable != 0;
    blend_constants = PlanBlendConstants(key.state);
    if (key.state.xfb_enabled != 0) {
        for (size_t index = 0; index < stream_output_strides.size(); ++index) {
            stream_output_strides[index] = key.state.xfb_state.layouts[index].stride;
        }
    }
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        const Shader::Info* const info = infos[stage];
        if (!info) {
            continue;
        }
        has_stage[stage] = true;
        stage_infos[stage] = InternStage(*info);
        enabled_uniform_buffer_masks[stage] = info->constant_buffer_mask;
        std::ranges::copy(info->constant_buffer_used_sizes, uniform_buffer_sizes[stage].begin());
        has_images |= !info->image_descriptors.empty();
        local_memory_scratch_words =
            std::max(local_memory_scratch_words, info->local_memory_scratch_words);
        // infos skip VertexA, the first hash.
        LogLocalMemory(STAGE_NAMES[stage], key.unique_hashes[stage + 1], *info);
    }
    // The buffer cache binds each stage's enabled constant buffers in index order, the order of
    // the descriptors that RootSignatureCache walked.
    const std::span<const u32> root_slots = layout.CbvRootSlots();
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!infos[stage]) {
            continue;
        }
        const auto& used_sizes = infos[stage]->constant_buffer_used_sizes;
        for (const auto& desc : infos[stage]->constant_buffer_descriptors) {
            for (u32 element = 0; element < desc.count; ++element) {
                const size_t ordinal = cbv_slots.size();
                const u32 cbuf = desc.index + element;
                cbv_slots.push_back({
                    .root_slot = ordinal < root_slots.size() ? root_slots[ordinal]
                                                             : PipelineLayout::NO_ROOT_CBV,
                    .reach = cbuf < used_sizes.size() ? used_sizes[cbuf] : 0x10000,
                });
            }
        }
    }
    namespace FT = VideoCore::FrameTrace;
    const u64 trace_pipeline = reinterpret_cast<uintptr_t>(this);
    FT::Mark(FT::Event::PipelineBuildRequested, trace_pipeline, key.Hash());
    auto func{[this, shader_notify, compile_dxil = std::move(compile_dxil), trace_pipeline,
               build_pso] {
        FT::Mark(FT::Event::PipelineWorkerBegin, trace_pipeline);
        FT::ScopedSpan worker_span{FT::Event::PipelineWorkerLong, trace_pipeline};
        bool compiled = false;
        try {
            FT::ScopedSpan dxil_span{FT::Event::PipelineDxilLong, trace_pipeline};
            const u64 dxil_start = SteadyMicros();
            dxil = compile_dxil(trace_pipeline, false);
            GetPipelineStats().Record(PipelineStage::Dxil, SteadyMicros() - dxil_start);
            compiled = true;
        } catch (const std::exception& exception) {
            // Handle() stays null: draws with this pipeline are skipped.
            BUG_TRACK_KEY(ShaderCompile, key.Hash(),
                          "DXIL for VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
                          key.unique_hashes[5], exception.what());
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_ERROR(Render, "D3D12: building the pipeline for VS {:016x} PS {:016x} failed: {}",
                      key.unique_hashes[1], key.unique_hashes[5], exception.what());
        }
        if (compiled && build_pso) {
            Build(true);
        }
        if (flatten_retry) {
            // The console's shader compiler faults on some vertex shaders with large branches
            // around the position, and builds them once every branch is a select. A pipeline
            // first built by Wake() has no SPIR-V left to retry with: its draws are skipped for
            // this session, but trying to draw it makes it one of the pipelines built at boot.
            flatten_retry = false;
            try {
                dxil = compile_dxil(trace_pipeline, true);
                flattened_vertex = true;
            } catch (const std::exception& exception) {
                BUG_TRACK_KEY(PipelineRejected, key.Hash(),
                              "the driver faulted building the pipeline of VS {:016x} PS {:016x} "
                              "and flattening its vertex shader failed: {}",
                              key.unique_hashes[1], key.unique_hashes[5], exception.what());
                const Common::BugTracker::TapMute bug_tracker_mute;
                LOG_ERROR(Render, "D3D12: flattening VS {:016x} failed: {}", key.unique_hashes[1],
                          exception.what());
            }
            if (flattened_vertex) {
                Build();
                if (pipeline_state) {
                    LOG_INFO(Render,
                             "D3D12: pipeline for VS {:016x} PS {:016x} built with the vertex "
                             "shader's branches flattened (the driver faulted on the original)",
                             key.unique_hashes[1], key.unique_hashes[5]);
                }
            }
        }
        FT::Mark(FT::Event::PipelineBuildDone, trace_pipeline, pipeline_state.Get() != nullptr);
        // Without a PSO yet, a pipeline whose DXIL failed is still ready: its draws are skipped.
        // Most pipelines loaded at boot stay dormant, so their DXIL waits on disk instead.
        if (compiled && !build_pso) {
            ReleaseDxil();
        }
        Publish((compiled && !build_pso) || out_of_memory ? State::Dormant : State::Ready);
        if (shader_notify) {
            shader_notify->MarkShaderComplete();
        }
    }};
    QueueBuild(worker_thread, std::move(func));
}

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::QueueBuild(Common::ThreadWorker* worker, Common::UniqueFunction<void> job) {
    if (!worker) {
        job();
        return;
    }
    auto build = std::make_shared<PendingBuild>();
    build->queued_us = SteadyMicros();
    build->run = std::move(job);
    {
        std::scoped_lock lock{build_mutex};
        pending = build;
    }
    // Pipelines outlive the workers (joined first), so this is still valid if the worker runs.
    worker->QueueWork([this, build] {
        if (!build->claimed.exchange(true, std::memory_order::acq_rel)) {
            RunClaimed(*build);
        }
    });
}

void GraphicsPipeline::RunClaimed(PendingBuild& build) {
    GetPipelineStats().Record(PipelineStage::QueueWait, SteadyMicros() - build.queued_us);
    {
        std::scoped_lock lock{build_mutex};
        if (pending.get() == &build) {
            pending.reset();
        }
    }
    // Moved out, so the job's captures are gone once it ran, whoever still holds the entry.
    const Common::UniqueFunction<void> job = std::move(build.run);
    job();
}

void GraphicsPipeline::Publish(State next) {
    {
        std::scoped_lock lock{build_mutex};
        // Publish DXIL/PSO to IsBuilt's lock-free fast path as well as WaitBuilt.
        build_state.store(next, std::memory_order::release);
    }
    build_condvar.notify_all();
}

bool GraphicsPipeline::Wake(Common::ThreadWorker& worker) {
    State expected = State::Dormant;
    if (!build_state.compare_exchange_strong(expected, State::Building, std::memory_order::acq_rel)) {
        return false;
    }
    QueueBuild(&worker, [this] {
        // Without its DXIL the pipeline stays ready with no PSO: its draws are skipped. Out of
        // memory, it keeps the DXIL and stays dormant to be woken again.
        out_of_memory = false;
        if (dxil || ReloadDxil()) {
            Build();
        }
        Publish(out_of_memory ? State::Dormant : State::Ready);
    });
    return true;
}

bool GraphicsPipeline::Evict(Scheduler& scheduler) {
    // Only the GPU thread draws with, wakes and evicts pipelines, so a ready one stays ready here.
    if (build_state.load(std::memory_order::acquire) != State::Ready || !pipeline_state ||
        !dxil) {
        return false;
    }
    ReleaseDxil();
    build_state.store(State::Dormant, std::memory_order::release);
    evicted = true;
    // Keep the shared entry discoverable and charged until its last submitted use retires.
    // Other guest variants may still own it or acquire it while this fence is pending.
    scheduler.DeferRelease(std::move(pipeline_state),
                           [retired = std::move(shared_pso), back = std::move(cull_back_state),
                            front = std::move(cull_front_states),
                            capture = std::move(tess_capture_state),
                            control = std::move(tess_control_state)] {});
    return true;
}

void GraphicsPipeline::ReleaseDxil() {
    // Pipelines sharing the linked entry, the linked shader cache and live PSO cache entries may
    // still hold the bytes; they are freed with the last one.
    if (!dxil || !dxil->disk_key || !dxil->stream_output.empty()) {
        return;
    }
    u64 bytes = 0;
    u8 mask = 0;
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!dxil->stages[stage].empty()) {
            mask |= static_cast<u8>(1U << stage);
            bytes += dxil->stages[stage].size();
        }
    }
    released_dxil = dxil->disk_key;
    released_stage_mask = mask;
    dxil.reset();
    dxil_released.fetch_add(1, std::memory_order_relaxed);
    dxil_released_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

bool GraphicsPipeline::ReloadDxil() {
    if (!released_dxil) {
        return false;
    }
    auto stages = ShaderCompiler::LoadPipeline(*released_dxil,
                                               static_cast<size_t>(std::popcount(released_stage_mask)));
    if (!stages) {
        dxil_reload_failed.fetch_add(1, std::memory_order_relaxed);
        BUG_TRACK_KEY(ShaderCompile, key.Hash(),
                      "reading back the DXIL of VS {:016x} PS {:016x} from the disk cache failed",
                      key.unique_hashes[1], key.unique_hashes[5]);
        return false;
    }
    auto reloaded = std::make_shared<ReloadedDxil>();
    u64 bytes = 0;
    size_t next = 0;
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if ((released_stage_mask >> stage) & 1) {
            bytes += (*stages)[next].dxil.capacity();
            reloaded->linked.stages[stage] = std::move((*stages)[next++].dxil);
        }
    }
    reloaded->linked.disk_key = released_dxil;
    reloaded->linked.PreparePsoHashes();
    reloaded->charge = Common::MemoryCharge(Common::MemoryAccount::ShaderBytecode, bytes);
    dxil = SharedDxilStages(reloaded, &reloaded->linked);
    dxil_reloaded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

GraphicsPipeline::DxilReleaseStats GraphicsPipeline::GetDxilReleaseStats() noexcept {
    return {.released = dxil_released.load(std::memory_order_relaxed),
            .released_bytes = dxil_released_bytes.load(std::memory_order_relaxed),
            .reloaded = dxil_reloaded.load(std::memory_order_relaxed),
            .failed = dxil_reload_failed.load(std::memory_order_relaxed)};
}

void GraphicsPipeline::AddTransition(const GraphicsPipelineCacheKey& from_key,
                                     GraphicsPipeline* transition) {
    transition_order.push_back(static_cast<u32>(transition_keys.size()));
    transition_keys.push_back(from_key);
    transitions.push_back(transition);
}

D3D12_GPU_VIRTUAL_ADDRESS IntegerSamplerTable::Upload(BufferCache& buffer_cache) const {
    if (rows.empty()) {
        return 0;
    }
    // The staging alignment (512) covers the root CBV's 256-byte placement.
    const size_t size = Common::AlignUp(num_bindings * sizeof(eden_integer_sampler_state),
                                        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const StagingBufferRef ref = buffer_cache.runtime.UploadStagingBuffer(size);
    // Bindings without a row (storage images, gaps) must read reduction NONE.
    std::memset(ref.mapped_span.data(), 0, size);
    for (const auto& [binding, state] : rows) {
        std::memcpy(ref.mapped_span.data() + binding * sizeof(state), &state, sizeof(state));
    }
    if (VideoCommon::FullCapture::Active()) VideoCommon::FullCapture::Data("integer_sampler_states",
        fmt::format("{{\"num_bindings\":{}}}", num_bindings), ref.mapped_span.first(size));
    return ref.buffer->GetGPUVirtualAddress() + ref.offset;
}

void GraphicsPipeline::WaitBuilt() {
    VideoCore::FrameTrace::ScopedSpan wait_span{
        VideoCore::FrameTrace::Event::PipelineWaitLong, reinterpret_cast<uintptr_t>(this)};
    std::shared_ptr<PendingBuild> build;
    {
        std::scoped_lock lock{build_mutex};
        if (build_state.load(std::memory_order::relaxed) != State::Building) {
            return;
        }
        build = pending;
    }
    // Still queued: nothing to wait for but the queue. A build already running on its worker is
    // waited for (claimed), since the driver call cannot be taken over halfway.
    if (build && !build->claimed.exchange(true, std::memory_order::acq_rel)) {
        GetPipelineStats().inline_builds.fetch_add(1, std::memory_order_relaxed);
        RunClaimed(*build);
    }
    std::unique_lock lock{build_mutex};
    // A build that ran out of memory ends dormant rather than ready.
    build_condvar.wait(lock, [this] {
        return build_state.load(std::memory_order::relaxed) != State::Building;
    });
}

bool GraphicsPipeline::WaitBuiltFor(u64 microseconds) {
    std::unique_lock lock{build_mutex};
    return build_condvar.wait_for(lock, std::chrono::microseconds{microseconds}, [this] {
        return build_state.load(std::memory_order::relaxed) != State::Building;
    });
}

void GraphicsPipeline::Configure(bool is_indexed, const PipelineBindContext& context,
                                 PipelineBindings& out) {
    using VideoCommon::ImageViewInOut;
    using VideoCommon::SamplerId;

    BufferCache& buffer_cache = context.buffer_cache;
    TextureCache& texture_cache = context.texture_cache;
    Tegra::MemoryManager& gpu_memory = context.gpu_memory;
    const auto& maxwell3d = context.maxwell3d;
    const auto& regs = maxwell3d.regs;

    ImageViewList views;
    SamplerIdList samplers;
    DrawSamplerCache<SamplerId> sampler_cache{DrawSamplerReuseEnabled()};
    const bool sample_stage_bindings = DrawMetrics::SampleStageBindings();
    VideoCore::Perf::LapTimer lap;

    texture_cache.SynchronizeDescriptors(false);
    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);

    // stage_view_cache=1: what each stage resolved stays valid under the same binding stamp.
    const bool cache_views = stage_view_cache.load(std::memory_order_relaxed);
    VideoCommon::GraphicsBindingStamp stamp{};
    if (cache_views) {
        stamp = texture_cache.CurrentGraphicsBindingStamp();
        if (!stage_views) {
            stage_views = std::make_unique<StageViewState>();
        }
        if (stage_views->stamp != stamp) {
            stage_views->valid = {};
        }
    }
    std::array<bool, NUM_STAGES> reused{};
    std::array<std::pair<u32, u32>, NUM_STAGES> view_ranges{};
    std::array<u32, NUM_STAGES> first_samplers{};
    boost::container::small_vector<u32, 32> tsc_handles;
    std::array<std::pair<u32, u32>, NUM_STAGES> tsc_ranges{};
    bool any_reused = false;

    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!HasStage(stage)) {
            continue;
        }
        const StageBindings& info = *stage_infos[stage];
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        size_t ssbo_index = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index, desc.cbuf_index,
                                                   desc.cbuf_offset, desc.is_written);
            ++ssbo_index;
        }
        const auto& cbufs = maxwell3d.state.shader_stages[stage].const_buffers;
        const auto read_handle = [&](const auto& desc, u32 index) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(desc)>,
                                         Shader::TextureDescriptor>) {
                if (desc.varying_handle) [[unlikely]] {
                    return Tegra::Texture::TexturePair(out.vertex_texture_handle, via_header_index);
                }
            }
            return ReadTextureHandle(gpu_memory, desc, index, via_header_index,
                                     [&](u32 cbuf) { return cbufs[cbuf].address; });
        };
        const u32 first_view = static_cast<u32>(views.size());
        first_samplers[stage] = static_cast<u32>(samplers.size());
        if (cache_views && stage_views->valid[stage] &&
            ReuseStageViews(info, texture_cache, read_handle, stage_views->entries[stage], views,
                            samplers)) {
            reused[stage] = true;
            any_reused = true;
        } else if (cache_views) {
            // The TSC handles are not kept by the views: record them for StoreStageViews.
            const u32 first_tsc = static_cast<u32>(tsc_handles.size());
            const auto read_and_record = [&](const auto& desc, u32 index) {
                const auto handle = read_handle(desc, index);
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(desc)>,
                                             Shader::TextureDescriptor>) {
                    tsc_handles.push_back(handle.second);
                }
                return handle;
            };
            GatherStageViews(info, texture_cache, false, read_and_record, views, samplers,
                             sampler_cache);
            tsc_ranges[stage] = {first_tsc, static_cast<u32>(tsc_handles.size())};
        } else {
            GatherStageViews(info, texture_cache, false, read_handle, views, samplers,
                             sampler_cache);
        }
        view_ranges[stage] = {first_view, static_cast<u32>(views.size())};
    }
    if (!any_reused) {
        texture_cache.FillImageViews(std::span(views.data(), views.size()), false, has_images);
    } else {
        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
            const auto [first, last] = view_ranges[stage];
            if (!reused[stage] && first != last) {
                texture_cache.FillImageViews(std::span(views.data() + first, last - first), false,
                                             has_images);
            }
        }
        // Resolving the others changed the images (deleted some...): resolve every view again.
        if (texture_cache.CurrentGraphicsBindingStamp().epoch != stamp.epoch) {
            texture_cache.FillImageViews(std::span(views.data(), views.size()), false,
                                         has_images);
        }
    }
    if (cache_views) {
        const VideoCommon::GraphicsBindingStamp after = texture_cache.CurrentGraphicsBindingStamp();
        if (after.epoch != stamp.epoch) {
            // Images changed while the views were resolved: views prepared before may be stale.
            stage_views->valid = {};
            stage_views->stamp = {};
        } else {
            stage_views->stamp = after;
            for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
                if (!HasStage(stage)) {
                    continue;
                }
                if (reused[stage]) {
                    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::StageViewReuses, 1);
                    continue;
                }
                const StageBindings& info = *stage_infos[stage];
                const bool cacheable = StageViewsCacheable(info);
                stage_views->valid[stage] = cacheable;
                if (!cacheable) {
                    continue;
                }
                VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::StageViewMisses, 1);
                const auto [first, last] = view_ranges[stage];
                const auto [first_tsc, last_tsc] = tsc_ranges[stage];
                StoreStageViews(info, std::span(views.data() + first, last - first),
                                std::span(samplers.data() + first_samplers[stage],
                                          samplers.size() - first_samplers[stage]),
                                std::span(tsc_handles.data() + first_tsc, last_tsc - first_tsc),
                                stage_views->entries[stage]);
            }
        }
    }
    if (VideoCore::Perf::DetailedGpuProfileEnabled()) {
        DrawMetrics::sampler_resolve_requests.fetch_add(samplers.size(), std::memory_order_relaxed);
        DrawMetrics::sampler_resolve_reused.fetch_add(sampler_cache.Hits(), std::memory_order_relaxed);
    }
    lap.Lap(VideoCore::Perf::Counter::DrawTexturesNs);

    const ImageViewInOut* texture_buffer_it = views.data();
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!HasStage(stage)) {
            continue;
        }
        const StageBindings& info = *stage_infos[stage];
        buffer_cache.UnbindGraphicsTextureBuffers(stage);
        BindStageTextureBuffers(info, texture_cache, texture_buffer_it,
                                [&](size_t index, ImageView& view,
                                    VideoCore::Surface::PixelFormat format, bool is_written,
                                    bool is_image) {
                                    buffer_cache.BindGraphicsTextureBuffer(
                                        stage, index, view.GpuAddr(), view.BufferSize(), format,
                                        is_written, is_image);
                                });
        texture_buffer_it += info.num_texture_views;
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);
    lap.Lap(VideoCore::Perf::Counter::DrawBuffersNs);

    // Resolve attachments before writing SRVs: a writable depth attachment must be sampled
    // through a GPU snapshot, not through its read-only DSV (which would suppress guest writes).
    VideoCommon::ImageId depth_image;
    {
        VideoCore::Perf::LapTimer target_lap;
        texture_cache.UpdateRenderTargets(false);
        target_lap.Lap(VideoCore::Perf::Counter::DrawUpdateTargetsNs);
        depth_image = texture_cache.GetFramebuffer()->DepthImageId();
        target_lap.Lap(VideoCore::Perf::Counter::DrawFramebufferNs);
    }
    lap.Lap(VideoCore::Perf::Counter::DrawTargetsNs);
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    const auto& dynamic = key.state.dynamic_state;
    const bool writable_depth =
        (dynamic.depth_test_enable != 0 && dynamic.depth_write_enable != 0) ||
        (dynamic.stencil_enable != 0 && key.stencil_write_mask != 0);
    bool depth_feedback_attempted = false;
    bool depth_feedback_ready = false;

    // The table is written in root-signature order while the caches bind, stage by stage:
    // uniform, storage and texel buffers from the buffer cache, then textures and images.
    // Sampler exhaustion may Finish/reset the list. Reserve it before the resource range and
    // streamed UBOs, whose lifetime must cover the submission that actually executes this draw.
    SamplerTableBuilder sampler_table;
    for (const SamplerId id : samplers) sampler_table.Add(texture_cache.GetSampler(id));
    out.sampler_table = sampler_table.Table(context.sampler_heap);
    lap.Lap(VideoCore::Perf::Counter::DrawSamplersNs);
    GuestDescriptorQueue& queue = context.descriptor_queue;
    queue.Acquire(layout.NumResourceDescriptors(), cbv_slots);
    ImageTransitionList image_transitions;
    bool uses_render_area = false;
    const bool has_sampler_table = layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE;
    const bool emulates_reduction =
        (key.shader_flags & GraphicsPipelineCacheKey::SHADER_REDUCTION) != 0;
    IntegerSamplerTable integer_samplers;

    // texture_replay: a draw resolving the same views and samplers as the pipeline's last one,
    // under the same stamp and depth target, binds what that draw computed per texture.
    const TextureReplayMode replay_mode = texture_replay_mode.load(std::memory_order_relaxed);
    TextureReplayState* replay = nullptr;
    bool replay_matches = false;
    VideoCommon::GraphicsBindingStamp replay_stamp{};
    if (replay_mode != TextureReplayMode::Off && !out.trace_all_views && !out.trace_views &&
        !out.trace_filters && !out.trace_types && !GpuDiagnosticsEnabled() &&
        !VideoCommon::FullCapture::Active()) {
        if (!texture_replay) {
            texture_replay = std::make_unique<TextureReplayState>();
        }
        replay = texture_replay.get();
        replay_stamp = texture_cache.CurrentGraphicsBindingStamp();
        replay_matches = replay->framebuffer == framebuffer &&
                         replay->key.Matches(replay_stamp, depth_image, writable_depth, views, samplers);
    }
    const bool replaying = replay_matches && replay_mode == TextureReplayMode::On;
    // What the full path binds, kept to replay it later or, in check mode, to compare.
    const bool recording = replay && !replaying;
    boost::container::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 32> recorded_handles;
    boost::container::small_vector<u8, 32> recorded_prepare;
    u8 texture_unemulated = 0;
    std::array<u32, 2> mirror_border_masks{};
    bool aliased_depth = false;
    size_t texture_index = 0;

    u32 stage_binding = 0;
    const SamplerId* samplers_it = samplers.data();
    const ImageViewInOut* views_it = views.data();
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!HasStage(stage)) {
            continue;
        }
        {
            const DrawMetrics::SampledStageBindingTimer binding_timer{sample_stage_bindings};
            buffer_cache.BindHostStageBuffers(stage);
        }
        const StageBindings& info = *stage_infos[stage];
        views_it += info.num_buffer_views;
        u32 texture_binding = stage_binding + info.first_texture_binding;
        stage_binding += info.num_bindings;
        for (const auto& desc : info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index, ++texture_binding) {
                const VideoCommon::ImageViewId view_id = (views_it++)->id;
                if (replaying) {
                    const size_t t = texture_index++;
                    if (texture_replay->prepare[t] != 0) {
                        texture_cache.GetImageView(view_id).PrepareRead(desc.type);
                    }
                    queue.AddCopy(texture_replay->handles[t]);
                    ++samplers_it;
                    image_transitions.emplace_back(view_id, false);
                    continue;
                }
                const ImageView& image_view = texture_cache.GetImageView(view_id);
                const bool aliases_depth = depth_image != VideoCommon::ImageId{} &&
                                           image_view.image_id == depth_image;
                aliased_depth |= aliases_depth;
                if (aliases_depth && writable_depth && !depth_feedback_attempted) {
                    depth_feedback_attempted = true;
                    depth_feedback_ready = image_view.SourceImage()->PrepareDepthFeedback() != nullptr;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                // A color target the draw also samples is read in place, as upstream does: the
                // guest only orders such reads after its writes across a texture barrier.
                if (aliases_depth && depth_feedback_ready) {
                    handle = image_view.DepthFeedbackHandle(desc.type);
                } else {
                    image_view.PrepareRead(desc.type);
                    handle = image_view.Handle(desc.type);
                }
                queue.AddCopy(handle);
                if (recording) {
                    recorded_handles.push_back(handle);
                    recorded_prepare.push_back(image_view.NeedsPrepareRead(desc.type) ? 1 : 0);
                }
                const SamplerId sampler_id = *(samplers_it++);
                const Sampler& sampler = texture_cache.GetSampler(sampler_id);
                if (has_sampler_table && (desc.is_integer || emulates_reduction)) {
                    integer_samplers.Add(texture_binding, sampler, image_view);
                }
                if (!emulates_reduction && sampler.EmulatedReduction() != 0 &&
                    ReducibleTexture(desc)) {
                    texture_unemulated |= static_cast<u8>(1u << stage);
                }
                if (const u32 axes = sampler.MirrorBorderAxes();
                    axes != 0 && stage == FRAGMENT_STAGE && !desc.is_integer) {
                    const size_t descriptor = &desc - info.texture_descriptors.data();
                    if (descriptor < Shader::Optimization::MIRROR_BORDER_MAX_DESCRIPTORS) {
                        const u32 bit = u32{1} << descriptor;
                        mirror_border_masks[0] |= (axes & 1) != 0 ? bit : 0;
                        mirror_border_masks[1] |= (axes & 2) != 0 ? bit : 0;
                    }
                }
                image_transitions.emplace_back(view_id, false);
                if (out.trace_all_views) out.trace_all_views->push_back(
                    {view_id, false, desc.type, aliases_depth && depth_feedback_ready});
                if (out.trace_views) {
                    out.trace_views->push_back(view_id);
                }
                if (out.trace_filters) {
                    out.trace_filters->push_back(fmt::format(
                        "stage={} binding={} cbuf={}+{:x} element={} depth={} integer={} "
                        "srv=[{}] sampler=[{}]",
                        stage, texture_binding, desc.cbuf_index, desc.cbuf_offset, index,
                        desc.is_depth, desc.is_integer,
                        image_view.DescribeSrv(desc.type, aliases_depth && depth_feedback_ready),
                        sampler.Describe()));
                }
                if (out.trace_types) {
                    out.trace_types->push_back(static_cast<u32>(desc.type));
                }
            }
        }
        PushStorageImages(info, texture_cache, queue, views_it, image_transitions);
        uses_render_area |= info.uses_render_area;
        if (info.uses_destination_read) {
            // The emulated logic op's destination, the last binding of the last stage.
            ImageView& target = texture_cache.GetImageView(framebuffer->ColorViewId(0));
            queue.AddCopy(ColorSnapshotHandle(target, Shader::TextureType::Color2D));
        }
    }
    if (buffer_cache.any_buffer_uploaded) {
        buffer_cache.runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }
    lap.Lap(VideoCore::Perf::Counter::DrawDescriptorsNs);
    VideoCore::Perf::LapTimer target_lap;
    texture_cache.CheckFeedbackLoop(std::span<const ImageViewInOut>{views.data(), views.size()});
    target_lap.Lap(VideoCore::Perf::Counter::DrawFeedbackNs);

    // Depth-based effects sample the bound depth buffer: both uses then share a read-only state
    // (DEPTH_SAMPLED_STATE) and the draw binds the read-only DSV.
    BarrierBatch barriers{context.scheduler};
    ForEachImageTransition(image_transitions, texture_cache, [&](VideoCommon::ImageViewId view_id,
                                                                 bool is_storage) {
        if (out.trace_all_views && is_storage) out.trace_all_views->push_back({view_id, true});
        if (GpuDiagnosticsEnabled()) {
            const auto& view = texture_cache.GetImageView(view_id);
            TraceGpuDetail(fmt::format("graphics image tick={} PSO={} guest={:x} resource={} storage={}",
                context.scheduler.CurrentTick(), static_cast<void*>(pipeline_state.Get()),
                view.GpuAddr(), static_cast<void*>(view.ImageHandle()), is_storage));
        }
        ImageView& image_view = texture_cache.GetImageView(view_id);
        if (!is_storage && depth_image != VideoCommon::ImageId{} &&
            image_view.image_id == depth_image) {
            if (!depth_feedback_ready) {
                out.depth_sampled = true;
                image_view.TransitionImage(DEPTH_SAMPLED_STATE, &barriers);
            }
            return;
        }
        image_view.TransitionImage(is_storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                              : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   &barriers);
    });
    barriers.Flush();
    target_lap.Lap(VideoCore::Perf::Counter::DrawImageTransitionsNs);
    if (out.depth_sampled && writable_depth) {
        WarnOnce(warned_depth_feedback, "depth feedback snapshot allocation failed; depth writes");
    }
    lap.Lap(VideoCore::Perf::Counter::DrawTargetsNs);

    out.resource_table = queue.Table();
    std::ranges::copy(queue.RootConstantBuffers(), out.root_cbvs.begin());
    const u64 tick = context.scheduler.CurrentTick();
    if (replaying) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureReplays, 1);
        TextureReplayState& state = *texture_replay;
        out.unemulated_reduction_stages |= state.unemulated_reduction_stages;
        mirror_border_masks = state.mirror_border_masks;
        if (state.integer_samplers.Empty()) {
            out.integer_samplers = 0;
        } else {
            if (state.integer_tick != tick) {
                state.integer_address = state.integer_samplers.Upload(buffer_cache);
                state.integer_tick = tick;
            }
            out.integer_samplers = state.integer_address;
        }
    } else {
        out.unemulated_reduction_stages |= texture_unemulated;
        out.integer_samplers = integer_samplers.Upload(buffer_cache);
    }
    if (recording) {
        TextureReplayState& state = *texture_replay;
        if (replay_matches) {
            // Check mode: what a replay would have bound against what the full path bound.
            const bool same =
                state.handles.size() == recorded_handles.size() &&
                std::equal(recorded_handles.begin(), recorded_handles.end(), state.handles.begin(),
                           [](auto a, auto b) { return a.ptr == b.ptr; }) &&
                std::equal(recorded_prepare.begin(), recorded_prepare.end(),
                           state.prepare.begin(), state.prepare.end()) &&
                state.unemulated_reduction_stages == texture_unemulated &&
                state.mirror_border_masks == mirror_border_masks &&
                state.integer_samplers.SameRows(integer_samplers);
            if (same) {
                VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureReplays, 1);
            } else {
                VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureReplayMismatches, 1);
                if (texture_replay_logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                    LOG_WARNING(Render,
                                "texture_replay=check: pipeline {} would have bound {} texture "
                                "descriptors that differ from the {} bound in full",
                                static_cast<const void*>(this), state.handles.size(),
                                recorded_handles.size());
                }
                state.key.valid = false;
            }
        } else if (aliased_depth ||
                   texture_cache.CurrentGraphicsBindingStamp() != replay_stamp) {
            // Feedback depends on its snapshot; changed images may resolve to new views.
            state.key.valid = false;
        } else {
            state.key.Store(replay_stamp, depth_image, writable_depth, views, samplers);
            state.framebuffer = framebuffer;
            state.handles.assign(recorded_handles.begin(), recorded_handles.end());
            state.prepare.assign(recorded_prepare.begin(), recorded_prepare.end());
            state.unemulated_reduction_stages = texture_unemulated;
            state.mirror_border_masks = mirror_border_masks;
            state.integer_samplers = integer_samplers;
            state.integer_tick = tick;
            state.integer_address = out.integer_samplers;
        }
    }
    out.mirror_border_masks = mirror_border_masks;
    out.unemulated_mirror_border =
        (mirror_border_masks[0] | mirror_border_masks[1]) != 0 && dynamic.shader_mirror_border == 0;
    lap.Lap(VideoCore::Perf::Counter::DrawSamplersNs);

    // Push constants: no resolution scaling yet (all rescaling bits clear, down factor 1), then
    // the render area, which shares the first words exactly as in the Vulkan backend.
    ResetPushConstants(out.push_constants);
    if (uses_render_area) {
        out.push_constants[0] = std::bit_cast<u32>(static_cast<f32>(regs.surface_clip.width));
        out.push_constants[1] = std::bit_cast<u32>(static_cast<f32>(regs.surface_clip.height));
    }
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC GraphicsPipeline::Describe(
    std::vector<D3D12_INPUT_ELEMENT_DESC>& elements) const {
    using VideoCore::Surface::PixelFormatFromDepthFormat;
    using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

    const auto& stages = dxil->stages;
    const FixedPipelineState& state = key.state;
    const FixedPipelineState::DynamicState& dynamic = state.dynamic_state;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = layout.Handle();
    desc.VS = Bytecode(stages[0]);
    desc.HS = Bytecode(stages[1]);
    desc.DS = Bytecode(stages[2]);
    desc.GS = Bytecode(stages[3]);
    desc.PS = Bytecode(stages[4]);
    // The pixel shader adds the bias (SHADER_DEPTH_BIAS); without one the PSO keeps it.
    const bool shader_bias =
        (key.shader_flags & GraphicsPipelineCacheKey::SHADER_DEPTH_BIAS) != 0 && !stages[4].empty();

    // Native transform feedback: the last pre-rasterization stage streams out the captured
    // outputs where its Xfb decorations place them. Rasterizer discard keeps the capture and
    // drops every primitive after it, so nothing reaches the pixel shader.
    const auto& stream_output = dxil->stream_output;
    if (!stream_output.empty()) {
        const bool rasterize = dynamic.rasterize_enable != 0;
        desc.StreamOutput = DescribeStreamOutput(stream_output, stream_output_strides, rasterize);
        if (!rasterize) {
            desc.PS = {};
        }
    }

    // Vertex input: nir_to_dxil names generic attribute N "TEXCOORD" N. Strides are dynamic (they
    // go in the vertex buffer views), so the layout only needs slots and offsets.
    for (size_t index = 0; index < state.attributes.size(); ++index) {
        const auto& attribute = state.attributes[index];
        if (attribute.enabled == 0 || !stage_infos[0] || !stage_infos[0]->loads.Generic(index)) {
            continue;
        }
        DXGI_FORMAT format = MaxwellToD3D12::VertexFormat(attribute.Type(), attribute.Size());
        if (format == DXGI_FORMAT_UNKNOWN) {
            BUG_TRACK_KEY(UnsupportedFormat,
                          static_cast<u64>(attribute.Type()) << 8 |
                              static_cast<u64>(attribute.Size()),
                          "vertex attribute type {} size {} has no DXGI format; read as RGBA32F",
                          static_cast<u32>(attribute.Type()), static_cast<u32>(attribute.Size()));
            WarnOnceLog(warned_vertex_format, "{} is not supported and is ignored",
                        "a vertex attribute format (read as RGBA32F)");
            format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        const u32 slot = attribute.buffer;
        const u32 divisor = state.binding_divisors[slot];
        // Part k of an attribute fetched in parts is generic N + 32k (MakeRuntimeInfo).
        const MaxwellToD3D12::AttributeFetch fetch = MaxwellToD3D12::SplitAttributeFetch(
            attribute.Type(), attribute.Size(), attribute.offset, state.vertex_strides[slot]);
        if (fetch.parts > 1) {
            format = fetch.part_format;
        }
        // The bgra bit fetches the first and third bytes swapped; DXGI has that only for UNORM.
        if (attribute.bgra != 0) {
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                format = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
        }
        for (u32 part = 0; part < fetch.parts; ++part) {
            elements.push_back({
                .SemanticName = "TEXCOORD",
                .SemanticIndex = static_cast<UINT>(index + part * Shader::IR::NUM_GENERICS),
                .Format = format,
                .InputSlot = slot,
                .AlignedByteOffset = attribute.offset + part * fetch.part_bytes,
                .InputSlotClass = divisor != 0 ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                               : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                .InstanceDataStepRate = divisor,
            });
        }
    }
    // An emulated evaluation shader reads its domain points and patches from buffers; the
    // vertex shader that fetches these runs in the capture pipeline (BuildTessEmulation).
    if (!tess_plan.Active()) {
        desc.InputLayout = {.pInputElementDescs = elements.empty() ? nullptr : elements.data(),
                            .NumElements = static_cast<UINT>(elements.size())};
    }
    desc.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(key.strip_cut);

    const bool has_tessellation = !stages[1].empty() || !stages[2].empty();
    desc.PrimitiveTopologyType = MaxwellToD3D12::PrimitiveTopologyType(state.topology);
    if (tess_plan.Active()) {
        // The tessellator's index list: segments for isolines, triangles otherwise.
        const bool isolines =
            tess_plan.domain == static_cast<u32>(Shader::TessPrimitive::Isolines);
        desc.PrimitiveTopologyType = isolines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                              : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    } else if (has_tessellation) {
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    } else if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
        BUG_TRACK(UnsupportedState, "patch topology without tessellation drawn as points");
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_WARNING(Render, "D3D12: patch topology without tessellation, using points");
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }

    const auto polygon_mode = FixedPipelineState::UnpackPolygonMode(state.polygon_mode);
    if (polygon_mode == Maxwell::PolygonMode::Point && stages[3].empty()) {
        WarnOnce(warned_point_fill, "point fill mode");
    }
    // D3D12 does not allow it with wireframe or line primitives.
    const bool conservative_raster =
        state.conservative_raster_enable != 0 && device.SupportsConservativeRaster() &&
        polygon_mode == Maxwell::PolygonMode::Fill &&
        desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (state.conservative_raster_enable != 0 && !conservative_raster) {
        WarnOnce(warned_conservative, "conservative rasterization");
    }
    // The y-flip happens in the shader (spirv_to_dxil yz_flip), so primitives keep the winding they
    // have in Vulkan's framebuffer space and the front face maps directly.
    desc.RasterizerState = {
        .FillMode = MaxwellToD3D12::FillMode(polygon_mode),
        // Points have no back face; the quads the point geometry shader makes of them would. Same
        // for the quads made of lines, of polygon vertices and of bounding boxes.
        .CullMode = (state.topology == Maxwell::PrimitiveTopology::Points ||
                     (!stages[3].empty() && (dynamic.generated_lines != 0 ||
                                             dynamic.generated_polygon_points != 0 ||
                                             dynamic.generated_fill_rectangle != 0))) &&
                            key.unique_hashes[2] == 0 && key.unique_hashes[3] == 0 &&
                            key.unique_hashes[4] == 0
                        ? D3D12_CULL_MODE_NONE
                        : MaxwellToD3D12::CullMode(dynamic.cull_enable != 0, dynamic.CullFace()),
        .FrontCounterClockwise = dynamic.FrontFace() == Maxwell::FrontFace::CounterClockWise,
        .DepthBias = shader_bias ? 0 : static_cast<INT>(std::bit_cast<f32>(key.depth_bias) / 2.0f),
        .DepthBiasClamp = shader_bias ? 0.0f : std::bit_cast<f32>(key.depth_bias_clamp),
        .SlopeScaledDepthBias =
            shader_bias ? 0.0f : std::bit_cast<f32>(key.slope_scaled_depth_bias),
        .DepthClipEnable = dynamic.depth_clamp_disabled != 0,
        .MultisampleEnable = FALSE,
        .AntialiasedLineEnable = state.smooth_lines != 0,
        .ForcedSampleCount = 0,
        .ConservativeRaster = conservative_raster ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                                  : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    };

    const bool shader_logic_op = EmulatesLogicOp(state) && !stages[4].empty();
    if (dynamic.logic_op_enable != 0 && !shader_logic_op) {
        WarnOnce(warned_logic_op, "logic op");
    }
    const size_t num_attachments = DrawColorTargetCount(NumAttachments(state), bound_color_targets);
    desc.BlendState.AlphaToCoverageEnable =
        state.alpha_to_coverage_enabled != 0 && key.ShaderAlphaToCoverage() == 0;
    desc.BlendState.IndependentBlendEnable = TRUE;
    desc.NumRenderTargets = static_cast<UINT>(num_attachments);
    for (size_t index = 0; index < num_attachments; ++index) {
        desc.BlendState.RenderTarget[index] =
            BlendTarget(state.attachments[index],
                        blend_constants.shader_factors[index] != Shader::BlendConstantFactor::None);
        if (index == 0 && shader_logic_op) {
            desc.BlendState.RenderTarget[0].BlendEnable = FALSE; // A logic op replaces blending.
        }
        const auto format = static_cast<Tegra::RenderTargetFormat>(state.color_formats[index]);
        desc.RTVFormats[index] = DrawColorTargetFormat(
            format == Tegra::RenderTargetFormat::NONE
                ? DXGI_FORMAT_UNKNOWN
                : texture_runtime.Format(PixelFormatFromRenderTargetFormat(format)).view,
            bound_color_targets, index);
    }

    if (state.depth_enabled != 0) {
        const auto depth_format = static_cast<Tegra::DepthFormat>(state.depth_format.Value());
        desc.DSVFormat = texture_runtime.Format(PixelFormatFromDepthFormat(depth_format)).dsv;
    }
    const bool has_depth = desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
    if (dynamic.depth_bounds_enable != 0 && !depth_bounds) {
        WarnOnce(warned_depth_bounds, "the depth bounds test");
    }
    desc.DepthStencilState = {
        .DepthEnable = has_depth && dynamic.depth_test_enable != 0,
        .DepthWriteMask = has_depth && dynamic.depth_write_enable != 0
                              ? D3D12_DEPTH_WRITE_MASK_ALL
                              : D3D12_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc = dynamic.depth_test_enable != 0
                         ? MaxwellToD3D12::ComparisonFunc(dynamic.DepthTestFunc())
                         : D3D12_COMPARISON_FUNC_ALWAYS,
        .StencilEnable = has_depth && dynamic.stencil_enable != 0,
        .StencilReadMask = key.stencil_read_mask,
        .StencilWriteMask = key.stencil_write_mask,
        .FrontFace = StencilFace(dynamic.front),
        .BackFace = StencilFace(dynamic.back),
    };

    desc.SampleMask = key.SampleMask();
    desc.SampleDesc = {.Count = MaxwellToD3D12::SampleCount(state.msaa_mode), .Quality = 0};

    return desc;
}

ComPtr<ID3D12PipelineState> GraphicsPipeline::CreatePickProbePipeline(
    std::span<const u8> ps, bool float_outputs) const {
    if (!dxil || key.unique_hashes[1] != 0x17d43777e7441cf4ULL ||
        key.unique_hashes[5] != 0x0b7e4e46d61d3694ULL || has_stage[1] || has_stage[2] ||
        has_stage[3] || layout.NumResourceDescriptors() != 5 ||
        layout.NumSamplerDescriptors() != 4) {
        throw std::runtime_error("pick probe: unsupported linked interface");
    }
    for (const auto& info : stage_infos) {
        if (info && (!info->storage_buffers_descriptors.empty() ||
                     !info->image_descriptors.empty() || !info->image_buffer_descriptors.empty()))
            throw std::runtime_error("pick probe: side effects rejected");
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    auto desc = Describe(elements);
    if (desc.SampleDesc.Count != 1 || desc.DSVFormat != DXGI_FORMAT_UNKNOWN ||
        desc.NumRenderTargets != 3 || desc.DepthStencilState.DepthEnable ||
        desc.DepthStencilState.StencilEnable)
        throw std::runtime_error("pick probe: unsupported framebuffer");
    desc.PS = {ps.data(), ps.size()};
    if (float_outputs) {
        for (u32 i = 0; i < 3; ++i) desc.RTVFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    ComPtr<ID3D12PipelineState> result;
    ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&result)),
                  "pick capture probe pipeline");
    return result;
}

ComPtr<ID3D12PipelineState> GraphicsPipeline::CreateMaterialProbePipeline(
    std::span<const u8> pixel_shader, std::span<const u8> geometry_shader,
    bool reflection_probe, DXGI_FORMAT output_format) const {
    // Private diagnostic sidecars have a fixed linked interface. Reject every other
    // pipeline rather than applying a material-specific replacement to normal draws.
    if (!dxil || has_stage[1] || has_stage[2] || has_stage[3]) {
        throw std::runtime_error("material GPU probe: unsupported stages");
    }
    if (output_format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
        (!reflection_probe || output_format != DXGI_FORMAT_R11G11B10_FLOAT)) {
        throw std::runtime_error("material GPU probe: unsupported output format");
    }
    if (reflection_probe) {
        if (key.unique_hashes[1] != 0x55e0d63ec3a46b1fULL ||
            key.unique_hashes[5] != 0xba9295f0d82e7ae1ULL || !geometry_shader.empty() ||
            StageInfo(4).interpolation != 0x2000 ||
            layout.NumResourceDescriptors() != 13 || layout.NumSamplerDescriptors() != 6 ||
            (lower_noperspective_mask != 0 && lower_noperspective_mask != 0x40)) {
            throw std::runtime_error("material reflection probe: unsupported linked interface");
        }
    } else if (StageInfo(4).interpolation != 0xa || !StageInfo(0).stores.Generic(0) ||
               !StageInfo(0).stores.Generic(1) || layout.NumResourceDescriptors() != 7 ||
               layout.NumSamplerDescriptors() != 2) {
        throw std::runtime_error("material GPU probe: unsupported shader interface");
    }
    for (const auto& info : stage_infos) {
        if (info && (!info->storage_buffers_descriptors.empty() || !info->image_descriptors.empty() ||
                     !info->image_buffer_descriptors.empty())) {
            throw std::runtime_error("material GPU probe: side-effecting stage rejected");
        }
    }
    const auto& textures = StageInfo(4).texture_descriptors;
    const auto first_binding = FirstTextureBinding(StageInfo(4), NumStageBindings(StageInfo(0)));
    if (textures.size() != (reflection_probe ? 5U : 2U) ||
        first_binding != (reflection_probe ? 8U : 5U)) {
        throw std::runtime_error("material GPU probe: unsupported texture bindings");
    }
    constexpr std::array reflection_types{Shader::TextureType::Color2D,
        Shader::TextureType::Color2D, Shader::TextureType::ColorCube,
        Shader::TextureType::Color1D, Shader::TextureType::Color2D};
    for (size_t i = 0; i < textures.size(); ++i) {
        const auto& texture = textures[i];
        if (texture.type != (reflection_probe ? reflection_types[i] : Shader::TextureType::Color2D) ||
            texture.is_depth || texture.is_integer || texture.is_multisample || texture.count != 1) {
            throw std::runtime_error("material GPU probe: unsupported texture type");
        }
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    auto desc = Describe(elements);
    if ((!reflection_probe && desc.DSVFormat != DXGI_FORMAT_UNKNOWN) || desc.SampleDesc.Count != 1 ||
        desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) {
        throw std::runtime_error("material GPU probe: depth/MSAA/topology unsupported");
    }
    // Isolated probe does not attach or modify the guest depth/stencil surface.
    if (reflection_probe) {
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
    }
    desc.PS = {pixel_shader.data(), pixel_shader.size()};
    if (!geometry_shader.empty()) {
        desc.GS = {geometry_shader.data(), geometry_shader.size()};
    }
    desc.NumRenderTargets = 1;
    std::fill(std::begin(desc.RTVFormats), std::end(desc.RTVFormats), DXGI_FORMAT_UNKNOWN);
    desc.RTVFormats[0] = output_format;
    desc.BlendState = {};
    desc.BlendState.RenderTarget[0] = {
        .SrcBlend = D3D12_BLEND_ONE, .DestBlend = D3D12_BLEND_ZERO,
        .BlendOp = D3D12_BLEND_OP_ADD, .SrcBlendAlpha = D3D12_BLEND_ONE,
        .DestBlendAlpha = D3D12_BLEND_ZERO, .BlendOpAlpha = D3D12_BLEND_OP_ADD,
        .LogicOp = D3D12_LOGIC_OP_NOOP, .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL};
    ComPtr<ID3D12PipelineState> result;
    ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&result)),
                  "material GPU probe pipeline");
    return result;
}

namespace {
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

/// The description as a pipeline state stream, with DEPTH_STENCIL1 to enable the depth bounds
/// test (D3D12_GRAPHICS_PIPELINE_STATE_DESC has no field for it). Same state otherwise.
HRESULT CreateWithDepthBounds(ID3D12Device2* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                              ComPtr<ID3D12PipelineState>& out) {
    struct Stream {
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE> ds;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE> hs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE> gs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC> so;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1>
            depth;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC> input;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,
                  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>
            strip_cut;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
                  D3D12_PRIMITIVE_TOPOLOGY_TYPE>
            topology;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>
            rtvs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT> node_mask;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, D3D12_PIPELINE_STATE_FLAGS> flags;
    } stream;
    stream.root.value = desc.pRootSignature;
    stream.vs.value = desc.VS;
    stream.ps.value = desc.PS;
    stream.ds.value = desc.DS;
    stream.hs.value = desc.HS;
    stream.gs.value = desc.GS;
    stream.so.value = desc.StreamOutput;
    stream.blend.value = desc.BlendState;
    stream.sample_mask.value = desc.SampleMask;
    stream.raster.value = desc.RasterizerState;
    const D3D12_DEPTH_STENCIL_DESC& depth = desc.DepthStencilState;
    stream.depth.value = {
        .DepthEnable = depth.DepthEnable,
        .DepthWriteMask = depth.DepthWriteMask,
        .DepthFunc = depth.DepthFunc,
        .StencilEnable = depth.StencilEnable,
        .StencilReadMask = depth.StencilReadMask,
        .StencilWriteMask = depth.StencilWriteMask,
        .FrontFace = depth.FrontFace,
        .BackFace = depth.BackFace,
        .DepthBoundsTestEnable = TRUE,
    };
    stream.input.value = desc.InputLayout;
    stream.strip_cut.value = desc.IBStripCutValue;
    stream.topology.value = desc.PrimitiveTopologyType;
    stream.rtvs.value.NumRenderTargets = desc.NumRenderTargets;
    std::copy(std::begin(desc.RTVFormats), std::end(desc.RTVFormats),
              std::begin(stream.rtvs.value.RTFormats));
    stream.dsv.value = desc.DSVFormat;
    stream.samples.value = desc.SampleDesc;
    stream.node_mask.value = desc.NodeMask;
    stream.flags.value = desc.Flags;
    const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{.SizeInBytes = sizeof(stream),
                                                       .pPipelineStateSubobjectStream = &stream};
    return device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&out));
}
/// Whether a pipeline with this key fails for the first time.
bool FirstFailure(u64 key_hash) {
    static std::mutex mutex;
    static std::unordered_set<u64> failed;
    std::scoped_lock lock{mutex};
    return failed.insert(key_hash).second;
}
} // Anonymous namespace

void GraphicsPipeline::LinkedDxil::PreparePsoHashes() {
    PsoShaderSpans spans;
    for (size_t i = 0; i < spans.size(); ++i) {
        spans[i] = stages[i];
    }
    pso_hashes.emplace(spans);
}

void GraphicsPipeline::Build(bool flatten_on_fault) try {
    const auto& stages = dxil->stages;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    auto desc = Describe(elements);
    const size_t num_attachments = desc.NumRenderTargets;
    // Only with a depth target: without one there is nothing to test against.
    const bool bounds_test = depth_bounds && desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
    auto host_key = MakeGraphicsPsoKey(desc, dxil, dxil->PsoHashes());
    host_key.state.push_back(bounds_test ? 1U : 0U);
    // The driver's blob of this PSO from an earlier session (pso_blobs); not through the stream.
    const bool use_blob = !bounds_test && PsoBlobCache::Enabled();
    const u64 blob_hash = use_blob ? PsoBlobCache::StableHash(host_key) : 0;
    const u64 index_bytes = sizeof(GraphicsPsoCache::Entry) + 128 +
                            host_key.state.capacity() * sizeof(u32);
    HRESULT hr = S_OK;
    const bool may_flatten = flatten_on_fault && !flattened_vertex;
    const auto log_driver_fault = [&](DWORD code, ComPtr<ID3D12PipelineState>& handle) {
        handle.Detach(); // whatever the driver left there is not an object to release
        if (may_flatten) {
            return; // the caller retries with flattened branches
        }
        BUG_TRACK_KEY(PipelineRejected, key.Hash(),
                      "the driver faulted (0x{:08X}) building the pipeline of VS {:016x} PS "
                      "{:016x}; its draws are skipped",
                      static_cast<u32>(code), key.unique_hashes[1], key.unique_hashes[5]);
    };
    const auto try_create = [&](ComPtr<ID3D12PipelineState>& handle, DWORD& fault) {
        if (bounds_test) {
            const HRESULT stream_hr = CallDriverGuarded(
                [&] { return CreateWithDepthBounds(device.Get2(), desc, handle); }, fault);
            if (SUCCEEDED(stream_hr) || stream_hr == DRIVER_FAULT_HRESULT) {
                return stream_hr;
            }
            // The option bit is not a promise the driver takes the stream: draw without the test.
            WarnOnce(warned_depth_bounds, "the depth bounds test (pipeline stream rejected)");
        }
        ID3D12Device* const raw = device.Get();
        return CallDriverGuarded(
            [&] { return raw->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&handle)); }, fault);
    };
    const auto create_pso = [&](ComPtr<ID3D12PipelineState>& handle) {
        DWORD fault = 0;
        HRESULT result;
        const std::vector<u8> blob =
            use_blob ? GetPsoBlobCache().Load(blob_hash) : std::vector<u8>{};
        desc.CachedPSO = {blob.data(), blob.size()};
        {
            std::shared_lock build_lock{PipelineBuildMutex()};
            result = try_create(handle, fault);
        }
        desc.CachedPSO = {};
        if (!blob.empty() && FAILED(result) && result != E_OUTOFMEMORY &&
            result != DRIVER_FAULT_HRESULT) {
            // A blob of another driver version or adapter: dropped, and the PSO made from scratch.
            handle.Reset();
            GetPsoBlobCache().Reject(blob_hash);
            std::shared_lock build_lock{PipelineBuildMutex()};
            result = try_create(handle, fault);
            if (SUCCEEDED(result)) {
                GetPsoBlobCache().Store(blob_hash, handle.Get());
            }
        } else if (use_blob && blob.empty() && SUCCEEDED(result)) {
            GetPsoBlobCache().Store(blob_hash, handle.Get());
        }
        if (result == DRIVER_FAULT_HRESULT) {
            log_driver_fault(fault, handle);
        }
        if (result == E_OUTOFMEMORY) {
            MemoryReserve::NoteOutOfMemory();
            if (MemoryReserve::Release()) {
                std::shared_lock build_lock{PipelineBuildMutex()};
                result = try_create(handle, fault);
            }
        }
        return result;
    };
    const auto create = [&] {
        const u64 create_start = SteadyMicros();
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PsoCreateUs};
        VideoCore::Perf::Add(VideoCore::Perf::Counter::PsoCreates, 1);
        VideoCore::FrameTrace::ScopedSpan pso_span{
            VideoCore::FrameTrace::Event::PipelinePsoLong, reinterpret_cast<uintptr_t>(this)};
        ResidentGraphicsPso result;
        result.root = desc.pRootSignature;
        result.index_charge = Common::MemoryCharge(Common::MemoryAccount::Pipelines, index_bytes, 0);
        if (pso_probe_enabled.load(std::memory_order_relaxed)) {
            std::scoped_lock probe_lock{pso_probe_mutex};
            const s64 before = PrivateCommit();
            hr = create_pso(result.handle);
            if (SUCCEEDED(hr) && before != 0) {
                pso_probe_samples.push_back(PrivateCommit() - before);
            }
        } else {
            hr = create_pso(result.handle);
        }
        GetPipelineStats().Record(PipelineStage::PsoCreate, SteadyMicros() - create_start);
        // Failed entries must not poison the index. All current waiters receive the HRESULT.
        if (FAILED(hr)) {
            throw hr;
        }
        return result;
    };
    VideoCore::Perf::Add(VideoCore::Perf::Counter::PsoRequests, 1);
    try {
        if (pso_cache) {
            const auto result = pso_cache->Get(std::move(host_key), create);
            shared_pso = result.entry;
            pipeline_state = shared_pso->value->handle;
            switch (result.outcome) {
            case GraphicsPsoCache::Outcome::Hit:
                VideoCore::Perf::Add(VideoCore::Perf::Counter::PsoHits, 1);
                break;
            case GraphicsPsoCache::Outcome::SharedInFlight:
                VideoCore::Perf::Add(VideoCore::Perf::Counter::PsoShared, 1);
                break;
            case GraphicsPsoCache::Outcome::Bypassed:
                VideoCore::Perf::Add(VideoCore::Perf::Counter::PsoBypasses, 1);
                break;
            case GraphicsPsoCache::Outcome::Created:
                break;
            }
        } else {
            // Standalone gates also use a shared entry so accounting follows COM lifetime.
            GraphicsPsoCache local_cache;
            shared_pso = local_cache.Get(std::move(host_key), create).entry;
            pipeline_state = shared_pso->value->handle;
        }
    } catch (HRESULT failure) {
        hr = failure;
    }
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("graphics PSO for VS {:016x} PS {:016x} (HRESULT 0x{:08X})",
                           key.unique_hashes[1], key.unique_hashes[5], static_cast<u32>(hr));
    });
    if (hr == DRIVER_FAULT_HRESULT && may_flatten) {
        LOG_INFO(Render, "D3D12: the driver faulted building the pipeline for VS {:016x} PS "
                         "{:016x}; retrying with the vertex shader's branches flattened",
                 key.unique_hashes[1], key.unique_hashes[5]);
        flatten_retry = true;
        pipeline_state.Reset();
        shared_pso.reset();
        return;
    }
    if (FAILED(hr)) {
        BUG_TRACK_KEY(PipelineRejected, key.Hash(),
                      "CreateGraphicsPipelineState failed (HRESULT 0x{:08X}) for VS {:016x} PS "
                      "{:016x}: {} attributes, {} RTs (RT0 {}), DSV {}",
                      static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[5],
                      elements.size(), num_attachments, static_cast<u32>(desc.RTVFormats[0]),
                      static_cast<u32>(desc.DSVFormat));
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_ERROR(Render,
                  "D3D12: CreateGraphicsPipelineState failed (HRESULT 0x{:08X}) for VS {:016x} "
                  "PS {:016x}: {} attributes, {} RTs (RT0 {}), DSV {}, features VS {:x} PS {:x}",
                  static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[5],
                  elements.size(), num_attachments, static_cast<u32>(desc.RTVFormats[0]),
                  static_cast<u32>(desc.DSVFormat), ShaderFeatureFlags(stages[0]),
                  ShaderFeatureFlags(stages[4]));
        if (hr == E_OUTOFMEMORY) {
            // Built again once memory is back: the pipeline stays dormant with its DXIL.
            out_of_memory = true;
        } else if (FirstFailure(key.Hash())) {
            // Every pipeline object with these shaders fails again; diagnose the first, alone in
            // the driver so that other builds do not decide its variants.
            std::unique_lock build_lock{PipelineBuildMutex()};
            if (hr == DRIVER_FAULT_HRESULT) {
                DumpFaultedPipeline(key.unique_hashes[1], key.unique_hashes[5], stages);
                ProbeFaultedPixelShader(device.Get(), desc, key.unique_hashes[5]);
            }
            DiagnoseFailedPipeline(device.Get(), desc);
        }
        pipeline_state.Reset();
        return;
    }
    if (tess_plan.Active() && !BuildTessEmulation(elements)) {
        pipeline_state.Reset();
        shared_pso.reset();
        return;
    }
    if (GpuDiagnosticsEnabled()) {
        LOG_INFO(Render, "D3D12 PSO identity: PSO={} shaders={:016x}/{:016x}/{:016x}/{:016x}/{:016x}/{:016x} RTs={} RT0={} DSV={}",
            static_cast<void*>(pipeline_state.Get()), key.unique_hashes[0], key.unique_hashes[1],
            key.unique_hashes[2], key.unique_hashes[3], key.unique_hashes[4], key.unique_hashes[5],
            num_attachments, static_cast<u32>(desc.RTVFormats[0]), static_cast<u32>(desc.DSVFormat));
    }
    LOG_INFO(Render,
             "D3D12: pipeline built for VS {:016x} PS {:016x} ({} attributes, {} RTs, RT0 {}, "
             "DSV {}, {} + {} descriptors, features VS {:x} PS {:x})",
             key.unique_hashes[1], key.unique_hashes[5], elements.size(), num_attachments,
             static_cast<u32>(desc.RTVFormats[0]), static_cast<u32>(desc.DSVFormat),
             layout.NumResourceDescriptors(), layout.NumSamplerDescriptors(),
             ShaderFeatureFlags(stages[0]), ShaderFeatureFlags(stages[4]));
} catch (const std::exception& exception) {
    pipeline_state.Reset();
    shared_pso.reset();
    LOG_ERROR(Render, "D3D12: graphics pipeline preparation failed: {}", exception.what());
    VideoCommon::GPUThread::ReportException("D3D12 graphics pipeline preparation failed");
} catch (...) {
    pipeline_state.Reset();
    shared_pso.reset();
    LOG_ERROR(Render, "D3D12: graphics pipeline preparation failed with an unknown exception");
    VideoCommon::GPUThread::ReportException("D3D12 graphics pipeline preparation failed");
}

bool GraphicsPipeline::BuildTessEmulation(std::span<const D3D12_INPUT_ELEMENT_DESC> elements) {
    // The vertex shader alone over the guest's vertices, one point each, storing to the capture
    // buffer. Its points land behind the viewer, with nothing to write to anyway.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC capture{};
    capture.pRootSignature = layout.Handle();
    capture.VS = Bytecode(dxil->tess_capture);
    capture.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    capture.SampleMask = UINT_MAX;
    capture.RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID,
                               .CullMode = D3D12_CULL_MODE_NONE,
                               .DepthClipEnable = TRUE};
    capture.InputLayout = {.pInputElementDescs = elements.empty() ? nullptr : elements.data(),
                           .NumElements = static_cast<UINT>(elements.size())};
    capture.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    capture.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    capture.SampleDesc = {.Count = 1, .Quality = 0};
    const D3D12_COMPUTE_PIPELINE_STATE_DESC control{
        .pRootSignature = layout.Handle(),
        .CS = Bytecode(dxil->tess_control),
    };
    HRESULT hr;
    {
        std::shared_lock build_lock{PipelineBuildMutex()};
        hr = device.Get()->CreateGraphicsPipelineState(&capture, IID_PPV_ARGS(&tess_capture_state));
        if (SUCCEEDED(hr)) {
            hr = device.Get()->CreateComputePipelineState(&control,
                                                          IID_PPV_ARGS(&tess_control_state));
        }
    }
    if (SUCCEEDED(hr)) {
        return true;
    }
    tess_capture_state.Reset();
    tess_control_state.Reset();
    BUG_TRACK_KEY(PipelineRejected, key.Hash(),
                  "tessellation emulation pipelines rejected (HRESULT 0x{:08X}) for VS {:016x} "
                  "TCS {:016x}; its draws are skipped",
                  static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[2]);
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render,
              "D3D12: tessellation emulation pipelines failed (HRESULT 0x{:08X}) for VS {:016x} "
              "TCS {:016x}",
              static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[2]);
    return false;
}

std::optional<GraphicsPsoKey> GraphicsPipeline::FoldIdentity() const {
    if (!dxil) {
        return std::nullopt;
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    auto desc = Describe(elements);
    auto& depth = desc.DepthStencilState;
    if (!depth.StencilEnable) {
        depth.FrontFace = {};
        depth.BackFace = {};
    }
    if (!depth.DepthEnable) {
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    for (auto& target : desc.BlendState.RenderTarget) {
        if (target.RenderTargetWriteMask == 0) {
            target = {};
        }
    }
    desc.RasterizerState.DepthBiasClamp = RoundDepthBiasStep(desc.RasterizerState.DepthBiasClamp);
    desc.RasterizerState.SlopeScaledDepthBias =
        RoundDepthBiasStep(desc.RasterizerState.SlopeScaledDepthBias);
    return MakeGraphicsPsoKey(desc, dxil, dxil->PsoHashes());
}

ID3D12PipelineState* GraphicsPipeline::CulledHandle(D3D12_CULL_MODE mode, u8 back_read_mask,
                                                    u8 back_write_mask) {
    const bool back_faces = mode == D3D12_CULL_MODE_FRONT;
    const u16 masks = static_cast<u16>(back_read_mask | back_write_mask << 8);
    // std::addressof: operator& on a WRL ComPtr converts by releasing the object it holds, and the
    // commands already recorded with that object run later.
    ComPtr<ID3D12PipelineState>* slot = std::addressof(cull_back_state);
    if (back_faces) {
        const auto it = std::ranges::find_if(
            cull_front_states, [masks](const auto& entry) { return entry.first == masks; });
        slot = it != cull_front_states.end()
                   ? std::addressof(it->second)
                   : std::addressof(cull_front_states.emplace_back(masks, nullptr).second);
    }
    ComPtr<ID3D12PipelineState>& handle = *slot;
    if (handle) {
        return handle.Get();
    }
    const bool reloaded = !dxil;
    if (reloaded && !ReloadDxil()) {
        return nullptr;
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    auto desc = Describe(elements);
    desc.RasterizerState.CullMode = mode;
    if (back_faces && desc.DepthStencilState.StencilEnable) {
        desc.DepthStencilState.StencilReadMask = back_read_mask;
        desc.DepthStencilState.StencilWriteMask = back_write_mask;
    }
    const bool bounds_test = depth_bounds && desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
    DWORD fault = 0;
    HRESULT result;
    {
        std::shared_lock build_lock{PipelineBuildMutex()};
        result = CallDriverGuarded(
            [&] {
                if (bounds_test) {
                    return CreateWithDepthBounds(device.Get2(), desc, handle);
                }
                return device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&handle));
            },
            fault);
    }
    if (result == DRIVER_FAULT_HRESULT) {
        handle.Detach(); // whatever the driver left there is not an object to release
    } else if (FAILED(result)) {
        handle.Reset();
    }
    if (reloaded) {
        ReleaseDxil();
    }
    return handle.Get();
}

void SetPsoCostProbe(bool enabled) {
    pso_probe_enabled.store(enabled, std::memory_order_relaxed);
}

std::string DescribePsoCost() {
    std::vector<s64> samples;
    {
        std::scoped_lock probe_lock{pso_probe_mutex};
        samples = pso_probe_samples;
    }
    if (samples.empty()) {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    s64 sum = 0;
    for (const s64 sample : samples) {
        sum += sample;
    }
    const auto at = [&](size_t percent) { return samples[(samples.size() - 1) * percent / 100] / 1024; };
    return fmt::format("PSO driver commit over {} builds: median {} KiB, p10 {} KiB, p90 {} KiB, "
                       "mean {} KiB",
                       samples.size(), at(50), at(10), at(90),
                       sum / static_cast<s64>(samples.size()) / 1024);
}

} // namespace D3D12
