// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <spirv_to_dxil.h>

#include "common/alignment.h"
#include "common/bug_tracker.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/cpu_write_queue.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_async_pipelines.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/renderer_d3d12/d3d12_draw_metrics.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_tracking.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_compute_inputs.h"

namespace D3D12 {

namespace {

std::atomic<u32> submit_draws{512};

/// Some color or depth target of the framebuffer has had no draw recorded to it yet.
bool HasNewTarget(const Framebuffer& framebuffer) {
    for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
        const Image* const image = framebuffer.ColorImage(index);
        if (image && !image->TargetDrawn()) {
            return true;
        }
    }
    const Image* const depth = framebuffer.DepthImage();
    return depth && !depth->TargetDrawn();
}

void MarkTargetsDrawn(const Framebuffer& framebuffer) {
    for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
        if (const Image* const image = framebuffer.ColorImage(index)) {
            image->MarkTargetDrawn();
        }
    }
    if (const Image* const depth = framebuffer.DepthImage()) {
        depth->MarkTargetDrawn();
    }
}

} // Anonymous namespace

void SetSubmitDraws(u32 draws) {
    submit_draws.store(draws, std::memory_order_relaxed);
}

bool RasterizerD3D12::CullsEveryPrimitive() const {
    // D3D12 has no cull mode for both faces (MaxwellToD3D12::CullMode): those draws are skipped.
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    const auto& regs = maxwell3d->regs;
    if (regs.gl_cull_test_enabled == 0 || regs.gl_cull_face != Maxwell::CullFace::FrontAndBack) {
        return false;
    }
    // Geometry and tessellation stages may turn the primitives into points or lines.
    if (regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
        regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation) ||
        regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry)) {
        return false;
    }
    // Culling only discards polygons: points and lines still draw.
    switch (maxwell3d->draw_manager.draw_state.topology) {
    case Maxwell::PrimitiveTopology::Points:
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
    case Maxwell::PrimitiveTopology::Patches:
        return false;
    default:
        return true;
    }
}

RasterizerD3D12::RasterizerD3D12(Tegra::GPU& gpu_,
                                 Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                 const Device& device_, Scheduler& scheduler_,
                                 const ShaderCompiler& compiler,
                                 BufferCacheRuntime& buffer_runtime_,
                                 TextureCacheRuntime& texture_runtime,
                                 DescriptorRing& descriptor_ring_, SamplerHeap& sampler_heap_,
                                 BlitImageHelper& blit_helper_, StagingBufferPool& staging)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_}, scheduler{scheduler_}, staging{staging},
      buffer_runtime{buffer_runtime_}, texture_runtime{texture_runtime}, descriptor_ring{descriptor_ring_},
      sampler_heap{sampler_heap_}, blit_helper{blit_helper_},
      indirect_args{device, scheduler_, staging},
      stream_output{device, scheduler_, compiler},
      tess_emulation{device, scheduler_, compiler},
      descriptor_queue{device.Get(), descriptor_ring_},
      buffer_cache{device_memory_, buffer_runtime_}, texture_cache{texture_runtime, device_memory_},
      pipeline_cache{device_memory_, device, compiler, texture_runtime, gpu_.ShaderNotify()},
      local_memory_pool{device, scheduler_},
      query_runtime{device, scheduler_, compiler, buffer_cache, buffer_runtime_, staging},
      query_cache{gpu_, *this, device_memory_, query_runtime},
      conditional_rendering{device, scheduler_, compiler, query_cache, buffer_cache},
      accelerate_dma{buffer_cache, texture_cache, draw_hunt},
      fence_manager{*this, gpu_, texture_cache, buffer_cache, query_cache, scheduler_},
      draw_hunt{device, scheduler_} {
    buffer_runtime.SetDescriptorQueue(&descriptor_queue);
    scheduler.RegisterOnReset([this] { InvalidateCommandListState(); });
    LOG_INFO(Render, "D3D12: rasterizer active (draws, clears, caches, DMA and pipelines)");
}

RasterizerD3D12::~RasterizerD3D12() {
    // Renderer drains the scheduler before destroying caches/staging. Export completed work
    // on a normal shutdown too; a removed device must never make destruction throw.
    try {
        std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
        PollMaterialReadbacks();
        for (auto& pending : material_readbacks) staging.FreeDeferred(pending.buffer);
        for (auto& pending : material_buffer_readbacks)
            if (pending.buffer.buffer) staging.FreeDeferred(pending.buffer);
        if (pending_material_probe) staging.FreeDeferred(pending_material_probe->buffer);
    } catch (...) {
    }
    buffer_runtime.SetDescriptorQueue(nullptr);
}

bool RasterizerD3D12::WaitPendingPipeline(GraphicsPipeline* pipeline, u32 vertices,
                                          u32 instances, bool indexed) {
    // Still building on a worker (async_pipelines, pso_budget): skipped, unless the draw matters
    // enough to wait for it.
    using VideoCore::FramePipeline::Mode;
    const AsyncPipelines mode = GetAsyncPipelines();
    // The budget belongs to Balanced: under it Wait (Off) used to skip too once the frame's few
    // ms were spent, and Skip (Full) waited, so every choice behaved alike.
    const Mode budget = mode == AsyncPipelines::Balanced ? GetPsoBudget() : Mode::Off;
    const PendingDraw draw{
        .vertices = vertices,
        .instances = instances,
        .indexed = indexed,
        .new_target = HasNewTarget(*texture_cache.GetFramebuffer()),
    };
    // Off waits for every draw, here or in the pipeline cache when no budget is configured.
    const PendingDrawAction unbudgeted = mode == AsyncPipelines::Off
                                             ? PendingDrawAction::WaitBudget
                                             : DecidePendingDraw(mode, GetWaitSmallDraws(), draw);
    PendingDrawAction action = unbudgeted;
    if (budget != Mode::Off) {
        const PendingDrawAction budgeted =
            DecideBudgetedDraw(GetWaitSmallDraws(), draw, PendingWaitBudgetLeft());
        if (budget == Mode::On) {
            action = budgeted;
        } else if ((budgeted == PendingDrawAction::Skip) !=
                   (unbudgeted == PendingDrawAction::Skip)) {
            CountBudgetDifference(budgeted == PendingDrawAction::Skip);
        }
    }
    const auto wait_start = std::chrono::steady_clock::now();
    const auto add_wait = [&wait_start] {
        AddPendingWait(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - wait_start)
                                            .count()));
    };
    if (budget == Mode::On && action == PendingDrawAction::WaitBudget) {
        // The budget has to cut the wait itself: checked only before waiting, one driver build
        // cost 100-330 ms of a frame "within the budget" and the game dropped to 20 fps.
        if (pipeline_cache.WaitPendingFor(pipeline, PendingWaitBudgetRemaining())) {
            CountPendingDraw(action);
            add_wait();
            return pipeline_cache.WaitPending(pipeline) != nullptr;
        }
        add_wait();
        action = DecideBudgetedDraw(GetWaitSmallDraws(), draw, false);
    }
    CountPendingDraw(action);
    if (action == PendingDrawAction::Skip) {
        return false;
    }
    const bool built = pipeline_cache.WaitPending(pipeline) != nullptr;
    add_wait();
    return built;
}

