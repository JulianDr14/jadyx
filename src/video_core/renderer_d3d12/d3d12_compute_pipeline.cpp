// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>

#include <boost/container/small_vector.hpp>

#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/gpu_thread.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_driver_guard.h"
#include "video_core/renderer_d3d12/d3d12_memory_reserve.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_helper.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_pipeline_diagnostics.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace D3D12 {

size_t ComputePipelineCacheKey::Hash() const noexcept {
    return static_cast<size_t>(
        Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this));
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

ComputePipeline::ComputePipeline(const Device& device_, VideoCore::ShaderNotify* shader_notify,
                                 Common::ThreadWorker* worker_thread, u64 unique_hash_,
                                 CompileDxil compile_dxil, const Shader::Info& info_,
                                 const PipelineLayout& layout_, bool emulate_reduction_,
                                 u32 workgroup_split_)
    : device{device_}, unique_hash{unique_hash_}, emulate_reduction{emulate_reduction_},
      workgroup_split{workgroup_split_}, layout{layout_}, info{info_} {
    std::ranges::copy(info.constant_buffer_used_sizes, uniform_buffer_sizes.begin());
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    auto func{[this, shader_notify, compile_dxil = std::move(compile_dxil)] {
        bool compiled = false;
        try {
            dxil = compile_dxil();
            dxil_charge = Common::MemoryCharge(Common::MemoryAccount::ShaderBytecode,
                                               dxil.capacity());
            compiled = true;
        } catch (const std::exception& exception) {
            // Handle() stays null: dispatches with this pipeline are skipped.
            BUG_TRACK_KEY(ShaderCompile, unique_hash, "DXIL for compute {:016x} failed: {}",
                          unique_hash, exception.what());
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_ERROR(Render, "D3D12: building compute pipeline {:016x} failed: {}", unique_hash,
                      exception.what());
        }
        if (compiled) {
            Build();
        }
        {
            std::scoped_lock lock{build_mutex};
            is_built.store(true, std::memory_order::release);
        }
        build_condvar.notify_all();
        if (shader_notify) {
            shader_notify->MarkShaderComplete();
        }
    }};
    if (!worker_thread) {
        func();
        return;
    }
    auto build = std::make_shared<PendingBuild>();
    build->queued_us = SteadyMicros();
    build->run = std::move(func);
    pending = build;
    // Pipelines outlive the workers (joined first), so this is still valid if the worker runs.
    worker_thread->QueueWork([this, build] {
        if (!build->claimed.exchange(true, std::memory_order::acq_rel)) {
            RunClaimed(*build);
        }
    });
}

ComputePipeline::~ComputePipeline() = default;

void ComputePipeline::RunClaimed(PendingBuild& build) {
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

bool ComputePipeline::RetryOutOfMemory() {
    if (!OutOfMemory()) {
        return false;
    }
    Build();
    if (!out_of_memory) {
        LOG_INFO(Render, "D3D12: compute pipeline {:016x} built after running out of memory",
                 unique_hash);
    }
    return !out_of_memory;
}

void ComputePipeline::WaitBuilt() {
    std::shared_ptr<PendingBuild> build;
    {
        std::scoped_lock lock{build_mutex};
        if (is_built.load(std::memory_order::relaxed)) {
            return;
        }
        build = pending;
    }
    // Still queued: run here rather than wait for the queue. A build already running on its
    // worker is waited for, since the driver call cannot be taken over halfway.
    if (build && !build->claimed.exchange(true, std::memory_order::acq_rel)) {
        GetPipelineStats().inline_builds.fetch_add(1, std::memory_order_relaxed);
        RunClaimed(*build);
    }
    std::unique_lock lock{build_mutex};
    build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::relaxed); });
}

ID3D12PipelineState* ComputePipeline::WaitHandle() {
    if (!IsBuilt()) {
        const u64 wait_start = SteadyMicros();
        WaitBuilt();
        GetPipelineStats().Record(PipelineStage::WaitCompute, SteadyMicros() - wait_start);
    }
    // RetryOutOfMemory may set it on the GPU thread meanwhile.
    std::scoped_lock lock{build_mutex};
    return pipeline_state.Get();
}

