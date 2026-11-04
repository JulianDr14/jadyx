// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>

#include "video_core/query_cache/query_cache_base.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace Tegra::Engines {
class Maxwell3D;
}

namespace VideoCommon {
class StreamerInterface;
template <class P>
class BufferCache;
} // namespace VideoCommon

namespace D3D12 {

class BufferCacheRuntime;
class Scheduler;
class ShaderCompiler;
class StagingBufferPool;
struct BufferCacheParams;
using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

class QueryCacheRuntime;

struct QueryCacheParams {
    using RuntimeType = QueryCacheRuntime;
};

/// The guest counters (occlusion, primitives generated, stream output primitives) as the Vulkan
/// backend runs them, on VideoCommon::QueryCacheBase: no query is ever waited for on the CPU.
using QueryCache = VideoCommon::QueryCacheBase<QueryCacheParams>;

/// D3D12 side of the query cache.
///
/// A guest counter runs over host query slots taken from banks (a query heap and a persistently
/// mapped readback buffer of BANK_SIZE slots); a slot ends at each report, reset, disable and
/// submission. Every report is the sum of the slots since the last reset, reached two ways:
///  - on the GPU: when the guest waits for idle or a fence is queued, the pending slots are
///    resolved in contiguous runs and a compute shader sums each report into place in the buffer
///    cache, so draws, shaders and render conditions read it without a CPU wait;
///  - on the CPU: at the fence, the same slots are resolved into the banks' readback memory and
///    the fence thread sums them once the GPU is past it, for the guest's own reads.
/// A bank is reused once both have consumed its slots and the GPU is past its last use.
class QueryCacheRuntime {
public:
    explicit QueryCacheRuntime(const Device& device, Scheduler& scheduler,
                               const ShaderCompiler& compiler, BufferCache& buffer_cache,
                               BufferCacheRuntime& buffer_runtime, StagingBufferPool& staging);
    ~QueryCacheRuntime();

    QueryCacheRuntime(const QueryCacheRuntime&) = delete;
    QueryCacheRuntime& operator=(const QueryCacheRuntime&) = delete;

    /// Whether the guest counter type is counted by a host query (else written as a payload).
    [[nodiscard]] static bool IsHostCounted(VideoCommon::QueryType type) noexcept;

    /// Right before a guest draw: starts the counters it counts into, and pauses occlusion when
    /// the guest disabled it.
    void PrepareDraw(bool zpass_enabled);

    /// Ends every running counter (before internal draws, and before each submission); the next
    /// guest draw starts them again.
    void PauseCounters();

    // VideoCommon::QueryCacheBase interface.
    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType type);
    void Bind3DEngine(Tegra::Engines::Maxwell3D*) {}
    void Barriers(bool) {} // every resource is transitioned where it is used
    template <typename SyncValuesType>
    void SyncValues(std::span<SyncValuesType> values, ID3D12Resource* base_src = nullptr);
    // Render conditions are predicated by ConditionalRendering, which compares any reports.
    void EndHostConditionalRendering() {}
    void PauseHostConditionalRendering() {}
    void ResumeHostConditionalRendering() {}
    bool HostConditionalRenderingCompareValue(VideoCommon::LookupData, bool) {
        return false;
    }
    bool HostConditionalRenderingCompareValues(VideoCommon::LookupData, VideoCommon::LookupData,
                                               bool, bool) {
        return false;
    }

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

} // namespace D3D12