void RasterizerD3D12::DrawVertexTextureRuns(const GraphicsPipeline& pipeline, bool is_indexed,
                                            u32 instance_count) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    using Topology = Maxwell::PrimitiveTopology;
    const GraphicsPipeline::VertexTextureHandle source = *pipeline.GetVertexTextureHandle();
    const auto& regs = maxwell3d->regs;
    auto& draw = maxwell3d->draw_manager.draw_state;
    const auto& attribute = regs.vertex_attrib_format[source.attribute];
    const u32 stream_index = attribute.buffer;
    const auto& stream = regs.vertex_streams[stream_index];
    const bool holds_words = attribute.size == Maxwell::VertexAttribute::Size::Size_R32 ||
                             attribute.size == Maxwell::VertexAttribute::Size::Size_R32_G32 ||
                             attribute.size == Maxwell::VertexAttribute::Size::Size_R32_G32_B32 ||
                             attribute.size == Maxwell::VertexAttribute::Size::Size_R32_G32_B32_A32;
    if (attribute.constant != 0 || !stream.IsEnabled() || !holds_words ||
        source.component >= attribute.ComponentCount()) {
        BUG_TRACK_KEY(DrawSkipped, reinterpret_cast<std::uintptr_t>(&pipeline),
                      "draw skipped: unreadable per-primitive texture handle (attribute {} "
                      "{:#x}, PS {:016x})",
                      source.attribute, attribute.hex, pipeline.Key().unique_hashes[5]);
        return;
    }
    const GPUVAddr words = stream.Address() + attribute.offset + source.component * sizeof(u32);
    const u32 count = is_indexed ? draw.index_buffer.count : draw.vertex_buffer.count;
    const u32 first = is_indexed ? draw.index_buffer.first : draw.vertex_buffer.first;
    const auto& restart = regs.primitive_restart;

    const auto read_vertex = [&](u32 element, u32& vertex) {
        if (!is_indexed) {
            vertex = first + element;
            return true;
        }
        u32 index{};
        if (!draw.inline_index_draw_indexes.empty()) {
            const size_t offset = (size_t{first} + element) * sizeof(u32);
            if (offset + sizeof(u32) > draw.inline_index_draw_indexes.size()) {
                return false;
            }
            std::memcpy(&index, draw.inline_index_draw_indexes.data() + offset, sizeof(u32));
        } else {
            const GPUVAddr address = draw.index_buffer.StartAddress() +
                                     (u64{first} + element) * draw.index_buffer.FormatSizeInBytes();
            switch (draw.index_buffer.format) {
            case Maxwell::IndexFormat::UnsignedByte:
                index = gpu_memory->Read<u8>(address);
                break;
            case Maxwell::IndexFormat::UnsignedShort:
                index = gpu_memory->Read<u16>(address);
                break;
            case Maxwell::IndexFormat::UnsignedInt:
                index = gpu_memory->Read<u32>(address);
                break;
            }
        }
        if (restart.enabled != 0 && index == restart.index) {
            return false;
        }
        vertex = index + draw.base_index;
        return true;
    };
    const auto read_handle = [&](u32 element, u32 previous) {
        u32 vertex{};
        return read_vertex(element, vertex)
                   ? gpu_memory->Read<u32>(words + u64{vertex} * stream.stride)
                   : previous;
    };

    // Primitives of a list stand alone, so a run can start at any of them. Strips and fans cannot
    // be cut without changing their winding: they draw with one texture and report if they had more.
    const u32 primitive = [&]() -> u32 {
        switch (draw.topology) {
        case Topology::Points:
            return 1;
        case Topology::Lines:
            return 2;
        case Topology::Triangles:
            return 3;
        case Topology::Quads:
        case Topology::LinesAdjacency:
            return 4;
        case Topology::TrianglesAdjacency:
            return 6;
        default:
            return 0;
        }
    }();
    const bool per_instance = regs.vertex_stream_instances.IsInstancingEnabled(stream_index);
    const u32 provoking =
        primitive != 0 && regs.provoking_vertex == Maxwell::ProvokingVertex::Last ? primitive - 1 : 0;

    // The state is what the guest would have set for each run: the draw path works as for any draw.
    const auto saved_index = draw.index_buffer;
    const auto saved_vertex = draw.vertex_buffer;
    SCOPE_EXIT {
        draw.index_buffer = saved_index;
        draw.vertex_buffer = saved_vertex;
        if (is_indexed) {
            maxwell3d->dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
        }
    };
    const auto draw_run = [&](u32 run_first, u32 run_count, u32 handle) {
        if (is_indexed) {
            draw.index_buffer.first = first + run_first;
            draw.index_buffer.count = run_count;
            maxwell3d->dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
        } else {
            draw.vertex_buffer.first = first + run_first;
            draw.vertex_buffer.count = run_count;
        }
        DrawRun(is_indexed, instance_count, handle);
    };

    if (per_instance) {
        const u32 handle = gpu_memory->Read<u32>(words + u64{draw.base_instance} * stream.stride);
        if (instance_count > 1) {
            BUG_TRACK_KEY(UnsupportedState, reinterpret_cast<std::uintptr_t>(&pipeline),
                          "per-instance texture handle in a draw of {} instances (PS {:016x})",
                          instance_count, pipeline.Key().unique_hashes[5]);
        }
        draw_run(0, count, handle);
        return;
    }
    if (primitive == 0) {
        const u32 handle = count != 0 ? read_handle(0, 0) : 0;
        for (u32 element = 1; element < count; ++element) {
            if (read_handle(element, handle) != handle) {
                BUG_TRACK_KEY(UnsupportedState, reinterpret_cast<std::uintptr_t>(&pipeline),
                              "several per-primitive texture handles in topology {} (PS {:016x})",
                              static_cast<u32>(draw.topology), pipeline.Key().unique_hashes[5]);
                break;
            }
        }
        draw_run(0, count, handle);
        return;
    }
    if (count < primitive) {
        draw_run(0, count, 0);
        return;
    }
    u32 run_first = 0;
    u32 run_handle = read_handle(provoking, 0);
    for (u32 element = primitive; element + primitive <= count; element += primitive) {
        const u32 handle = read_handle(element + provoking, run_handle);
        if (handle != run_handle) {
            draw_run(run_first, element - run_first, run_handle);
            run_first = element;
            run_handle = handle;
        }
    }
    draw_run(run_first, count - run_first, run_handle);
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    DrawRun(is_indexed, instance_count, std::nullopt);
}

