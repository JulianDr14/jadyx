// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <eden_spirv_to_dxil.h>
#include <fmt/format.h>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/diagnostics/full_capture.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host_shaders/d3d12_query_sum_comp_spv.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12 {
namespace {

using VideoCommon::QueryFlagBits;
using VideoCommon::QueryType;

/// Host query slots per bank: one query heap and its readback memory.
constexpr u32 BANK_SIZE = 256;

/// Slots a counter may run over without a report before they are summed anyway, so a counter the
/// guest never reports does not hold banks without end.
constexpr size_t SPILL_SLOTS = 1024;

/// Threads per group of d3d12_query_sum.comp.
constexpr u32 SUM_GROUP_SIZE = 64;

/// Root parameters of d3d12_query_sum.comp: its push constants, the resolved slots (readonly SSBO
/// binding 0, t0), the entries (binding 1, t1), the carry (binding 2, u2), the results (binding 3,
/// u3) and spirv_to_dxil's compute runtime data.
constexpr u32 SUM_CONSTANTS_PARAM = 0;
constexpr u32 SUM_RESOLVED_PARAM = 1;
constexpr u32 SUM_ENTRIES_PARAM = 2;
constexpr u32 SUM_CARRY_PARAM = 3;
constexpr u32 SUM_RESULTS_PARAM = 4;
constexpr u32 SUM_RUNTIME_DATA_PARAM = 5;

/// How a guest counter is counted on the host.
struct CounterKind {
    D3D12_QUERY_HEAP_TYPE heap;
    D3D12_QUERY_TYPE query;
    u32 stride;       ///< bytes ResolveQueryData writes per slot
    u32 value_offset; ///< where the guest's count is in them
};

// Precise occlusion (not binary): the guest reads the sample count.
constexpr CounterKind OCCLUSION{D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3D12_QUERY_TYPE_OCCLUSION,
                                sizeof(u64), 0};
constexpr CounterKind PRIMITIVES{
    D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
    sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
    offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CPrimitives)};
constexpr CounterKind STREAM_OUTPUT{
    D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0,
    sizeof(D3D12_QUERY_DATA_SO_STATISTICS),
    offsetof(D3D12_QUERY_DATA_SO_STATISTICS, NumPrimitivesWritten)};

static_assert(OCCLUSION.stride % 8 == 0 && PRIMITIVES.stride % 8 == 0 &&
                  STREAM_OUTPUT.stride % 8 == 0,
              "ResolveQueryData destinations must stay 8-byte aligned");

/// Streamer ids: the bit of each in the query cache's masks, equal to one of the guest types each
/// serves (QueryCacheBase indexes its streamers by type).
constexpr size_t GUEST_STREAMER_ID = static_cast<size_t>(QueryType::Payload);
constexpr size_t OCCLUSION_STREAMER_ID = static_cast<size_t>(QueryType::ZPassPixelCount64);
constexpr size_t PRIMITIVES_STREAMER_ID = static_cast<size_t>(QueryType::PrimitivesGenerated);
constexpr size_t STREAM_OUTPUT_STREAMER_ID =
    static_cast<size_t>(QueryType::StreamingPrimitivesSucceeded);

/// A query heap of BANK_SIZE slots and the persistently mapped readback memory they resolve to.
struct Bank {
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    std::span<u8> readback_data;
    u32 used{};
    /// Last tick that began, ended or resolved one of its slots.
    u64 last_tick{};
    /// Two per slot handed out: one for the CPU sum, one for the GPU sum. Released on the fence
    /// thread too, so atomic.
    std::atomic<u32> refs{};
};

struct Slot {
    Bank* bank;
    u32 index;
};

/// A guest report on a host counter.
class CounterQuery : public VideoCommon::HostQueryBase {
public:
    using HostQueryBase::HostQueryBase;

    /// The cache freed it before the fence thread summed it: freed once summed.
    bool free_when_synced{};
};