void ComputePipeline::Configure(const ComputeBindContext& context, PipelineBindings& out) {
    using VideoCommon::ImageViewInOut;
    using VideoCommon::SamplerId;

    BufferCache& buffer_cache = context.buffer_cache;
    TextureCache& texture_cache = context.texture_cache;
    Tegra::MemoryManager& gpu_memory = context.gpu_memory;
    const auto& qmd = context.kepler_compute.launch_description;

    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &uniform_buffer_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t ssbo_index = 0;
    for (const auto& desc : info.storage_buffers_descriptors) {
        buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
        ++ssbo_index;
    }

    texture_cache.SynchronizeDescriptors(true);

    ImageViewList views;
    SamplerIdList samplers;
    DrawSamplerCache<VideoCommon::SamplerId> sampler_cache{DrawSamplerReuseEnabled()};

    const auto& cbufs = qmd.const_buffer_config;
    const bool via_header_index = qmd.linked_tsc != 0;
    const auto read_handle = [&](const auto& desc, u32 index) {
        return ReadTextureHandle(gpu_memory, desc, index, via_header_index,
                                 [&](u32 cbuf) { return cbufs[cbuf].Address(); });
    };
    GatherStageViews(info, texture_cache, true, read_handle, views, samplers, sampler_cache);
    texture_cache.FillImageViews(std::span(views.data(), views.size()), true);

    buffer_cache.UnbindComputeTextureBuffers();
    const ImageViewInOut* texture_buffer_it = views.data();
    BindStageTextureBuffers(info, texture_cache, texture_buffer_it,
                            [&](size_t index, ImageView& view,
                                VideoCore::Surface::PixelFormat format, bool is_written,
                                bool is_image) {
                                buffer_cache.BindComputeTextureBuffer(
                                    index, view.GpuAddr(), view.BufferSize(), format, is_written,
                                    is_image);
                            });

    buffer_cache.UpdateComputeBuffers();

    // The table is written in root-signature order: uniform, storage and texel buffers while the
    // buffer cache binds them, then textures and images.
    // A full sampler heap can Finish/reset: do this before reserving descriptors and UBOs.
    SamplerTableBuilder sampler_table;
    for (const SamplerId id : samplers) sampler_table.Add(texture_cache.GetSampler(id));
    out.sampler_table = sampler_table.Table(context.sampler_heap);
    GuestDescriptorQueue& queue = context.descriptor_queue;
    queue.Acquire(layout.NumResourceDescriptors());
    buffer_cache.BindHostComputeBuffers();

    ImageTransitionList image_transitions;
    const SamplerId* samplers_it = samplers.data();
    const ImageViewInOut* views_it = views.data();
    views_it += Shader::NumDescriptors(info.texture_buffer_descriptors);
    views_it += Shader::NumDescriptors(info.image_buffer_descriptors);
    const bool has_sampler_table = layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE;
    IntegerSamplerTable integer_samplers;
    u32 texture_binding = FirstTextureBinding(info, 0);
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index, ++texture_binding) {
            const VideoCommon::ImageViewId view_id = (views_it++)->id;
            const ImageView& image_view = texture_cache.GetImageView(view_id);
            image_view.PrepareRead(desc.type);
            queue.AddCopy(image_view.Handle(desc.type));
            const SamplerId sampler_id = *(samplers_it++);
            if (out.trace_filters) out.trace_filters->push_back(fmt::format(
                "binding={} type={} srv=[{}] sampler=[{}]", texture_binding,
                static_cast<u32>(desc.type), image_view.DescribeSrv(desc.type, false),
                texture_cache.GetSampler(sampler_id).Describe()));
            const Sampler& sampler = texture_cache.GetSampler(sampler_id);
            if (has_sampler_table && (desc.is_integer || emulate_reduction)) {
                integer_samplers.Add(texture_binding, sampler, image_view);
            }
            if (!emulate_reduction && sampler.EmulatedReduction() != 0 && ReducibleTexture(desc)) {
                out.unemulated_reduction_stages = 1;
            }
            image_transitions.emplace_back(view_id, false);
            if (out.trace_all_views) out.trace_all_views->push_back({view_id, false, desc.type});
        }
    }
    PushStorageImages(info, texture_cache, queue, views_it, image_transitions);

    BarrierBatch barriers{context.scheduler};
    ForEachImageTransition(image_transitions, texture_cache, [&](VideoCommon::ImageViewId view_id,
                                                                 bool is_storage) {
        if (out.trace_all_views && is_storage) out.trace_all_views->push_back({view_id, true});
        if (GpuDiagnosticsEnabled()) {
            const auto& view = texture_cache.GetImageView(view_id);
            TraceGpuDetail(fmt::format("compute image tick={} CS={:016x} guest={:x} resource={} storage={}",
                context.scheduler.CurrentTick(), unique_hash, view.GpuAddr(),
                static_cast<void*>(view.ImageHandle()), is_storage));
        }
        texture_cache.GetImageView(view_id).TransitionImage(
            is_storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            &barriers);
    });
    barriers.Flush();

    out.resource_table = queue.Table();
    out.integer_samplers = integer_samplers.Upload(buffer_cache);

    // No resolution scaling yet.
    ResetPushConstants(out.push_constants);
}