void RasterizerD3D12::DrawRun(bool is_indexed, u32 instance_count,
                              std::optional<u32> vertex_texture_handle) {
    DrainCpuWrites();
    const bool full = VideoCommon::FullCapture::Active();
    if (full) {
        ++full_capture_operation;
        VideoCommon::FullCapture::Event("draw_begin", fmt::format(
            "{{\"op\":{},\"indexed\":{},\"instances\":{}}}",
            full_capture_operation, is_indexed, instance_count));
    }
    ++draw_counter;
    ++zpass_diag.attempted;
    if (maxwell3d->regs.transform_feedback_enabled == 0) {
        stream_output.Disable();
    }
    const u64 full_operation = full_capture_operation;
    bool full_draw_recorded = false;
    SCOPE_EXIT {
        if (full) VideoCommon::FullCapture::Event("draw_call_end", fmt::format(
            "{{\"op\":{},\"recorded\":{}}}", full_operation, full_draw_recorded));
    };
    HuntScope hunt{draw_hunt, is_indexed ? Hunt::Kind::DrawIndexed : Hunt::Kind::Draw};
    if (conditional_rendering.SkipsDraws()) {
        if (full) VideoCommon::FullCapture::Event("draw_conditional_skip");
        ++zpass_diag.conditional;
        hunt.Outcome(Hunt::Outcome::Conditional);
        return;
    }
    DrawMetrics::ScopedDrawTimer draw_timer;
    SCOPE_EXIT {
        scheduler.EndWork();
        FlushIfUploadHeavy();
        gpu.TickWork();
    };
    gpu_memory->FlushCaching();
    ApplyPendingStateInvalidation();
    if (CullsEveryPrimitive()) {
        if (full) VideoCommon::FullCapture::Event("draw_culled");
        ++zpass_diag.culled;
        hunt.Outcome(Hunt::Outcome::Culled);
        if (trace_draws) {
            TraceDraw("draw skipped (both faces culled)", nullptr, nullptr, {}, 0, 0);
        }
        return;
    }

    scheduler.BeginWork();
    hunt.PipelineStart();
    GraphicsPipeline* pipeline = pipeline_cache.CurrentGraphicsPipeline();
    hunt.PipelineEnd();
    if (!pipeline) {
        if (full) VideoCommon::FullCapture::Event("draw_no_pipeline");
        ++zpass_diag.no_pipeline;
        hunt.Outcome(Hunt::Outcome::NoPipeline);
        if (trace_draws) {
            TraceDraw("draw skipped (no pipeline)", nullptr, nullptr, {}, 0, 0);
        }
        return;
    }
    hunt.Shaders(pipeline->Key().unique_hashes[1], pipeline->Key().unique_hashes[5]);
    if (pipeline->GetVertexTextureHandle() && !vertex_texture_handle) [[unlikely]] {
        DrawVertexTextureRuns(*pipeline, is_indexed, instance_count);
        return;
    }
    const DrawMetrics::CacheLockTimer lock_timer;
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    lock_timer.Locked();

    const bool trace_this_draw = trace_draws ||
        material_trace.Select(pipeline->Key().unique_hashes[5]) || full;
    PipelineBindings bindings;
    bindings.vertex_texture_handle = vertex_texture_handle.value_or(0);
    std::vector<VideoCommon::ImageViewId> traced_views;
    std::vector<std::string> traced_filters;
    std::vector<u32> traced_types;
    std::vector<TracedBuffer> traced_buffers;
    FullCaptureViews full_views;
    if (full) bindings.trace_all_views = &full_views;
    if (trace_this_draw) {
        bindings.trace_views = &traced_views;
        bindings.trace_filters = &traced_filters;
        bindings.trace_types = &traced_types;
        if (trace_dumps || material_trace.Active()) {
            buffer_runtime.SetTraceBuffers(&traced_buffers);
        }
    }
    if (material_trace.Active()) {
        const auto& draw = maxwell3d->draw_manager.draw_state;
        const u32 count = is_indexed ? draw.index_buffer.count : draw.vertex_buffer.count;
        if (full || material_snapshots.Targeted(pipeline->Key().unique_hashes[5], count)) {
            // The normal binder skips unchanged VBs. Rebind only the explicit diagnostic draw
            // so its geometry snapshot includes retained bindings, without retaining Buffer*.
            for (u32 index = 0; index < Tegra::Engines::Maxwell3D::Regs::NumVertexArrays; ++index) {
                maxwell3d->dirty.flags[VideoCommon::Dirty::VertexBuffer0 + index] = true;
            }
        }
    }
    const auto& restart = maxwell3d->regs.primitive_restart;
    buffer_runtime.SetIndexRestart(restart.enabled != 0 ? std::optional<u32>{restart.index}
                                                        : std::nullopt);
    pipeline->Configure(is_indexed,
                        {*maxwell3d, *gpu_memory, buffer_cache, texture_cache, descriptor_queue,
                         sampler_heap, scheduler},
                        bindings);
    if ((bindings.unemulated_reduction_stages != 0 || bindings.unemulated_mirror_border))
        [[unlikely]] {
        if (bindings.unemulated_reduction_stages != 0) {
            pipeline_cache.EmulateReduction(pipeline->Key(), bindings.unemulated_reduction_stages);
        }
        if (bindings.unemulated_mirror_border) {
            pipeline_cache.EmulateMirrorBorder(pipeline->Key());
        }
        // This draw too: only the bindings seen just now tell that it needs the other pipeline.
        if (GraphicsPipeline* const emulating = pipeline_cache.CurrentGraphicsPipeline();
            emulating && emulating != pipeline) {
            pipeline = emulating;
            traced_views.clear();
            traced_filters.clear();
            traced_types.clear();
            pipeline->Configure(is_indexed,
                                {*maxwell3d, *gpu_memory, buffer_cache, texture_cache,
                                 descriptor_queue, sampler_heap, scheduler},
                                bindings);
        }
    }
    buffer_runtime.SetTraceBuffers(nullptr);
    if (!pipeline->IsBuilt()) [[unlikely]] {
        const auto& draw = maxwell3d->draw_manager.draw_state;
        if (!WaitPendingPipeline(pipeline,
                                 is_indexed ? draw.index_buffer.count : draw.vertex_buffer.count,
                                 instance_count, is_indexed)) {
            ++zpass_diag.pending;
            hunt.Outcome(Hunt::Outcome::NoPipeline);
            if (trace_this_draw) {
                TraceDraw("draw skipped (pipeline building)", pipeline, nullptr, traced_views, 0,
                          0);
            }
            return;
        }
    }
    if (!pipeline->Handle()) {
        BUG_TRACK_KEY(DrawSkipped, reinterpret_cast<std::uintptr_t>(pipeline),
                      "draw skipped: the pipeline was rejected (VS {:016x} PS {:016x})",
                      pipeline->Key().unique_hashes[1], pipeline->Key().unique_hashes[5]);
        ++zpass_diag.rejected;
        hunt.Outcome(Hunt::Outcome::PsoRejected);
        if (trace_this_draw) {
            TraceDraw("draw skipped (PSO rejected)", pipeline, nullptr, traced_views, 0, 0);
        }
        return; // D3D12 rejected the PSO (logged when it was built)
    }
    if (UsesSkippedShader(pipeline->Key().unique_hashes)) [[unlikely]] {
        ++zpass_diag.skipped_shader;
        hunt.Outcome(Hunt::Outcome::SkippedShader);
        return; // skip_shader
    }
    VideoCore::Perf::ScopedNsTimer record_timer{VideoCore::Perf::Counter::DrawRecordNs};
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    framebuffer->PrepareAttachments(scheduler, bindings.depth_sampled,
                                    pipeline->BoundColorTargets());
    hunt.Target(framebuffer);

    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    DrawParams params{
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .num_instances = instance_count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .base_instance = draw_state.base_instance,
        .is_indexed = is_indexed,
        .runtime_first_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
    };
    const bool emulated_topology =
        !is_indexed && BufferCacheRuntime::IsEmulatedTopology(draw_state.topology);
    hunt.Geometry(params.num_vertices, params.num_instances,
                  static_cast<u32>(draw_state.topology),
                  emulated_topology ? Hunt::EmulatedTopology : 0);
    if (emulated_topology) {
        buffer_runtime.EmulateTopology(draw_state.topology, draw_state.vertex_buffer.first,
                                       draw_state.vertex_buffer.count);
    }
    if (const std::optional<u32> rewritten = buffer_runtime.RewrittenIndexCount()) {
        // Indexed from a rewritten list that starts at index 0; guest indices keep the guest's
        // base vertex, generated ones are absolute vertex numbers.
        params.num_vertices = *rewritten;
        params.first_index = 0;
        params.base_vertex = is_indexed ? draw_state.base_index : 0;
        params.runtime_first_vertex = params.base_vertex;
        params.is_indexed = true;
    }
    if (params.num_vertices == 0 || params.num_instances == 0) {
        ++zpass_diag.empty;
        hunt.Outcome(Hunt::Outcome::Empty);
        buffer_runtime.ApplyGeometry(scheduler.CommandList());
        if (trace_this_draw) {
            TraceDraw("draw skipped (empty)", pipeline, framebuffer, traced_views,
                      params.num_vertices, params.num_instances);
        }
        return;
    }
    if (material_trace.Active() && trace_this_draw && !full) {
        try {
            CaptureMaterialSampledInputs(*pipeline, traced_views, params.num_vertices, *framebuffer);
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material sampled inputs skipped: {}", e.what());
        }
    }
    if (full) {
        CaptureFullPipeline(*pipeline, bindings);
        VideoCommon::FullCapture::Event("draw_geometry", fmt::format(
            "{{\"op\":{},\"vertices\":{},\"instances\":{},\"first_index\":{},"
            "\"base_vertex\":{},\"base_instance\":{},\"indexed\":{},\"runtime_first_vertex\":{}}}",
            full_capture_operation, params.num_vertices, params.num_instances, params.first_index,
            params.base_vertex, params.base_instance, params.is_indexed, params.runtime_first_vertex));
        CaptureFullBuffers(traced_buffers, false);
        CaptureFullImages(full_views, framebuffer, false);
    }
    RecordDraw(*pipeline, bindings, *framebuffer, params, draw_state.topology);
    full_draw_recorded = true;
    if (GetAsyncPipelines() == AsyncPipelines::Balanced) {
        MarkTargetsDrawn(*framebuffer);
    }
    if (material_trace.Active() && pipeline->Layout().StreamOutput() &&
        maxwell3d->regs.transform_feedback_enabled != 0) {
        try {
            CaptureMaterialStreamOutput(*pipeline, params);
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material stream output skipped: {}", e.what());
        }
    }
    if (full) {
        CaptureFullBuffers(traced_buffers, true);
        CaptureFullImages(full_views, framebuffer, true);
        CapturePickProbes(*pipeline, bindings, traced_buffers, *framebuffer, params, draw_state.topology);
        VideoCommon::FullCapture::Event("draw_end", fmt::format("{{\"op\":{}}}", full_capture_operation));
    }
    hunt.Recorded();
    if (zpass_diag.recorded < zpass_diag.vs.size()) {
        zpass_diag.vs[zpass_diag.recorded] = pipeline->Key().unique_hashes[1];
        zpass_diag.ps[zpass_diag.recorded] = pipeline->Key().unique_hashes[5];
        zpass_diag.depth_test[zpass_diag.recorded] = maxwell3d->regs.depth_test_enable;
    }
    ++zpass_diag.recorded;
    if (material_trace.Active() && trace_this_draw) {
        try {
            ProbeMaterialIntermediates(*pipeline, bindings, *framebuffer, params, draw_state.topology);
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material GPU probe skipped: {}", e.what());
        }
    }
    if (material_trace.Active() && trace_this_draw && !full) {
        try {
            CaptureMaterialInputs(*pipeline, traced_buffers, params);
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material inputs skipped: {}", e.what());
        }
    }
    if (trace_this_draw) {
        if (material_trace.Active()) ProbeMaterialDraw(*pipeline, *framebuffer);
        if (!traced_filters.empty()) {
            LogDrawTrace(fmt::format("D3D12 trace #{} samplers: {} types: {}", trace_index,
                         fmt::join(traced_filters, " / "), fmt::join(traced_types, " ")));
        }
        TraceDraw(params.is_indexed ? "draw indexed" : "draw", pipeline, framebuffer,
                  traced_views, params.num_vertices, params.num_instances);
        if (trace_dumps) {
            CheckTracedBuffers(traced_buffers, params, trace_non_finite_grew);
        }
    }
    if (!logged_first_draw) {
        LOG_INFO(Render,
                 "D3D12: first guest draw recorded ({} {} vertices, {} instances, {} RTs{})",
                 params.is_indexed ? "indexed" : "non-indexed", params.num_vertices,
                 params.num_instances, framebuffer->ColorTargets().size(),
                 framebuffer->DepthTarget().ptr ? ", depth" : "");
        logged_first_draw = true;
    }
}

