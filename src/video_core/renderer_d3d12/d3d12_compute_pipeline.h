// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include "common/memory_ledger.h"
#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Tegra::Engines {
class KeplerCompute;
}

namespace D3D12 {

class PipelineLayout;
class Scheduler;

/// Same key as Vulkan's compute pipelines.
struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace D3D12

namespace std {
template <>
struct hash<D3D12::ComputePipelineCacheKey> {
    size_t operator()(const D3D12::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace D3D12 {

/// What a compute pipeline binds its guest resources with (the rasterizer's caches and heaps).
struct ComputeBindContext {
    Tegra::Engines::KeplerCompute& kepler_compute;
    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    GuestDescriptorQueue& descriptor_queue;
    SamplerHeap& sampler_heap;
    Scheduler& scheduler;
};

/// A guest compute pipeline: signed DXIL, its root signature and the PSO, built on a worker when
/// one is given. Mirrors Vulkan::ComputePipeline.
class ComputePipeline {
public:
    /// SPIR-V to signed DXIL; throws when it fails (the dispatches are then skipped).
    using CompileDxil = std::function<std::vector<u8>()>;

    /// compile_dxil runs with the PSO build, on worker_thread when one is given: the GPU thread
    /// only translates the guest shader.
    ComputePipeline(const Device& device, VideoCore::ShaderNotify* shader_notify,
                    Common::ThreadWorker* worker_thread, u64 unique_hash, CompileDxil compile_dxil,
                    const Shader::Info& info, const PipelineLayout& layout,
                    bool emulate_reduction = false, u32 workgroup_split = 1);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::acquire);
    }

    /// Any thread: waits for the build, running it here if it is still queued (the claim), since
    /// waiting for it would also wait for the queue ahead of it.
    void WaitBuilt();

    /// The command recorder, for a dispatch recorded before its pipeline was built: WaitBuilt,
    /// then the PSO, null when rejected or out of memory.
    [[nodiscard]] ID3D12PipelineState* WaitHandle();

    /// GPU thread, once built: whether its PSO failed for lack of memory, so its dispatches are
    /// skipped until RetryOutOfMemory builds it.
    [[nodiscard]] bool OutOfMemory() const noexcept {
        return IsBuilt() && out_of_memory;
    }

    /// GPU thread: builds again a PSO that ran out of memory. True once it has one.
    bool RetryOutOfMemory();

    /// GPU thread: the frame before which RetryOutOfMemory is not tried again.
    u64 retry_frame{};

    /// Null when D3D12 rejected the pipeline or it is still building (IsBuilt).
    [[nodiscard]] ID3D12PipelineState* Handle() const noexcept {
        return IsBuilt() ? pipeline_state.Get() : nullptr;
    }

    [[nodiscard]] const PipelineLayout& Layout() const noexcept {
        return layout;
    }

    [[nodiscard]] const Shader::Info& Info() const noexcept {
        return info;
    }

    /// Port of Vulkan's ComputePipeline::Configure: binds the launch's buffers and textures and
    /// writes the descriptor tables in root-signature order. Records no command-list state, like
    /// GraphicsPipeline::Configure.
    void Configure(const ComputeBindContext& context, PipelineBindings& out);

    [[nodiscard]] u64 UniqueHash() const noexcept {
        return unique_hash;
    }
    /// Once built.
    [[nodiscard]] std::span<const u8> DiagnosticDxil() const noexcept { return dxil; }
    /// Built with EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION (PipelineCache::EmulateReduction).
    [[nodiscard]] bool EmulatesReduction() const noexcept { return emulate_reduction; }
    /// Real groups per guest group along X (Shader::Optimization::WorkgroupSplitPass): the
    /// dispatch multiplies its X group count by it. 1 for an unsplit shader.
    [[nodiscard]] u32 WorkgroupSplit() const noexcept { return workgroup_split; }

private:
    struct PendingBuild {
        std::atomic_bool claimed{};
        u64 queued_us{};
        Common::UniqueFunction<void> run;
    };
    /// Runs a claimed build, recording how long it sat in the queue.
    void RunClaimed(PendingBuild& build);
    void Build();

    const Device& device;
    const u64 unique_hash;
    const bool emulate_reduction;
    const u32 workgroup_split;
    const PipelineLayout& layout;
    std::vector<u8> dxil;
    Common::MemoryCharge dxil_charge;
    Shader::Info info;
    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};
    Common::MemoryCharge charge{Common::MemoryAccount::Pipelines, sizeof(ComputePipeline)};

    ComPtr<ID3D12PipelineState> pipeline_state;
    Common::MemoryCharge pso_charge;
    bool out_of_memory{}; ///< The last Build() failed with E_OUTOFMEMORY.

    std::mutex build_mutex;
    std::condition_variable build_condvar;
    std::atomic_bool is_built{false};
    std::shared_ptr<PendingBuild> pending; ///< build_mutex: the build in a queue, if any.
};

} // namespace D3D12
