// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "common/logging.h"

#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {
namespace {

struct QueryInfo {
    D3D12_QUERY_HEAP_TYPE heap;
    D3D12_QUERY_TYPE query;
    u64 bytes;        ///< what ResolveQueryData writes
    u64 value_offset; ///< where the guest's count is in it
};

QueryInfo GetQueryInfo(VideoCore::QueryType type) {
    switch (type) {
    case VideoCore::QueryType::SamplesPassed:
        return {D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3D12_QUERY_TYPE_OCCLUSION, sizeof(u64), 0};
    case VideoCore::QueryType::PrimitivesGenerated:
        return {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CPrimitives)};
    case VideoCore::QueryType::TfbPrimitivesWritten:
        return {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0,
                sizeof(D3D12_QUERY_DATA_SO_STATISTICS),
                offsetof(D3D12_QUERY_DATA_SO_STATISTICS, NumPrimitivesWritten)};
    default:
        UNREACHABLE();
    }
}

} // namespace

QueryPool::QueryPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

QueryPool::Slot QueryPool::Reserve(VideoCore::QueryType type) {
    std::scoped_lock lock{mutex};
    TypePool& pool = pools[static_cast<size_t>(type)];
    if (pool.free.empty() || !scheduler.IsFree(pool.free.front().tick)) {
        AddPage(type);
    }
    const Slot slot = pool.free.front().slot;
    pool.free.pop_front();
    return slot;
}

void QueryPool::Release(VideoCore::QueryType type, const Slot& slot, u64 tick) {
    std::scoped_lock lock{mutex};
    pools[static_cast<size_t>(type)].free.push_back({slot, tick});
}

void QueryPool::AddPage(VideoCore::QueryType type) {
    const QueryInfo info = GetQueryInfo(type);
    Page page;
    const D3D12_QUERY_HEAP_DESC heap_desc{.Type = info.heap, .Count = PAGE_SIZE, .NodeMask = 0};
    ThrowIfFailed(device.Get()->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&page.heap)),
                  "CreateQueryHeap");
    page.readback = CreateCommittedBuffer(device.Get(), info.bytes * PAGE_SIZE,
                                          D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                          "CreateCommittedResource (query readback)");
    // New slots go first: the free list's front was not free yet.
    TypePool& pool = pools[static_cast<size_t>(type)];
    for (u32 i = PAGE_SIZE; i-- > 0;) {
        pool.free.push_front({Slot{page.heap.Get(), page.readback.Get(), i, info.bytes * i}, 0});
    }
    pool.pages.push_back(std::move(page));
    LOG_DEBUG(Render, "D3D12: query page {} created (type {})", pool.pages.size(),
              static_cast<u32>(type));
}

QueryCache::QueryCache(RasterizerD3D12& rasterizer_,
                       Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device_,
                       Scheduler& scheduler_)
    : QueryCacheLegacy{rasterizer_, device_memory_}, rasterizer{rasterizer_}, device{device_},
      scheduler{scheduler_}, pool{std::make_shared<QueryPool>(device_, scheduler_)} {
    EnableCounters();
    scheduler.RegisterOnSubmit([this] { DisableStreams(); });
    scheduler.RegisterOnReset([this] { EnableCounters(); });
}

QueryCache::~QueryCache() {
    try {
        if (SUCCEEDED(device.Get()->GetDeviceRemovedReason())) DisableStreams();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "Query teardown: {}", e.what());
    }
    scheduler.ClearSubmissionCallbacks();
    scheduler.DrainForShutdown();
}

bool QueryCache::AnyCommandQueued() const noexcept {
    return rasterizer.AnyCommandQueued();
}

std::optional<PendingReport> QueryCache::PeekReport(VAddr addr) {
    return VisitQuery(addr, [](CachedQuery* query) -> std::optional<PendingReport> {
        if (!query) {
            return std::nullopt;
        }
        // The value is the counter's slice plus every slice it continues from (HostCounterBase).
        PendingReport report;
        for (std::shared_ptr<HostCounter> counter = query->GetCounter(); counter;) {
            if (const std::optional<u64>& value = counter->KnownResult()) {
                report.known += *value;
                break;
            }
            if (report.num_slices == PendingReport::MAX_SLICES || !counter->Ended()) {
                report.complete = false;
                break;
            }
            std::shared_ptr<HostCounter> next = counter->Dependency();
            report.slices[report.num_slices++] = std::move(counter);
            counter = std::move(next);
        }
        return report;
    });
}