/// A report summed on the GPU, written from offset (its index * 8) of the results buffer.
struct HostSyncValues {
    DAddr address;
    u64 size;
    u64 offset;

    static constexpr bool GeneratesBaseBuffer = false;
};

/// A buffer of the GPU sum, its state tracked for the list being recorded (buffers decay to
/// COMMON after each submission).
class TrackedBuffer {
public:
    /// Grows to hold bytes (in powers of two); the old buffer lives until the GPU is past it.
    void Reserve(const Device& device, Scheduler& scheduler, u64 bytes,
                 D3D12_RESOURCE_FLAGS flags, const char* what) {
        if (resource && size >= bytes) {
            return;
        }
        if (resource) {
            scheduler.DeferRelease(std::move(resource));
        }
        size = std::bit_ceil(std::max<u64>(bytes, 256));
        resource = CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_STATE_COMMON, flags, what);
        tick = 0;
    }

    /// Appends the barrier to next (a UAV barrier from UAV to UAV), if any is needed.
    void Transition(const Scheduler& scheduler, D3D12_RESOURCE_STATES next,
                    std::vector<D3D12_RESOURCE_BARRIER>& barriers) {
        const u64 current_tick = scheduler.CurrentTick();
        const D3D12_RESOURCE_STATES current =
            tick == current_tick ? state : D3D12_RESOURCE_STATE_COMMON;
        if (current != next) {
            barriers.push_back(TransitionBarrier(resource.Get(), current, next));
        } else if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            barriers.push_back(UavBarrier(resource.Get()));
        }
        state = next;
        tick = current_tick;
    }

    [[nodiscard]] ID3D12Resource* Get() const noexcept {
        return resource.Get();
    }

private:
    ComPtr<ID3D12Resource> resource;
    u64 size{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    u64 tick{};
};

void Barrier(CommandStream* cmd, std::vector<D3D12_RESOURCE_BARRIER>& barriers) {
    if (!barriers.empty()) {
        cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        barriers.clear();
    }
}

/// Calls func(bank, first index, count, position) for each run of slots that are consecutive in
/// one bank, position being where the run starts among the slots. slot_of returns an item's
/// slot, or null for items that are not slots.
template <typename Range, typename Projection, typename Func>
void ForEachRun(const Range& range, Projection&& slot_of, Func&& func) {
    const Slot* run{};
    u32 count{};
    u32 run_position{};
    u32 position{};
    for (const auto& item : range) {
        const Slot* const found = slot_of(item);
        if (!found) {
            continue;
        }
        const Slot& slot = *found;
        if (run && run->bank == slot.bank && run->index + count == slot.index) {
            ++count;
        } else {
            if (run) {
                func(run->bank, run->index, count, run_position);
            }
            run = &slot;
            count = 1;
            run_position = position;
        }
        ++position;
    }
    if (run) {
        func(run->bank, run->index, count, run_position);
    }
}

} // namespace

class CounterStreamer;

struct QueryCacheRuntime::Impl {
    Impl(QueryCacheRuntime& runtime_, const Device& device_, Scheduler& scheduler_,
         const ShaderCompiler& compiler_, BufferCache& buffer_cache_,
         BufferCacheRuntime& buffer_runtime_, StagingBufferPool& staging_);
    ~Impl();

    void CreateSumPipeline();

    QueryCacheRuntime& runtime;
    const Device& device;
    Scheduler& scheduler;
    const ShaderCompiler& compiler;
    BufferCache& buffer_cache;
    BufferCacheRuntime& buffer_runtime;
    StagingBufferPool& staging;

    ComPtr<ID3D12RootSignature> sum_root_signature;
    ComPtr<ID3D12PipelineState> sum_pipeline;

    VideoCommon::GuestStreamer<QueryCacheParams> guest_streamer;
    std::unique_ptr<CounterStreamer> occlusion;
    std::unique_ptr<CounterStreamer> primitives;
    std::unique_ptr<CounterStreamer> stream_output;