void RasterizerD3D12::RecordDraw(GraphicsPipeline& pipeline,
                                 const PipelineBindings& bindings, const Framebuffer& framebuffer,
                                 const DrawParams& params,
                                 Maxwell::PrimitiveTopology topology) {
    const auto compatible = texture_runtime.PrepareCompatibleDrawTargets(
        framebuffer, pipeline.BoundColorTargets());
    BindDrawState(pipeline, bindings, framebuffer, params, topology, compatible);
    CommandStream* const cmd = scheduler.CommandList();
    VideoCore::Perf::Add(VideoCore::Perf::Counter::Draws, 1);
    if (pipeline.GeneratesGeometry()) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::GeneratedGeometryDraws, 1);
    }
    if (pipeline.TessPlan().Active()) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::TessDraws, 1);
        RecordTessellatedDraw(pipeline, bindings, params);
    } else {
        query_runtime.PrepareDraw(maxwell3d->regs.zpass_pixel_count_enable);
        const ConditionalRendering::Scope predicate{conditional_rendering, cmd};
        DrawStencilFaces(pipeline, topology, [&] {
            if (params.is_indexed) {
                cmd->DrawIndexedInstanced(params.num_vertices, params.num_instances,
                                          params.first_index,
                                          static_cast<INT>(params.base_vertex),
                                          params.base_instance);
            } else {
                cmd->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                                   params.base_instance);
            }
        });
    }
    texture_runtime.FinishCompatibleDrawTargets(framebuffer);
}

void RasterizerD3D12::RecordTessellatedDraw(GraphicsPipeline& pipeline,
                                            const PipelineBindings& bindings,
                                            const DrawParams& params) {
    CommandStream* const cmd = scheduler.CommandList();
    const PipelineLayout& layout = pipeline.Layout();
    const ConditionalRendering::Scope predicate{conditional_rendering, cmd};
    TessEmulation::DrawShape shape{
        .num_vertices = params.num_vertices,
        .num_instances = params.num_instances,
        .first_index = params.first_index,
        .base_vertex = params.base_vertex,
        .base_instance = params.base_instance,
        .is_indexed = params.is_indexed,
        .index_address = 0,
        .index_size = 0,
        .index_slots = 0,
    };
    if (params.is_indexed) {
        const std::optional<D3D12_INDEX_BUFFER_VIEW>& view = buffer_runtime.AppliedIndex();
        if (!view) [[unlikely]] {
            BUG_TRACK(UnsupportedState, "tessellated indexed draw without an index buffer");
            return;
        }
        shape.index_address = view->BufferLocation;
        shape.index_size = view->Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        shape.index_slots = TessIndexSlots();
    }
    // The emulation's own passes are not the guest's draw: only its last one is counted.
    query_runtime.PauseCounters();
    const u32 patches = tess_emulation.Capture(cmd, pipeline, shape);
    command_state.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (patches != 0) {
        cmd->SetComputeRootSignature(layout.Handle());
        SetTessControlRootArguments(cmd, pipeline, bindings);
        tess_emulation.Control(cmd, pipeline, patches);
        tess_emulation.Tessellate(cmd, pipeline, patches, command_state.viewport.yz_flip_mask, 0);
        scheduler.SetPipelineState(pipeline.Handle());
        query_runtime.PrepareDraw(maxwell3d->regs.zpass_pixel_count_enable);
        command_state.topology = tess_emulation.Draw(cmd, pipeline);
        // The record wrote the runtime data constants, and the index buffer is the emulation's.
        command_state.root_args.valid = false;
        buffer_runtime.InvalidateGeometry();
    }
}

u32 RasterizerD3D12::TessIndexSlots() const {
    // The largest index the draw reads, from the guest's indices: the rewritten ones keep their
    // values. All ones is the restart index, which names no vertex.
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const auto& index_buffer = draw_state.index_buffer;
    const size_t size = index_buffer.FormatSizeInBytes();
    const size_t bytes = size_t{index_buffer.count} * size;
    std::vector<u8> guest;
    std::span<const u8> indices;
    if (!draw_state.inline_index_draw_indexes.empty()) {
        const std::span<const u8> inline_indices{draw_state.inline_index_draw_indexes};
        const size_t first = std::min(size_t{index_buffer.first} * size, inline_indices.size());
        indices = inline_indices.subspan(first, std::min(bytes, inline_indices.size() - first));
    } else {
        guest.resize(bytes);
        gpu_memory->ReadBlockUnsafe(index_buffer.IndexStart(), guest.data(), bytes);
        indices = guest;
    }
    const auto largest = [&]<typename T>(T) {
        T max{};
        for (size_t offset = 0; offset + sizeof(T) <= indices.size(); offset += sizeof(T)) {
            T value;
            std::memcpy(&value, indices.data() + offset, sizeof(T));
            if (value != static_cast<T>(~T{})) {
                max = std::max(max, value);
            }
        }
        return u64{max} + 1;
    };
    const u64 slots = size == 1 ? largest(u8{}) : size == 2 ? largest(u16{}) : largest(u32{});
    return static_cast<u32>(std::min<u64>(slots, ~0u));
}