void ComputePipeline::Build() {
    const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
        .pRootSignature = layout.Handle(),
        .CS = {.pShaderBytecode = dxil.data(), .BytecodeLength = dxil.size()},
        .NodeMask = 0,
        .CachedPSO = {},
        .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
    };
    ID3D12Device* const raw = device.Get();
    DWORD fault = 0;
    // Built aside and set under build_mutex: a dispatch recorded earlier may be reading it
    // (WaitHandle) while the GPU thread retries a build that ran out of memory.
    ComPtr<ID3D12PipelineState> built;
    const auto create = [&] {
        std::shared_lock build_lock{PipelineBuildMutex()};
        return CallDriverGuarded(
            [&] { return raw->CreateComputePipelineState(&desc, IID_PPV_ARGS(&built)); }, fault);
    };
    HRESULT hr = create();
    out_of_memory = hr == E_OUTOFMEMORY;
    if (out_of_memory) {
        MemoryReserve::NoteOutOfMemory();
        if (MemoryReserve::Release()) {
            hr = create();
            out_of_memory = hr == E_OUTOFMEMORY;
        }
    }
    if (hr == DRIVER_FAULT_HRESULT) {
        built.Detach(); // whatever the driver left there is not an object to release
        BUG_TRACK_KEY(PipelineRejected, unique_hash,
                      "the driver faulted (0x{:08X}) building compute pipeline {:016x}; its "
                      "dispatches are skipped",
                      static_cast<u32>(fault), unique_hash);
        DumpFaultedPipeline(unique_hash, 0, std::span{&dxil, 1});
        ProbeFaultedComputeShader(raw, desc, unique_hash);
    }
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("compute PSO for {:016x} (HRESULT 0x{:08X})", unique_hash,
                           static_cast<u32>(hr));
    });
    if (FAILED(hr)) {
        BUG_TRACK_KEY(PipelineRejected, unique_hash,
                      "CreateComputePipelineState failed (HRESULT 0x{:08X}) for {:016x}",
                      static_cast<u32>(hr), unique_hash);
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_ERROR(Render, "D3D12: CreateComputePipelineState failed (HRESULT 0x{:08X}) for {:016x}",
                  static_cast<u32>(hr), unique_hash);
        return;
    }
    {
        std::scoped_lock lock{build_mutex};
        pipeline_state = std::move(built);
    }
    pso_charge = Common::MemoryCharge(Common::MemoryAccount::PipelineStates, 0);
    LOG_INFO(Render, "D3D12: compute pipeline built for {:016x} ({} + {} descriptors)",
             unique_hash, layout.NumResourceDescriptors(), layout.NumSamplerDescriptors());
}

} // namespace D3D12