    // SyncValues scratch (GPU thread only).
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::vector<VideoCommon::BufferCopy> copies;
    std::vector<std::pair<DAddr, DAddr>> ranges;
    std::vector<std::pair<Buffer*, u32>> targets;
};

/// One guest counter counted over host query slots (see QueryCacheRuntime).
class CounterStreamer final : public VideoCommon::SimpleStreamer<CounterQuery> {
public:
    CounterStreamer(size_t id_, const CounterKind& kind_, QueryCacheRuntime::Impl& impl_)
        : SimpleStreamer<CounterQuery>(id_), kind{kind_}, impl{impl_} {}

    ~CounterStreamer() override = default;

    /// Whether the guest used the counter: until then it is never started.
    [[nodiscard]] bool Active() const noexcept {
        return active;
    }

    void StartCounter() override {
        if (running) {
            return;
        }
        running = AllocateSlot();
        impl.scheduler.CommandList()->BeginQuery(running->bank->heap.Get(), kind.query,
                                                 running->index);
    }

    void PauseCounter() override {
        if (!running) {
            return;
        }
        const Slot slot = *running;
        running.reset();
        impl.scheduler.CommandList()->EndQuery(slot.bank->heap.Get(), kind.query, slot.index);
        slot.bank->last_tick = impl.scheduler.CurrentTick();
        epoch_slots.push_back(slot);
        log.push_back({.slot = slot, .kind = Event::Kind::Slot});
        ++log_slots;
    }