void RasterizerD3D12::DispatchCompute() {
    DrainCpuWrites();
    const bool full = VideoCommon::FullCapture::Active();
    if (full) {
        ++full_capture_operation;
        VideoCommon::FullCapture::Event("dispatch_begin", fmt::format("{{\"op\":{}}}", full_capture_operation));
    }
    const u64 full_operation = full_capture_operation;
    SCOPE_EXIT {
        if (full) VideoCommon::FullCapture::Event("dispatch_call_end", fmt::format("{{\"op\":{}}}", full_operation));
    };
    ++draw_counter;
    HuntScope hunt{draw_hunt, Hunt::Kind::Dispatch};
    VideoCore::Perf::ScopedNsTimer dispatch_timer{VideoCore::Perf::Counter::DispatchNs};
    SCOPE_EXIT {
        scheduler.EndWork();
        FlushIfUploadHeavy();
        // As after a draw: a guest core waiting on a flush is not held up by a compute batch.
        gpu.TickWork();
    };
    gpu_memory->FlushCaching();
    ApplyPendingStateInvalidation();

    scheduler.BeginWork();
    hunt.PipelineStart();
    ComputePipeline* pipeline = pipeline_cache.CurrentComputePipeline();
    hunt.PipelineEnd();
    if (!pipeline) {
        hunt.Outcome(Hunt::Outcome::NoPipeline);
        return;
    }
    hunt.Shaders(0, pipeline->UniqueHash());
    // Diagnostic: how often each compute shader is dispatched (logged at 1, 2, 4, 8... and
    // every 1024th dispatch, so the last line per hash approximates its total).
    static std::unordered_map<u64, u64> dispatch_counts;
    const u64 dispatch_count = ++dispatch_counts[pipeline->UniqueHash()];
    {
        const u64 hash = pipeline->UniqueHash();
        const u64 count = dispatch_count;
        if ((count & (count - 1)) == 0 || (count & 1023) == 0) {
            LOG_WARNING(Render, "D3D12: compute shader {:016x} dispatched {} times", hash, count);
        }
        NoteComputeDispatch(hash, count);
    }
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    PipelineBindings bindings;
    std::vector<TracedBuffer> full_buffers;
    FullCaptureViews full_views;
    std::vector<std::string> full_samplers;
    if (full) {
        bindings.trace_all_views = &full_views;
        bindings.trace_filters = &full_samplers;
    }
    const bool dump_inputs = ComputeInputsWanted(pipeline->UniqueHash(), dispatch_count);
    if (full || dump_inputs) {
        buffer_runtime.SetTraceBuffers(&full_buffers);
        pipeline->WaitBuilt(); // the captures read its DXIL
    }
    // Still building on a worker: the dispatch is recorded all the same, and the recorder waits
    // for the PSO before it binds it (pipeline_cache.CurrentComputePipeline).
    bool pending = !pipeline->IsBuilt();
    SCOPE_EXIT { buffer_runtime.SetTraceBuffers(nullptr); };
    pipeline->Configure({*kepler_compute, *gpu_memory, buffer_cache, texture_cache,
                         descriptor_queue, sampler_heap, scheduler},
                        bindings);
    if (bindings.unemulated_reduction_stages != 0) [[unlikely]] {
        pipeline_cache.EmulateReduction(pipeline->UniqueHash());
        // This dispatch too: only the bindings seen just now tell that it needs the other one.
        if (ComputePipeline* const emulating = pipeline_cache.CurrentComputePipeline();
            emulating && emulating != pipeline) {
            pipeline = emulating;
            pending = !pipeline->IsBuilt();
            pipeline->Configure({*kepler_compute, *gpu_memory, buffer_cache, texture_cache,
                                 descriptor_queue, sampler_heap, scheduler},
                                bindings);
        }
    }
    if (!pending && !pipeline->Handle()) {
        hunt.Outcome(Hunt::Outcome::PsoRejected);
        return; // D3D12 rejected the PSO (logged when it was built)
    }
    hunt.Setup(Hunt::Kind::DispatchSetup);
    hunt.Target(nullptr);

    const auto& qmd = kepler_compute->launch_description;
    const PipelineLayout& layout = pipeline->Layout();
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    if (dump_inputs) {
        DumpComputeInputs(scheduler, staging, full_buffers, pipeline->UniqueHash(),
                          dispatch_count, dim, false);
    }
    if (full) {
        const auto meta = fmt::format("{{\"op\":{},\"shader\":{}}}", full_capture_operation, pipeline->UniqueHash());
        VideoCommon::FullCapture::Data("compute_dxil", meta, pipeline->DiagnosticDxil());
        VideoCommon::FullCapture::Data("compute_launch", meta,
            {reinterpret_cast<const u8*>(&qmd), sizeof(qmd)});
        VideoCommon::FullCapture::Data("compute_push_constants", meta,
            {reinterpret_cast<const u8*>(bindings.push_constants.data()), sizeof(bindings.push_constants)});
        VideoCommon::FullCapture::Event("compute_root_layout", fmt::format(
            "{{\"op\":{},\"runtime_words\":{},\"resource_index\":{},\"sampler_index\":{},"
            "\"integer_index\":{},\"resource_count\":{},\"sampler_count\":{}}}",
            full_capture_operation, layout.RuntimeDataWords(), layout.ResourceTableIndex(),
            layout.SamplerTableIndex(), layout.IntegerSamplerIndex(), layout.NumResourceDescriptors(),
            layout.NumSamplerDescriptors()));
        const auto descriptors = [&](std::string_view kind, const auto& list) {
            if (list.empty()) return;
            VideoCommon::FullCapture::Data("compute_descriptors", fmt::format(
                "{{\"op\":{},\"kind\":\"{}\",\"count\":{}}}", full_capture_operation, kind, list.size()),
                {reinterpret_cast<const u8*>(list.data()), list.size() * sizeof(list[0])});
        };
        const auto& info = pipeline->Info();
        descriptors("uniform", info.constant_buffer_descriptors);
        descriptors("storage", info.storage_buffers_descriptors);
        descriptors("texel", info.texture_buffer_descriptors);
        descriptors("image_buffer", info.image_buffer_descriptors);
        descriptors("texture", info.texture_descriptors);
        descriptors("image", info.image_descriptors);
        for (const auto& sampler : full_samplers) VideoCommon::FullCapture::Text("compute_sampler", sampler);
        CaptureFullBuffers(full_buffers, false);
        CaptureFullImages(full_views, nullptr, false);
    }
    bool dispatched = false;
    SCOPE_EXIT {
        if (dump_inputs && dispatched) {
            DumpComputeInputs(scheduler, staging, full_buffers, pipeline->UniqueHash(),
                              dispatch_count, dim, true);
        }
        if (full) {
            if (dispatched) {
                CaptureFullBuffers(full_buffers, true);
                CaptureFullImages(full_views, nullptr, true);
            }
            VideoCommon::FullCapture::Event("dispatch_end", fmt::format(
                "{{\"op\":{},\"recorded\":{}}}", full_capture_operation, dispatched));
        }
    };
    std::optional<IndirectArgumentRing::Range> indirect_range;
    if (const std::optional<GPUVAddr> indirect = kepler_compute->GetIndirectComputeAddress()) {
        // The group counts are also runtime data (root constants): the record carries them
        // twice, once for the constants and once for the dispatch, both copied on the GPU. The
        // D3D12 limit of 65535 groups cannot be checked here.
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(
            *indirect, 3 * sizeof(u32), VideoCommon::ObtainBufferSynchronize::FullSynchronize,
            VideoCommon::ObtainBufferOperation::DiscardWrite);
        if (full && buffer) {
            const TracedBuffer args{.kind = TracedBuffer::Kind::Storage, .buffer = buffer,
                .offset = static_cast<u32>(offset), .size = 3 * sizeof(u32),
                .device_addr = buffer->CpuAddr() + offset};
            CaptureFullBuffers({&args, 1}, false);
            VideoCommon::FullCapture::Event("dispatch_indirect", fmt::format("{{\"address\":{}}}", *indirect));
        }
        if (!layout.DispatchSignature() || !buffer || !buffer->Handle()) {
            return;
        }
        indirect_range = indirect_args.Allocate(INDIRECT_DISPATCH_WORDS * sizeof(u32));
        buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
        CommandStream* const copy_cmd = scheduler.CommandList();
        for (u32 copy = 0; copy < 2; ++copy) {
            copy_cmd->CopyBufferRegion(
                indirect_range->buffer,
                indirect_range->offset + copy * INDIRECT_DISPATCH_CONSTANT_WORDS * sizeof(u32),
                buffer->Handle(), offset, 3 * sizeof(u32));
        }
        indirect_args.Ready(*indirect_range);
    } else {
        constexpr u32 max_dim = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
        if (dim[0] == 0 || dim[1] == 0 || dim[2] == 0) {
            return;
        }
        if (dim[0] > max_dim || dim[1] > max_dim || dim[2] > max_dim) {
            BUG_TRACK(DrawSkipped, "dispatch of {}x{}x{} groups exceeds the D3D12 limit, skipped",
                      dim[0], dim[1], dim[2]);
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_WARNING(Render, "D3D12: dispatch of {}x{}x{} groups exceeds the limit, skipped",
                        dim[0], dim[1], dim[2]);
            return;
        }
    }

    CommandStream* const cmd = scheduler.CommandList();
    if (!command_state.heaps_bound) {
        ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
        cmd->SetDescriptorHeaps(2, heaps);
        command_state.heaps_bound = true;
    }
    cmd->SetComputeRootSignature(layout.Handle());
    if (!pending) {
        scheduler.SetPipelineState(pipeline->Handle());
    }
    MarkGpuCommands(cmd, "dispatch CS", pipeline->UniqueHash(), 0);
    if (GpuDiagnosticsEnabled()) {
        TraceGpuDetail(fmt::format("dispatch tick={} PSO={} CS={:016x} groups={}x{}x{} table={:x}",
            scheduler.CurrentTick(), static_cast<void*>(pipeline->Handle()), pipeline->UniqueHash(),
            dim[0], dim[1], dim[2], bindings.resource_table.ptr));
    }
    cmd->SetComputeRoot32BitConstants(PipelineLayout::PUSH_CONSTANTS_INDEX, PUSH_CONSTANT_WORDS,
                                      bindings.push_constants.data(), 0);
    dxil_spirv_compute_runtime_data runtime_data{};
    runtime_data.group_count_x = dim[0];
    runtime_data.group_count_y = dim[1];
    runtime_data.group_count_z = dim[2];
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> runtime_words{};
    std::memcpy(runtime_words.data(), &runtime_data, sizeof(runtime_data));
    if (full) VideoCommon::FullCapture::Data("compute_runtime",
        fmt::format("{{\"op\":{}}}", full_capture_operation),
        {reinterpret_cast<const u8*>(&runtime_data), sizeof(runtime_data)});
    cmd->SetComputeRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX,
                                      std::min<UINT>(layout.RuntimeDataWords(),
                                                     static_cast<UINT>(runtime_words.size())),
                                      runtime_words.data(), 0);
    if (layout.ResourceTableIndex() != PipelineLayout::NO_TABLE) {
        cmd->SetComputeRootDescriptorTable(layout.ResourceTableIndex(), bindings.resource_table);
    }
    if (layout.SamplerTableIndex() != PipelineLayout::NO_TABLE) {
        cmd->SetComputeRootDescriptorTable(layout.SamplerTableIndex(), bindings.sampler_table);
    }
    if (layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE && bindings.integer_samplers) {
        cmd->SetComputeRootConstantBufferView(layout.IntegerSamplerIndex(),
                                              bindings.integer_samplers);
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::Dispatches, 1);
    const u32 groups_x = dim[0] * pipeline->WorkgroupSplit();
    if (pending) {
        // One call: without a PSO (rejected, out of memory) the dispatch is left out with it.
        ID3D12CommandSignature* const signature = layout.DispatchSignature();
        ID3D12Resource* const arguments = indirect_range ? indirect_range->buffer : nullptr;
        const UINT64 arguments_offset = indirect_range ? indirect_range->offset : 0;
        if (indirect_range &&
            !ValidExecuteIndirect(signature, arguments, arguments_offset, nullptr, 0)) {
            return;
        }
        scheduler.ForgetPipelineState();
        cmd->Record([pipeline, signature, arguments, arguments_offset, groups_x, dim,
                     indirect = indirect_range.has_value()](ID3D12GraphicsCommandList* list) {
            ID3D12PipelineState* const pso = pipeline->WaitHandle();
            if (!pso) {
                return;
            }
            list->SetPipelineState(pso);
            if (indirect) {
                list->ExecuteIndirect(signature, 1, arguments, arguments_offset, nullptr, 0);
            } else {
                list->Dispatch(groups_x, dim[1], dim[2]);
            }
        });
        dispatched = true;
        hunt.Geometry(indirect_range ? 0 : dim[0] * dim[1] * dim[2], 1, 0);
        hunt.Recorded();
        return;
    }
    if (indirect_range) {
        cmd->ExecuteIndirect(layout.DispatchSignature(), 1, indirect_range->buffer,
                             indirect_range->offset, nullptr, 0);
        dispatched = true;
        hunt.Geometry(0, 1, 0); // the group count is on the GPU
        hunt.Recorded();
        if (!logged_indirect_dispatch) {
            LOG_INFO(Render, "D3D12: first indirect dispatch recorded (groups of {}x{}x{})",
                     qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z);
            logged_indirect_dispatch = true;
        }
        return;
    }
    // A split shader runs each guest group as WorkgroupSplit() real ones; the runtime data above
    // keeps the guest's count.
    cmd->Dispatch(groups_x, dim[1], dim[2]);
    dispatched = true;
    hunt.Geometry(dim[0] * dim[1] * dim[2], 1, 0);
    hunt.Recorded();
    if (!logged_first_dispatch) {
        LOG_INFO(Render, "D3D12: first guest dispatch recorded ({}x{}x{} groups of {}x{}x{})",
                 dim[0], dim[1], dim[2], qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z);
        logged_first_dispatch = true;
    }
}