HostCounter::HostCounter(QueryCache& cache_, std::shared_ptr<HostCounter> dependency_,
                         VideoCore::QueryType type_)
    : HostCounterBase{std::move(dependency_)}, cache{cache_}, type{type_},
      query_type{GetQueryInfo(type_).query}, pool{cache_.GetPool()}, slot{pool->Reserve(type_)} {
    begin_tick = cache.GetScheduler().CurrentTick();
    cache.GetScheduler().CommandList()->BeginQuery(slot.heap, query_type, slot.index);
}

HostCounter::~HostCounter() {
    // The streams end every counter before dropping it (CounterStreamBase::Start). One dropped
    // with its query open would leave a Begin without End and the list's Close would fail with
    // E_FAIL: end it while its list is still the one recording. A list already submitted closed
    // fine without it, so the slot is just not reused then.
    if (!ended) {
        static std::atomic_bool reported{};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            LOG_WARNING(Render, "D3D12: a counter (type {}) was dropped with its query open",
                        static_cast<u32>(type));
        }
        try {
            Scheduler& scheduler = cache.GetScheduler();
            if (scheduler.IsRecordingThread() && scheduler.CurrentTick() == begin_tick) {
                EndQuery();
            }
        } catch (const std::exception& exception) {
            LOG_ERROR(Render, "D3D12: ending a dropped query: {}", exception.what());
        }
    }
    if (ended) {
        pool->Release(type, slot, tick);
    }
}

void HostCounter::EndQuery() {
    if (ended) {
        return;
    }
    auto* list = cache.GetScheduler().CommandList();
    list->EndQuery(slot.heap, query_type, slot.index);
    list->ResolveQueryData(slot.heap, query_type, slot.index, 1, slot.readback, slot.offset);
    tick = cache.GetScheduler().CurrentTick();
    ended = true;
}

u64 HostCounter::ResolvedBytes() const noexcept {
    return GetQueryInfo(type).bytes;
}

u64 HostCounter::ValueOffset() const noexcept {
    return GetQueryInfo(type).value_offset;
}

u64 HostCounter::BlockingQuery([[maybe_unused]] bool async) const {
    if (!ended) {
        const_cast<HostCounter*>(this)->EndQuery();
    }
    cache.GetScheduler().Wait(tick);
    return ReadResolved();
}

std::optional<u64> HostCounter::TryQuery() const {
    if (!ended || !cache.GetScheduler().IsFree(tick)) {
        return std::nullopt;
    }
    return ReadResolved();
}

u64 HostCounter::ReadResolved() const {
    void* mapped_page{};
    const D3D12_RANGE read_range{static_cast<SIZE_T>(slot.offset),
                                 static_cast<SIZE_T>(slot.offset + GetQueryInfo(type).bytes)};
    ThrowIfFailed(slot.readback->Map(0, &read_range, &mapped_page), "Map (query readback)");
    const u8* const mapped = static_cast<const u8*>(mapped_page) + slot.offset;
    u64 value{};
    std::memcpy(&value, mapped + ValueOffset(), sizeof(value));
    const D3D12_RANGE written_range{0, 0};
    slot.readback->Unmap(0, &written_range);
    {
        // Diagnostic: values the GPU hands back for guest counters (first 24 per type, then
        // each 256th), to see whether occlusion-style results are ever non-zero.
        static std::array<std::atomic<u64>, 16> read_counts{};
        static std::array<std::atomic<u64>, 16> nonzero_counts{};
        const u32 index = static_cast<u32>(type) & 15;
        const u64 count = ++read_counts[index];
        if (VideoCommon::FullCapture::Active()) VideoCommon::FullCapture::Event("query_host_result",
            fmt::format("{{\"type\":{},\"value\":{}}}", static_cast<u32>(type), value));
        const u64 nonzero = value != 0 ? ++nonzero_counts[index] : nonzero_counts[index].load();
        if (count <= 24 || (count & 255) == 0) {
            LOG_WARNING(Render, "D3D12: counter type {} read #{} = {} ({} non-zero so far)",
                        static_cast<u32>(type), count, value, nonzero);
        }
    }
    return value;
}

CachedQuery::CachedQuery(QueryCache&, VideoCore::QueryType, VAddr cpu_addr, u8* host_ptr)
    : CachedQueryBase{cpu_addr, host_ptr} {}

} // namespace D3D12