    void ResetCounter() override {
        active = true;
        PauseCounter();
        // The CPU never needs the slots since the last report: no report counts them.
        if (!log_has_query) {
            for (const Event& event : log) {
                if (event.kind == Event::Kind::Slot) {
                    event.slot.bank->refs.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            log.clear();
            log_slots = 0;
        }
        log.push_back({.kind = Event::Kind::Reset});
        // Nor the GPU, when no report since the last sum is pending.
        if (epoch_queries.empty()) {
            ReleaseGpuRefs();
            epoch_slots.clear();
            epoch_reset = 0;
        } else {
            epoch_reset = static_cast<u32>(epoch_slots.size());
        }
    }

    size_t WriteCounter(VAddr address, bool has_timestamp, u32, std::optional<u32>) override {
        active = true;
        PauseCounter();
        const size_t query_id = BuildQuery(has_timestamp, address);
        const u32 first = epoch_reset.value_or(0);
        epoch_queries.push_back({
            .id = query_id,
            .first = first,
            .count = static_cast<u32>(epoch_slots.size()) - first,
            .use_carry = !epoch_reset.has_value(),
        });
        log.push_back({.query = query_id, .kind = Event::Kind::Query});
        log_has_query = true;
        return query_id;
    }

    bool HasPendingSync() const override {
        return !epoch_queries.empty() || epoch_slots.size() > SPILL_SLOTS;
    }

    /// Sums every pending report on the GPU (see d3d12_query_sum.comp).
    void PresyncWrites() override {
        if (!HasPendingSync()) {
            return;
        }
        Scheduler& scheduler = impl.scheduler;
        const u32 num_slots = static_cast<u32>(epoch_slots.size());
        // One entry per report, then the carry: every slot since the last reset.
        const u32 num_entries = static_cast<u32>(epoch_queries.size()) + 1;
        const StagingBufferRef entries =
            impl.staging.Request(num_entries * sizeof(std::array<u32, 4>), MemoryUsage::Upload);
        u32* const words = reinterpret_cast<u32*>(entries.mapped_span.data());
        for (size_t i = 0; i < epoch_queries.size(); ++i) {
            const EpochQuery& query = epoch_queries[i];
            words[i * 4 + 0] = query.first;
            words[i * 4 + 1] = query.count;
            words[i * 4 + 2] = query.use_carry ? 1U : 0U;
            words[i * 4 + 3] = 0;
        }
        const u32 carry_first = epoch_reset.value_or(0);
        const size_t carry_entry = (num_entries - 1) * 4;
        words[carry_entry + 0] = carry_first;
        words[carry_entry + 1] = num_slots - carry_first;
        words[carry_entry + 2] = epoch_reset.has_value() ? 0U : 1U;
        words[carry_entry + 3] = 0;

        resolved.Reserve(impl.device, scheduler, std::max<u64>(num_slots, 1) * kind.stride,
                         D3D12_RESOURCE_FLAG_NONE, "CreateCommittedResource (query resolve)");
        results.Reserve(impl.device, scheduler, (num_entries - 1) * sizeof(u64) + sizeof(u64),
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        "CreateCommittedResource (query results)");
        carry.Reserve(impl.device, scheduler, 2 * sizeof(u64),
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      "CreateCommittedResource (query carry)");
        if (!impl.sum_pipeline) {
            impl.CreateSumPipeline();
        }

        CommandStream* const cmd = scheduler.CommandList();
        const u64 tick = scheduler.CurrentTick();
        auto& barriers = impl.barriers;
        resolved.Transition(scheduler, D3D12_RESOURCE_STATE_COPY_DEST, barriers);
        Barrier(cmd, barriers);
        // Slots come from banks in order: few runs, one resolve each.
        ForEachRun(epoch_slots, [](const Slot& slot) { return &slot; },
                   [&](Bank* bank, u32 first, u32 count, u32 position) {
                       cmd->ResolveQueryData(bank->heap.Get(), kind.query, first, count,
                                             resolved.Get(), u64{position} * kind.stride);
                       bank->last_tick = tick;
                   });
        resolved.Transition(scheduler, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, barriers);
        carry.Transition(scheduler, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers);
        results.Transition(scheduler, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers);
        Barrier(cmd, barriers);

        const std::array<u32, 4> constants{num_entries, kind.stride / 4, kind.value_offset / 4,
                                           carry_parity};
        const u32 groups = Common::DivCeil(num_entries, SUM_GROUP_SIZE);
        dxil_spirv_compute_runtime_data runtime_data{};
        runtime_data.group_count_x = groups;
        runtime_data.group_count_y = 1;
        runtime_data.group_count_z = 1;
        std::array<u32, sizeof(runtime_data) / sizeof(u32)> runtime_words{};
        std::memcpy(runtime_words.data(), &runtime_data, sizeof(runtime_data));
        cmd->SetComputeRootSignature(impl.sum_root_signature.Get());
        scheduler.SetPipelineState(impl.sum_pipeline.Get());
        cmd->SetComputeRoot32BitConstants(SUM_CONSTANTS_PARAM, 4, constants.data(), 0);
        cmd->SetComputeRootShaderResourceView(SUM_RESOLVED_PARAM,
                                              resolved.Get()->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(SUM_ENTRIES_PARAM,
                                              entries.buffer->GetGPUVirtualAddress() +
                                                  entries.offset);
        cmd->SetComputeRootUnorderedAccessView(SUM_CARRY_PARAM,
                                               carry.Get()->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(SUM_RESULTS_PARAM,
                                               results.Get()->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstants(SUM_RUNTIME_DATA_PARAM,
                                          static_cast<UINT>(runtime_words.size()),
                                          runtime_words.data(), 0);
        cmd->Dispatch(groups, 1, 1);
        results.Transition(scheduler, D3D12_RESOURCE_STATE_COPY_SOURCE, barriers);
        Barrier(cmd, barriers);

        ReleaseGpuRefs();
        epoch_slots.clear();
        for (size_t i = 0; i < epoch_queries.size(); ++i) {
            synced.push_back({epoch_queries[i].id, static_cast<u32>(i)});
        }
        epoch_queries.clear();
        epoch_reset.reset();
        carry_parity ^= 1;
    }

    /// Copies the reports PresyncWrites summed to the guest's memory in the buffer cache.
    void SyncWrites() override {
        if (synced.empty()) {
            return;
        }
        sync_values.clear();
        {
            std::scoped_lock lock{guard};
            for (const auto& [query_id, index] : synced) {
                CounterQuery& query = slot_queries[query_id];
                if (True(query.flags & QueryFlagBits::IsRewritten) ||
                    True(query.flags & QueryFlagBits::IsInvalidated)) {
                    continue;
                }
                query.flags |= QueryFlagBits::IsHostSynced;
                sync_values.push_back({
                    .address = query.guest_address,
                    .size = True(query.flags & QueryFlagBits::HasTimestamp) ? 8U : 4U,
                    .offset = u64{index} * sizeof(u64),
                });
            }
        }
        synced.clear();
        if (!sync_values.empty()) {
            impl.runtime.SyncValues<HostSyncValues>(sync_values, results.Get());
        }
    }

    bool HasUnsyncedQueries() const override {
        return log_has_query || log_slots > SPILL_SLOTS;
    }

    /// Resolves the reports' slots to their banks' readback memory and hands them to the fence
    /// thread, which sums them once the GPU is past this fence.
    void PushUnsyncedQueries() override {
        CommandStream* const cmd = impl.scheduler.CommandList();
        const u64 tick = impl.scheduler.CurrentTick();
        ForEachRun(
            log,
            [](const Event& event) {
                return event.kind == Event::Kind::Slot ? &event.slot : nullptr;
            },
            [&](Bank* bank, u32 first, u32 count, u32) {
                // The readback heap stays in COPY_DEST.
                cmd->ResolveQueryData(bank->heap.Get(), kind.query, first, count,
                                      bank->readback.Get(), u64{first} * kind.stride);
                bank->last_tick = tick;
            });
        std::vector<Event> batch;
        {
            std::scoped_lock lock{flush_mutex};
            if (!spare_batches.empty()) {
                batch = std::move(spare_batches.back());
                spare_batches.pop_back();
            }
        }
        batch.swap(log);
        {
            std::scoped_lock lock{flush_mutex};
            batches.push_back(std::move(batch));
        }
        log_has_query = false;
        log_slots = 0;
    }

    /// On the fence thread, once the GPU is past the fence of the batch.
    void PopUnsyncedQueries() override {
        std::vector<Event> batch;
        {
            std::scoped_lock lock{flush_mutex};
            if (batches.empty()) {
                return;
            }
            batch = std::move(batches.front());
            batches.pop_front();
        }
        {
            std::scoped_lock lock{guard};
            for (const Event& event : batch) {
                switch (event.kind) {
                case Event::Kind::Slot: {
                    u64 value;
                    std::memcpy(&value,
                                event.slot.bank->readback_data.data() +
                                    u64{event.slot.index} * kind.stride + kind.value_offset,
                                sizeof(value));
                    since_reset += value;
                    event.slot.bank->refs.fetch_sub(1, std::memory_order_release);
                    break;
                }
                case Event::Kind::Reset:
                    since_reset = 0;
                    break;
                case Event::Kind::Query: {
                    CounterQuery& query = slot_queries[event.query];
                    query.value = since_reset;
                    query.flags |= QueryFlagBits::IsFinalValueSynced;
                    if (VideoCommon::FullCapture::Active()) {
                        VideoCommon::FullCapture::Event(
                            "query_host_result",
                            fmt::format("{{\"type\":{},\"value\":{}}}", GetId(), since_reset));
                    }
                    if (query.free_when_synced) {
                        ReleaseQuery(event.query);
                    }
                    break;
                }
                }
            }
        }
        batch.clear();
        std::scoped_lock lock{flush_mutex};
        spare_batches.push_back(std::move(batch));
    }

    /// The cache may free a report before the fence thread summed it (a fence at low GPU
    /// accuracy): its id is reused only once the sum no longer writes it.
    void Free(size_t query_id) override {
        std::scoped_lock lock{guard};
        CounterQuery& query = slot_queries[query_id];
        if (True(query.flags & QueryFlagBits::IsFinalValueSynced)) {
            ReleaseQuery(query_id);
        } else {
            query.free_when_synced = true;
        }
    }

private:
    struct Event {
        enum class Kind : u8 { Slot, Reset, Query };
        Slot slot{};
        size_t query{};
        Kind kind{};
    };

    struct EpochQuery {
        size_t id;
        u32 first;
        u32 count;
        bool use_carry;
    };

    /// A free slot, taking a new bank when the current one is full.
    Slot AllocateSlot() {
        if (!current || current->used == BANK_SIZE) {
            if (current) {
                closed.push_back(current);
            }
            current = TakeBank();
        }
        current->refs.fetch_add(2, std::memory_order_relaxed);
        return Slot{current, current->used++};
    }

    /// The oldest closed bank when both sums are done with it and the GPU is past it, else a
    /// new one.
    Bank* TakeBank() {
        if (!closed.empty()) {
            Bank* const oldest = closed.front();
            if (oldest->refs.load(std::memory_order_acquire) == 0 &&
                impl.scheduler.IsFree(oldest->last_tick)) {
                closed.pop_front();
                oldest->used = 0;
                return oldest;
            }
        }
        auto bank = std::make_unique<Bank>();
        const D3D12_QUERY_HEAP_DESC desc{.Type = kind.heap, .Count = BANK_SIZE, .NodeMask = 0};
        ThrowIfFailed(impl.device.Get()->CreateQueryHeap(&desc, IID_PPV_ARGS(&bank->heap)),
                      "CreateQueryHeap");
        bank->readback = CreateMappedBuffer(impl.device.Get(), u64{BANK_SIZE} * kind.stride,
                                            D3D12_HEAP_TYPE_READBACK, bank->readback_data);
        banks.push_back(std::move(bank));
        LOG_DEBUG(Render, "D3D12: query bank {} created (counter {})", banks.size(), GetId());
        return banks.back().get();
    }

    /// The GPU sum is done with the epoch's slots (or never needs them).
    void ReleaseGpuRefs() {
        for (const Slot& slot : epoch_slots) {
            slot.bank->refs.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    const CounterKind kind;
    QueryCacheRuntime::Impl& impl;
    bool active{};
    std::optional<Slot> running;

    std::deque<std::unique_ptr<Bank>> banks;
    Bank* current{};
    std::deque<Bank*> closed;

    // GPU sum: the slots and reports since the last sum.
    std::vector<Slot> epoch_slots;
    std::vector<EpochQuery> epoch_queries;
    /// Where the last reset in the epoch was, if any: the slots before it are not counted.
    std::optional<u32> epoch_reset{0};
    u32 carry_parity{};
    std::vector<std::pair<size_t, u32>> synced;
    std::vector<HostSyncValues> sync_values;
    TrackedBuffer resolved;
    TrackedBuffer results;
    TrackedBuffer carry;

    // CPU sum: what happened since the last fence, then batches the fence thread pops in order.
    std::vector<Event> log;
    bool log_has_query{};
    size_t log_slots{};
    std::mutex flush_mutex;
    std::deque<std::vector<Event>> batches;
    std::vector<std::vector<Event>> spare_batches;
    u64 since_reset{}; ///< fence thread only
};

QueryCacheRuntime::Impl::Impl(QueryCacheRuntime& runtime_, const Device& device_,
                              Scheduler& scheduler_, const ShaderCompiler& compiler_,
                              BufferCache& buffer_cache_, BufferCacheRuntime& buffer_runtime_,
                              StagingBufferPool& staging_)
    : runtime{runtime_}, device{device_}, scheduler{scheduler_}, compiler{compiler_}, buffer_cache{buffer_cache_},
      buffer_runtime{buffer_runtime_}, staging{staging_},
      guest_streamer{GUEST_STREAMER_ID, runtime_},
      occlusion{std::make_unique<CounterStreamer>(OCCLUSION_STREAMER_ID, OCCLUSION, *this)},
      primitives{std::make_unique<CounterStreamer>(PRIMITIVES_STREAMER_ID, PRIMITIVES, *this)},
      stream_output{
          std::make_unique<CounterStreamer>(STREAM_OUTPUT_STREAMER_ID, STREAM_OUTPUT, *this)} {}

QueryCacheRuntime::Impl::~Impl() = default;

void QueryCacheRuntime::Impl::CreateSumPipeline() {
    std::array<D3D12_ROOT_PARAMETER, 6> params{};
    params[SUM_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[SUM_CONSTANTS_PARAM].Constants = {
        .ShaderRegister = 0, .RegisterSpace = PUSH_CONSTANT_SPACE, .Num32BitValues = 4};
    params[SUM_RESOLVED_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[SUM_RESOLVED_PARAM].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    params[SUM_ENTRIES_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[SUM_ENTRIES_PARAM].Descriptor = {.ShaderRegister = 1, .RegisterSpace = 0};
    params[SUM_CARRY_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[SUM_CARRY_PARAM].Descriptor = {.ShaderRegister = 2, .RegisterSpace = 0};
    params[SUM_RESULTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[SUM_RESULTS_PARAM].Descriptor = {.ShaderRegister = 3, .RegisterSpace = 0};
    params[SUM_RUNTIME_DATA_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[SUM_RUNTIME_DATA_PARAM].Constants = {
        .ShaderRegister = 0,
        .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32)),
    };
    for (auto& param : params) {
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    sum_root_signature = CreateRootSignature(device.Get(), desc, "query sum root signature");
    const std::vector<u8> dxil = compiler.Compile(D3D12_QUERY_SUM_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = sum_root_signature.Get(),
        .CS = {dxil.data(), dxil.size()},
    };
    ThrowIfFailed(
        device.Get()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&sum_pipeline)),
        "CreateComputePipelineState (query sum)");
}

QueryCacheRuntime::QueryCacheRuntime(const Device& device, Scheduler& scheduler,
                                     const ShaderCompiler& compiler, BufferCache& buffer_cache,
                                     BufferCacheRuntime& buffer_runtime,
                                     StagingBufferPool& staging)
    : impl{std::make_unique<Impl>(*this, device, scheduler, compiler, buffer_cache,
                                  buffer_runtime, staging)} {
    // A query begins and ends in one command list.
    scheduler.RegisterOnSubmit([this] { PauseCounters(); });
}

QueryCacheRuntime::~QueryCacheRuntime() {
    try {
        if (SUCCEEDED(impl->device.Get()->GetDeviceRemovedReason())) {
            PauseCounters();
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "Query teardown: {}", e.what());
    }
    impl->scheduler.ClearSubmissionCallbacks();
    // The banks' heaps and readback memory outlive every list that uses them.
    impl->scheduler.DrainForShutdown();
}

bool QueryCacheRuntime::IsHostCounted(QueryType type) noexcept {
    switch (type) {
    case QueryType::ZPassPixelCount64:
    case QueryType::PrimitivesGenerated:
    case QueryType::VtgPrimitivesOut:
    case QueryType::StreamingPrimitivesSucceeded:
        return true;
    default:
        return false;
    }
}

void QueryCacheRuntime::PrepareDraw(bool zpass_enabled) {
    if (zpass_enabled) {
        impl->occlusion->StartCounter();
    } else {
        impl->occlusion->PauseCounter();
    }
    if (impl->primitives->Active()) {
        impl->primitives->StartCounter();
    }
    if (impl->stream_output->Active()) {
        impl->stream_output->StartCounter();
    }
}

void QueryCacheRuntime::PauseCounters() {
    impl->occlusion->PauseCounter();
    impl->primitives->PauseCounter();
    impl->stream_output->PauseCounter();
}

VideoCommon::StreamerInterface* QueryCacheRuntime::GetStreamerInterface(QueryType type) {
    switch (type) {
    case QueryType::Payload:
        return &impl->guest_streamer;
    case QueryType::ZPassPixelCount64:
        return impl->occlusion.get();
    case QueryType::PrimitivesGenerated:
    case QueryType::VtgPrimitivesOut:
        return impl->primitives.get();
    case QueryType::StreamingPrimitivesSucceeded:
        return impl->stream_output.get();
    default:
        return nullptr;
    }
}

template <typename SyncValuesType>
void QueryCacheRuntime::SyncValues(std::span<SyncValuesType> values, ID3D12Resource* base_src) {
    if (values.empty()) {
        return;
    }
    constexpr bool generates = SyncValuesType::GeneratesBaseBuffer;
    // By address: values on neighbouring pages share one buffer lookup and one copy call.
    std::ranges::sort(values, {}, &SyncValuesType::address);
    ID3D12Resource* source = base_src;
    u64 source_offset = 0;
    if constexpr (generates) {
        const StagingBufferRef ref =
            impl->staging.Request(values.size() * sizeof(u64), MemoryUsage::Upload);
        for (size_t i = 0; i < values.size(); ++i) {
            std::memcpy(ref.mapped_span.data() + i * sizeof(u64), &values[i].value,
                        values[i].size);
        }
        source = ref.buffer;
        source_offset = ref.offset;
    }
    auto& ranges = impl->ranges;
    ranges.clear();
    for (const SyncValuesType& value : values) {
        const DAddr begin = Common::AlignDown(value.address, Core::DEVICE_PAGESIZE);
        const DAddr end = Common::AlignUp(value.address + value.size, Core::DEVICE_PAGESIZE);
        if (!ranges.empty() && begin <= ranges.back().second) {
            ranges.back().second = std::max(ranges.back().second, end);
        } else {
            ranges.emplace_back(begin, end);
        }
    }
    BufferCache& buffer_cache = impl->buffer_cache;
    std::scoped_lock lock{buffer_cache.mutex};
    auto& targets = impl->targets;
    buffer_cache.BufferOperations([&] {
        targets.clear();
        for (const auto& [begin, end] : ranges) {
            targets.push_back(buffer_cache.ObtainCPUBuffer(
                begin, static_cast<u32>(end - begin),
                VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                VideoCommon::ObtainBufferOperation::DoNothing));
        }
    });
    auto& copies = impl->copies;
    size_t value_index = 0;
    for (size_t range = 0; range < ranges.size(); ++range) {
        const auto [buffer, buffer_offset] = targets[range];
        copies.clear();
        for (; value_index < values.size() && values[value_index].address < ranges[range].second;
             ++value_index) {
            const SyncValuesType& value = values[value_index];
            u64 src_offset;
            if constexpr (generates) {
                src_offset = source_offset + value_index * sizeof(u64);
            } else {
                src_offset = value.offset;
            }
            copies.push_back({
                .src_offset = src_offset,
                .dst_offset = buffer_offset + (value.address - ranges[range].first),
                .size = value.size,
            });
        }
        if (buffer && buffer->Handle() && !copies.empty()) {
            impl->buffer_runtime.CopyBuffer(*buffer, source, copies, true);
        }
    }
}

template void QueryCacheRuntime::SyncValues<VideoCommon::SyncValuesStruct>(
    std::span<VideoCommon::SyncValuesStruct>, ID3D12Resource*);
template void QueryCacheRuntime::SyncValues<HostSyncValues>(std::span<HostSyncValues>,
                                                            ID3D12Resource*);

} // namespace D3D12

namespace VideoCommon {
template class QueryCacheBase<D3D12::QueryCacheParams>;
} // namespace VideoCommon