std::optional<RasterizerD3D12::DisplayTexture> RasterizerD3D12::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr) {
    if (!framebuffer_addr) {
        return std::nullopt;
    }
    DrainCpuWrites();
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (!image_view) {
        return std::nullopt;
    }
    image_view->TransitionImage(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return DisplayTexture{
        .srv = image_view->Handle(Shader::TextureType::Color2D),
        .width = image_view->size.width,
        .height = image_view->size.height,
    };
}

bool RasterizerD3D12::AccelerateConditionalRendering() {
    return conditional_rendering.Accelerate(maxwell3d->regs, *gpu_memory);
}

void RasterizerD3D12::ResetCounter(VideoCommon::QueryType type) {
    if (type == VideoCommon::QueryType::ZPassPixelCount64) zpass_diag = {};
    if (QueryCacheRuntime::IsHostCounted(type)) query_cache.CounterReset(type);
}

void RasterizerD3D12::Query(GPUVAddr address, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload,
                            u32 subreport) {
    const bool host_type = QueryCacheRuntime::IsHostCounted(type);
    {
        // Diagnostic: every guest query by raw type, logged at 1, 2, 4, 8... requests.
        static std::array<std::atomic<u64>, 32> query_counts{};
        const u32 slot = static_cast<u32>(type) & 31;
        const u64 count = ++query_counts[slot];
        if ((count & (count - 1)) == 0 || (host_type && count <= 80)) {
            LOG_WARNING(Render,
                        "D3D12: guest query type {} flags {:#x} payload {:#x} addr {:#x} "
                        "({}), request #{}",
                        static_cast<u32>(type), static_cast<u32>(flags), payload, address,
                        host_type ? "host counter" : "fallback", count);
        }
    }
    if (type == VideoCommon::QueryType::ZPassPixelCount64) {
        static u32 diag_logged{};
        if (diag_logged++ < 40) {
            const ZPassDiag& d = zpass_diag;
            LOG_WARNING(Render,
                        "D3D12: ZPass report since reset: {} draws ({} recorded, {} conditional, "
                        "{} culled, {} no pipeline, {} pipeline building, {} rejected, {} skipped "
                        "shader, {} empty); first recorded VS/PS/depth: {:016x}/{:016x}/{} "
                        "{:016x}/{:016x}/{} {:016x}/{:016x}/{}",
                        d.attempted, d.recorded, d.conditional, d.culled, d.no_pipeline,
                        d.pending, d.rejected, d.skipped_shader, d.empty, d.vs[0], d.ps[0], d.depth_test[0],
                        d.vs[1], d.ps[1], d.depth_test[1], d.vs[2], d.ps[2], d.depth_test[2]);
        }
    }
    if (type == VideoCommon::QueryType::StreamingByteCount && HasDrawTransformFeedback()) {
        return QueryByteCount(address, flags, subreport);
    }
    if (!host_type) {
        // A report the CPU writes ends any counter report pending at the address.
        if (const std::optional<DAddr> cpu_address = gpu_memory->GpuToCpuAddress(address)) {
            query_cache.InvalidateRegion(*cpu_address, sizeof(u64));
        }
        return QueryFallback(address, type, flags, payload);
    }
    query_cache.CounterReport(address, type, flags, payload, subreport);
}

bool RasterizerD3D12::HasDrawTransformFeedback() {
    return pipeline_cache.NativeStreamOutput();
}

void RasterizerD3D12::QueryByteCount(GPUVAddr address, VideoCommon::QueryPropertiesFlags flags,
                                     u32 stream) {
    // Written on the GPU, ordered after the captures, into the guest memory the buffer cache
    // holds (as the Vulkan backend's SyncValues): DrawAuto reads it there without a stall.
    const bool timeout = True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout);
    std::scoped_lock lock{buffer_cache.mutex};
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(
        address, timeout ? 16 : 4, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
        VideoCommon::ObtainBufferOperation::MarkAsWritten);
    if (!buffer || !buffer->Handle()) {
        return QueryFallback(address, VideoCommon::QueryType::Payload, flags, 0);
    }
    CommandStream* const cmd = scheduler.CommandList();
    buffer->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_GPU_VIRTUAL_ADDRESS base = buffer->Address() + offset;
    std::array<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, 4> words{};
    UINT num_words = 0;
    // No capture with the current registers wrote the stream: nothing was written.
    if (const std::optional<u32> slot = stream_output.ByteCountSlot(maxwell3d->regs, stream)) {
        stream_output.CopyByteCount(cmd, *slot, buffer->Handle(), offset);
    } else {
        words[num_words++] = {.Dest = base, .Value = 0};
    }
    if (timeout) {
        const u64 ticks = gpu.GetTicks();
        words[num_words++] = {.Dest = base + 4, .Value = 0};
        words[num_words++] = {.Dest = base + 8, .Value = static_cast<u32>(ticks)};
        words[num_words++] = {.Dest = base + 12, .Value = static_cast<u32>(ticks >> 32)};
    }
    if (num_words != 0) {
        cmd->WriteBufferImmediate(num_words, words.data());
    }
}

void RasterizerD3D12::QueryFallback(GPUVAddr address, VideoCommon::QueryType type,
                                    VideoCommon::QueryPropertiesFlags flags, u32 payload) {
    if (!gpu_memory) return;
    if (type != VideoCommon::QueryType::Payload) payload = 1;
    auto operation = [this, address, flags, payload, memory = gpu_memory] {
        if (True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout)) {
            memory->Write<u64>(address + 8, gpu.GetTicks());
            memory->Write<u64>(address, payload);
        } else {
            memory->Write<u32>(address, payload);
        }
    };
    if (True(flags & VideoCommon::QueryPropertiesFlags::IsAFence)) {
        SignalFence(std::move(operation));
    } else {
        operation();
    }
}

void RasterizerD3D12::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr address,
                                                u32 size) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, address, size);
}
void RasterizerD3D12::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}
void RasterizerD3D12::FlushAll() { scheduler.Finish(); }

bool RasterizerD3D12::MaterializeCheckpointMemory() {
    // Do not hold a guest scheduler lock across these waits. This follows the renderer's existing
    // cache-lock order; callbacks retired by Scheduler must not take either cache mutex.
    scheduler.Finish();
    fence_manager.WaitPendingFences(true);
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    texture_cache.ValidateCheckpointMemory([this](DAddr address, u64 size) {
        return buffer_cache.IsRegionRegistered(address, size);
    });
    buffer_cache.FlushCachedWrites();
    // Reject a conflict rather than choose an arbitrary winner for buffer/texture aliases.
    // The cache readbacks remain separate; cross-cache authority requires a later merge gate.
    if (buffer_cache.HasCheckpointOverlap([this](DAddr address, u64 size) {
            return texture_cache.IsRegionRegisteredForCheckpoint(address, size);
        })) {
        throw std::runtime_error("Pulse: overlapping authoritative buffer and texture writes");
    }
    texture_cache.DownloadCheckpointMemory();
    buffer_cache.DownloadCheckpointMemory();
    scheduler.Finish();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(),
                  "Pulse materialize GPU memory");
    return true;
}

void RasterizerD3D12::FlushCheckpointRegion(DAddr address, u64 size) {
    if (!address || !size) return;
    // What each cache's download leaves in the app's memory, for the save's peak log.
    const auto used = [this] { return device.QueryCacheMemoryPressure().app_used; };
    const auto mib = [](u64 after, u64 before) {
        return (static_cast<s64>(after) - static_cast<s64>(before)) / (1 << 20);
    };
    const u64 start = used();
    size_t images{};
    u64 largest_image{}, swizzle{}, pooled{};
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.checkpoint_downloads = {};
        texture_cache.DownloadMemory(address, size, true);
        images = texture_cache.checkpoint_downloads.images;
        largest_image = texture_cache.checkpoint_downloads.largest;
        pooled = texture_cache.checkpoint_downloads.pooled;
        swizzle = texture_cache.SwizzleScratchBytes();
    }
    const u64 textures = used();
    u64 largest_buffer{};
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.largest_download = 0;
        buffer_cache.DownloadMemory(address, size);
        largest_buffer = buffer_cache.largest_download;
        staging.ReleaseRetiredDownloads();
    }
    const u64 buffers = used();
    query_cache.FlushRegion(address, size);
    if (start && (mib(textures, start) || mib(buffers, textures))) {
        LOG_INFO(Render,
                 "Pulse: flush of {:#x}+{} MiB: {} images (largest {} MiB, transfers {} MiB, swizzle "
                 "scratch {} MiB) {:+} MiB, buffers (largest {} MiB) {:+} MiB",
                 address, size >> 20, images, largest_image >> 20, pooled >> 20, swizzle >> 20,
                 mib(textures, start), largest_buffer >> 20, mib(buffers, textures));
    }
}

void RasterizerD3D12::FlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (!address || !size) return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) query_cache.FlushRegion(address, size);
}

bool RasterizerD3D12::MustFlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(address, size)) return true;
    }
    if (Settings::IsGPULevelHigh() && True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(address, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea(DAddr address, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        if (auto area = texture_cache.GetFlushArea(address, size)) return *area;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (auto area = buffer_cache.GetFlushArea(address, size)) return *area;
    }
    return {.start_address = Common::AlignDown(address, Core::DEVICE_PAGESIZE),
            .end_address = Common::AlignUp(address + size, Core::DEVICE_PAGESIZE),
            .preemtive = true};
}

void RasterizerD3D12::InvalidateRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (!address || !size) return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) query_cache.InvalidateRegion(address, size);
    if (True(which & VideoCommon::CacheType::ShaderCache)) pipeline_cache.InvalidateRegion(address, size);
}

void RasterizerD3D12::InnerInvalidation(
    std::span<const std::pair<DAddr, std::size_t>> sequences) {
    // The ranges written through the GPU memory manager since the last draw: one lock per cache
    // for the batch instead of two per range, with the same caches and setting as the default
    // per-range InvalidateRegion.
    if (Settings::values.skip_cpu_inner_invalidation.GetValue()) {
        return;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [address, size] : sequences) {
            if (address && size) texture_cache.WriteMemory(address, size);
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [address, size] : sequences) {
            if (address && size) buffer_cache.WriteMemory(address, size);
        }
    }
    for (const auto& [address, size] : sequences) {
        if (!address || !size) continue;
        query_cache.InvalidateRegion(address, size);
        pipeline_cache.InvalidateRegion(address, size);
    }
}

void RasterizerD3D12::DrainCpuWrites() {
    if (VideoCommon::CpuWriteQueue::Empty()) {
        return;
    }
    cpu_writes_drained.clear();
    u64 late = 0;
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        const u64 drained = VideoCommon::CpuWriteQueue::Drain([&](u64 address, u64 size) {
            // OnCPUWrite, with the locks taken once for the batch.
            if (buffer_cache.OnCPUWrite(address, size)) {
                cpu_writes_after_gpu.emplace_back(address, size);
                ++late;
                return;
            }
            texture_cache.WriteMemory(address, size);
            cpu_writes_drained.emplace_back(address, size);
        });
        VideoCore::Perf::Add(VideoCore::Perf::Counter::CpuWritesDrained, drained);
    }
    if (late != 0) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::CpuWritesLateGpu, late);
    }
    for (const auto& [address, size] : cpu_writes_drained) {
        pipeline_cache.InvalidateRegion(address, size);
    }
}

bool RasterizerD3D12::OnCPUWrite(PAddr address, u64 size) {
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(address, size)) return true;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(address, size);
    }
    pipeline_cache.InvalidateRegion(address, size);
    return false;
}

void RasterizerD3D12::OnCacheInvalidation(PAddr address, u64 size) {
    InvalidateRegion(address, size, VideoCommon::CacheType::TextureCache |
                                        VideoCommon::CacheType::BufferCache);
}
void RasterizerD3D12::InvalidateGPUCache() {
    DrainCpuWrites();
    for (const auto& [address, size] : cpu_writes_after_gpu) {
        OnCacheInvalidation(address, size);
    }
    cpu_writes_after_gpu.clear();
    gpu.InvalidateGPUCache();
}
void RasterizerD3D12::UnmapMemory(DAddr address, u64 size) {
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.UnmapMemory(address, size); }
    { std::scoped_lock lock{buffer_cache.mutex}; buffer_cache.WriteMemory(address, size); }
    pipeline_cache.OnCacheInvalidation(address, size);
}
void RasterizerD3D12::ModifyGPUMemory(size_t as_id, GPUVAddr address, u64 size) {
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, address, size);
}
void RasterizerD3D12::SignalFence(std::function<void()>&& f) { fence_manager.SignalFence(std::move(f)); }
void RasterizerD3D12::SyncOperation(std::function<void()>&& f) { fence_manager.SyncOperation(std::move(f)); }
void RasterizerD3D12::SignalSyncPoint(u32 value) { fence_manager.SignalSyncPoint(value); }
void RasterizerD3D12::SignalReference() { fence_manager.SignalOrdering(); }
void RasterizerD3D12::ReleaseFences(bool force) { fence_manager.WaitPendingFences(force); }
void RasterizerD3D12::FlushAndInvalidateRegion(DAddr a, u64 s, VideoCommon::CacheType w) {
    if (Settings::IsGPULevelHigh()) FlushRegion(a, s, w);
    InvalidateRegion(a, s, w);
}
void RasterizerD3D12::WaitForIdle() {
    // As Vulkan's (an event set and waited on inside the command buffer): a GPU-side barrier,
    // never a CPU wait. Games issue wait_for_idle dozens of times a frame; submitting and waiting
    // for the GPU on each one took about half of every frame (0.2.49 perf counters). One queue
    // runs in order and the caches transition what they hand over, so what is left to order are
    // UAV writes: a UAV barrier on every resource.
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                         .UAV = {.pResource = nullptr}};
    scheduler.CommandList()->ResourceBarrier(1, &barrier);
    // Counter reports read after the wait are summed where the GPU reads them.
    query_cache.NotifyWFI();
    fence_manager.SignalOrdering();
}
void RasterizerD3D12::FragmentBarrier() {
    // Fragment shaders' storage writes before later reads.
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                         .UAV = {.pResource = nullptr}};
    scheduler.CommandList()->ResourceBarrier(1, &barrier);
}
void RasterizerD3D12::TiledCacheBarrier() {
    // Nothing: the host has no tiled cache to flush (Vulkan does the same).
}
void RasterizerD3D12::FlushCommands() {
    // Called at the end of every guest command list: submit when something was drawn, or when
    // enough was uploaded. Loading screens upload thousands of small buffers without drawing;
    // left unsubmitted, the staging stream cannot be reused and every upload became a new
    // committed buffer until the console ran out of memory (0.2.49, 125 s into Game C).
    constexpr u64 UPLOAD_FLUSH_BYTES = 8ULL << 20;
    if (draw_counter == 0 && staging.PendingUploadBytes() < UPLOAD_FLUSH_BYTES &&
        texture_runtime.PendingTransferBytes() < UPLOAD_FLUSH_BYTES) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}
void RasterizerD3D12::FlushIfUploadHeavy() {
    if (removal_tripwire::removal_tripped.load(std::memory_order_relaxed)) return;
    // A loading frame may upload more textures than the staging stream holds within one command
    // list (151 MiB in Game C; the stream is 256 MiB, and the memory guard shrinks it down to
    // nothing under pressure), and the stream only reuses submitted regions. The
    // reset callback invalidates cached state, so submitting between draws is safe.
    // The threshold follows the ring size (StagingBufferPool::UploadSubmitBytes, at most 32 MiB).
    const u64 upload_submit_bytes = staging.UploadSubmitBytes();
    // A game drawing ~3900 times in one guest command list had the host GPU start only once the
    // whole list was recorded (~50 ms), and the guest waited for both in turn every frame.
    // Submitting every few hundred draws lets the GPU execute while the rest is recorded.
    const u32 submit_after = submit_draws.load(std::memory_order_relaxed);
    // Copy scratch past the transfer pool's budget is freed only with the list that used it: a
    // loading screen's conversions held ~600 MiB of 8 MiB buffers in one list and ran out.
    constexpr u64 TRANSFER_SUBMIT_BYTES = 32ULL << 20;
    if (staging.PendingUploadBytes() < upload_submit_bytes && draw_counter < submit_after &&
        texture_runtime.PendingTransferBytes() < TRANSFER_SUBMIT_BYTES) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}
void RasterizerD3D12::TickFrame() {
    Common::BugTracker::TickFrame();
    DrainCpuWrites();
    draw_counter = 0;
    ResetPendingWaitBudget();
    if (GetPsoLoading() != VideoCore::FramePipeline::Mode::Off) {
        const u64 draws = VideoCore::Perf::ReadCounter(VideoCore::Perf::Counter::Draws);
        pipeline_cache.SetLoadingScreen(
            loading_detector.Tick(LoadingDetector::Clock::now(), draws - composite_draws));
        composite_draws = draws;
    }
    fence_manager.TickFrame();
    {
        // The fence worker shares staging with both caches. Retire resources only while neither
        // cache can add a reference or release a pinned readback concurrently.
        std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
        PollMaterialReadbacks();
        auto snapshot = texture_runtime.BeginMemoryGuardFrame();
        if (staging.GuardMemory(snapshot)) {
            // A retired ring can return 256 MiB at once. Refresh only on that rare transition,
            // so GC does not perform synchronous recovery using the pre-reclamation snapshot.
            snapshot = texture_runtime.BeginMemoryGuardFrame();
        }
        pipeline_cache.GuardMemory(snapshot);
        pipeline_cache.TickResidency(snapshot, scheduler);
        texture_cache.TickFrame();
        buffer_cache.TickFrame();
    }
}
bool RasterizerD3D12::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& config) {
    DrainCpuWrites();
    HuntScope hunt{draw_hunt, Hunt::Kind::Blit};
    std::scoped_lock lock{texture_cache.mutex};
    hunt.Target(nullptr);
    const bool blitted = texture_cache.BlitImage(dst, src, config);
    if (blitted) {
        hunt.Recorded();
        InvalidateGraphicsState();
    }
    if (trace_draws) {
        TraceDraw(fmt::format("2D blit {:x} ({}x{}) -> {:x} ({}x{}) {}", src.Address(), src.width,
                              src.height, dst.Address(), dst.width, dst.height,
                              blitted ? "accelerated" : "left to the CPU"),
                  nullptr, nullptr, {}, 0, 0);
    }
    return blitted;
}

Tegra::Engines::AccelerateDMAInterface& RasterizerD3D12::AccessAccelerateDMA() {
    DrainCpuWrites(); // called on each DMA launch
    return accelerate_dma;
}
void RasterizerD3D12::AccelerateInlineToMemory(GPUVAddr address, size_t size,
                                               std::span<const u8> memory) {
    DrainCpuWrites();
    const auto cpu_address = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_address) return gpu_memory->WriteBlock(address, memory.data(), size);
    gpu_memory->WriteBlockUnsafe(address, memory.data(), size);
    { std::scoped_lock lock{buffer_cache.mutex};
      if (!buffer_cache.InlineMemory(*cpu_address, size, memory)) buffer_cache.WriteMemory(*cpu_address, size); }
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.WriteMemory(*cpu_address, size); }
    pipeline_cache.InvalidateRegion(*cpu_address, size);
    query_cache.InvalidateRegion(*cpu_address, size);
}
void RasterizerD3D12::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                        const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}
void RasterizerD3D12::InitializeChannel(Tegra::Control::ChannelState& channel) {
    // The dirty flags the generic caches rely on (render targets, shaders, vertex and index
    // buffers, descriptors) are only raised by register writes listed in these tables; without
    // them a new render target or program is never picked up (Vulkan's StateTracker::SetupTables
    // does the same, plus its own flags).
    state_tracker.SetupTables(channel);
    PipelineKeyTracking::SetupTable(
        channel.payload->maxwell_3d.dirty.tables[PipelineKeyTracking::TABLE_INDEX]);
    CreateChannel(channel);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.CreateChannel(channel); buffer_cache.CreateChannel(channel); }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    BindToChannel(channel.bind_id);
    state_tracker.ChangeChannel(channel);
    channel_bound = true;
    state_tracker.InvalidateState();
    state_invalidation_pending = false;
    InvalidateGraphicsState();
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.BindToChannel(channel.bind_id); buffer_cache.BindToChannel(channel.bind_id); }
    pipeline_cache.BindToChannel(channel.bind_id);
    query_cache.BindToChannel(channel.bind_id);
}
void RasterizerD3D12::ReleaseChannel(s32 id) {
    EraseChannel(id);
    // EraseChannel clears current_channel_id when the active channel goes away. A later scheduler
    // reset (notably during shutdown) must not dereference StateTracker::flags from that channel.
    channel_bound = current_channel_id != UNSET_CHANNEL;
    if (!channel_bound) {
        state_invalidation_pending = false;
    }
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.EraseChannel(id); buffer_cache.EraseChannel(id); }
    pipeline_cache.EraseChannel(id);
    query_cache.EraseChannel(id);
}

} // namespace D3D12
