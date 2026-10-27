// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/assert.h"
#include "common/logging.h"
#include "video_core/diagnostics/full_capture.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCore {
enum class QueryType {
    SamplesPassed,
    PrimitivesGenerated,
    TfbPrimitivesWritten,
    Count,
};
constexpr std::size_t NumQueryTypes = static_cast<size_t>(QueryType::Count);
} // namespace VideoCore

namespace VideoCommon {

using AsyncJobId = Common::SlotId;

static constexpr AsyncJobId NULL_ASYNC_JOB_ID{0};

/// Diagnostic ("occlusion_force_visible=1"): sample counts reach the guest as never zero, so
/// occlusion culling built on them keeps every object, and every conditional draw renders.
inline std::atomic<bool> force_occlusion_visible{};

template <class QueryCache, class HostCounter>
class CounterStreamBase {
public:
    explicit CounterStreamBase(QueryCache& cache_, VideoCore::QueryType type_)
        : cache{cache_}, type{type_} {}

    /// Resets the stream to zero. It doesn't disable the query after resetting.
    void Reset() {
        if (current) {
            current->EndQuery();
            current = nullptr;

            // Immediately start a new query to avoid disabling its state.
            Start(nullptr);
        }
        last = nullptr;
    }

    /// Returns the current counter slicing as needed.
    std::shared_ptr<HostCounter> Current() {
        if (!current) {
            return nullptr;
        }
        current->EndQuery();
        std::shared_ptr<HostCounter> slice = std::exchange(current, nullptr);
        Start(slice);
        // A flush while starting runs Disable(), which clears last.
        last = slice;
        return slice;
    }

    /// Returns true when the counter stream is enabled.
    bool IsEnabled() const {
        return current != nullptr;
    }

    /// Enables the stream.
    void Enable() {
        if (current) {
            return;
        }
        Start(last);
    }

    // Disables the stream.
    void Disable() {
        if (current) {
            current->EndQuery();
        }
        last = std::exchange(current, nullptr);
    }

private:
    /// Starts a counter as current. Creating it can wait on its dependency, and that wait can
    /// flush: the flush disables and re-enables this stream, which starts a counter of its own.
    /// That one is ended before it is replaced, or its query would be left open in the list.
    void Start(std::shared_ptr<HostCounter> dependency) {
        std::shared_ptr<HostCounter> counter = cache.Counter(std::move(dependency), type);
        if (current) {
            current->EndQuery();
        }
        current = std::move(counter);
    }

    QueryCache& cache;
    const VideoCore::QueryType type;

    std::shared_ptr<HostCounter> current;
    std::shared_ptr<HostCounter> last;
};

template <class QueryCache, class CachedQuery, class CounterStream, class HostCounter>
class QueryCacheLegacy : public VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit QueryCacheLegacy(VideoCore::RasterizerInterface& rasterizer_,
                              Tegra::MaxwellDeviceMemoryManager& device_memory_)
        : rasterizer{rasterizer_},
          // Use reinterpret_cast instead of static_cast as workaround for
          // UBSan bug (https://github.com/llvm/llvm-project/issues/59060)
          device_memory{device_memory_},
          streams{{
              {CounterStream{reinterpret_cast<QueryCache&>(*this),
                             VideoCore::QueryType::SamplesPassed}},
              {CounterStream{reinterpret_cast<QueryCache&>(*this),
                             VideoCore::QueryType::PrimitivesGenerated}},
              {CounterStream{reinterpret_cast<QueryCache&>(*this),
                             VideoCore::QueryType::TfbPrimitivesWritten}},
          }} {
        (void)slot_async_jobs.insert(); // Null value
    }

    void InvalidateRegion(VAddr addr, std::size_t size) {
        std::unique_lock lock{mutex};
        FlushAndRemoveRegion(addr, size);
    }

    void FlushRegion(VAddr addr, std::size_t size) {
        std::unique_lock lock{mutex};
        FlushAndRemoveRegion(addr, size);
    }

    /**
     * Records a query in GPU mapped memory, potentially marked with a timestamp.
     * @param gpu_addr  GPU address to flush to when the mapped memory is read.
     * @param type      Query type, e.g. SamplesPassed.
     * @param timestamp Timestamp, when empty the flushed query is assumed to be short.
     */
    void Query(GPUVAddr gpu_addr, VideoCore::QueryType type, std::optional<u64> timestamp) {
        std::unique_lock lock{mutex};
        const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        ASSERT(cpu_addr);

        CachedQuery* query = TryGet(*cpu_addr);
        if (!query) {
            ASSERT_OR_EXECUTE(cpu_addr, return;);
            u8* const host_ptr = gpu_memory->GetPointer(gpu_addr);

            query = Register(type, *cpu_addr, host_ptr, timestamp.has_value());
        }

        auto result = query->BindCounter(Stream(type).Current(), timestamp);
        if (result) {
            auto async_job_id = query->GetAsyncJob();
            auto& async_job = slot_async_jobs[async_job_id];
            async_job.collected = true;
            async_job.value = *result;
            query->SetAsyncJob(NULL_ASYNC_JOB_ID);
        }
        AsyncFlushQuery(query, type, timestamp, lock);
    }

    /// Enables all available GPU counters
    void EnableCounters() {
        std::unique_lock lock{mutex};
        for (auto& stream : streams) {
            stream.Enable();
        }
    }

    /// Resets a counter to zero. It doesn't disable the query after resetting.
    void ResetCounter(VideoCore::QueryType type) {
        std::unique_lock lock{mutex};
        Stream(type).Reset();
    }

    /// Disable all active streams. Expected to be called at the end of a command buffer.
    void DisableStreams() {
        std::unique_lock lock{mutex};
        for (auto& stream : streams) {
            stream.Disable();
        }
    }

    /// Returns a new host counter.
    std::shared_ptr<HostCounter> Counter(std::shared_ptr<HostCounter> dependency,
                                         VideoCore::QueryType type) {
        return std::make_shared<HostCounter>(static_cast<QueryCache&>(*this), std::move(dependency),
                                             type);
    }

    /// Returns the counter stream of the specified type.
    CounterStream& Stream(VideoCore::QueryType type) {
        return streams[static_cast<std::size_t>(type)];
    }

    /// Returns the counter stream of the specified type.
    const CounterStream& Stream(VideoCore::QueryType type) const {
        return streams[static_cast<std::size_t>(type)];
    }

    void CommitAsyncFlushes() {
        std::unique_lock lock{mutex};
        committed_flushes.push_back(uncommitted_flushes);
        uncommitted_flushes.reset();
    }

    bool HasUncommittedFlushes() const {
        std::unique_lock lock{mutex};
        return uncommitted_flushes != nullptr;
    }

    bool ShouldWaitAsyncFlushes() const {
        std::unique_lock lock{mutex};
        if (committed_flushes.empty()) {
            return false;
        }
        return committed_flushes.front() != nullptr;
    }

    /// Calls func with the query cached at addr (nullptr when there is none) under the cache lock.
    template <typename Func>
    decltype(auto) VisitQuery(VAddr addr, Func&& func) {
        std::unique_lock lock{mutex};
        return func(TryGet(addr));
    }

    void PopAsyncFlushes() {
        std::unique_lock lock{mutex};
        if (committed_flushes.empty()) {
            return;
        }
        auto& flush_list = committed_flushes.front();
        if (!flush_list) {
            committed_flushes.pop_front();
            return;
        }
        for (AsyncJobId async_job_id : *flush_list) {
            AsyncJob& async_job = slot_async_jobs[async_job_id];
            if (!async_job.collected) {
                FlushAndRemoveRegion(async_job.query_location, 2, true);
            }
        }
        committed_flushes.pop_front();
    }

private:
    struct AsyncJob {
        bool collected = false;
        u64 value = 0;
        VAddr query_location = 0;
        std::optional<u64> timestamp{};
    };

    /// Flushes a memory range to guest memory and removes it from the cache.
    void FlushAndRemoveRegion(VAddr addr, std::size_t size, bool async = false) {
        const u64 addr_begin = addr;
        const u64 addr_end = addr_begin + size;
        const auto in_range = [addr_begin, addr_end](const CachedQuery& query) {
            const u64 cache_begin = query.GetCpuAddr();
            const u64 cache_end = cache_begin + query.SizeInBytes();
            return cache_begin < addr_end && addr_begin < cache_end;
        };

        const u64 page_end = addr_end >> YUZU_PAGEBITS;
        for (u64 page = addr_begin >> YUZU_PAGEBITS; page <= page_end; ++page) {
            const auto& it = cached_queries.find(page);
            if (it == std::end(cached_queries)) {
                continue;
            }
            auto& contents = it->second;
            for (auto& query : contents) {
                if (!in_range(query)) {
                    continue;
                }
                AsyncJobId async_job_id = query.GetAsyncJob();
                auto flush_result = query.Flush(async);
                if (async_job_id == NULL_ASYNC_JOB_ID) {
                    ASSERT_MSG(false, "This should not be reachable at all");
                    continue;
                }
                AsyncJob& async_job = slot_async_jobs[async_job_id];
                async_job.collected = true;
                async_job.value = flush_result;
                query.SetAsyncJob(NULL_ASYNC_JOB_ID);
            }
            std::erase_if(contents, in_range);
        }
    }

    /// Registers the passed parameters as cached and returns a pointer to the stored cached query.
    CachedQuery* Register(VideoCore::QueryType type, VAddr cpu_addr, u8* host_ptr, bool timestamp) {
        const u64 page = static_cast<u64>(cpu_addr) >> YUZU_PAGEBITS;
        return &cached_queries[page].emplace_back(static_cast<QueryCache&>(*this), type, cpu_addr,
                                                  host_ptr);
    }

    /// Tries to a get a cached query. Returns nullptr on failure.
    CachedQuery* TryGet(VAddr addr) {
        const u64 page = static_cast<u64>(addr) >> YUZU_PAGEBITS;
        const auto it = cached_queries.find(page);
        if (it == std::end(cached_queries)) {
            return nullptr;
        }
        auto& contents = it->second;
        const auto found = std::find_if(std::begin(contents), std::end(contents),
                                        [addr](auto& query) { return query.GetCpuAddr() == addr; });
        return found != std::end(contents) ? &*found : nullptr;
    }

    void AsyncFlushQuery(CachedQuery* query, VideoCore::QueryType type,
                         std::optional<u64> timestamp,
                         std::unique_lock<std::recursive_mutex>& lock) {
        const auto reported = std::chrono::steady_clock::now();
        const AsyncJobId new_async_job_id = slot_async_jobs.insert();
        {
            AsyncJob& async_job = slot_async_jobs[new_async_job_id];
            query->SetAsyncJob(new_async_job_id);
            async_job.query_location = query->GetCpuAddr();
            async_job.collected = false;

            if (!uncommitted_flushes) {
                uncommitted_flushes = std::make_shared<std::vector<AsyncJobId>>();
            }
            uncommitted_flushes->push_back(new_async_job_id);
        }
        lock.unlock();
        std::function<void()> operation([this, new_async_job_id, type, timestamp, reported] {
            std::unique_lock local_lock{mutex};
            AsyncJob& async_job = slot_async_jobs[new_async_job_id];
            u64 value = async_job.value;
            VAddr address = async_job.query_location;
            slot_async_jobs.erase(new_async_job_id);
            local_lock.unlock();
            if (type == VideoCore::QueryType::SamplesPassed) {
                namespace Perf = VideoCore::Perf;
                const auto waited = std::chrono::steady_clock::now() - reported;
                Perf::Add(Perf::Counter::OcclusionResults, 1);
                Perf::Add(Perf::Counter::OcclusionResultsZero, value == 0 ? 1 : 0);
                Perf::Add(Perf::Counter::OcclusionResultLatencyUs,
                          std::chrono::duration_cast<std::chrono::microseconds>(waited).count());
                if (force_occlusion_visible.load(std::memory_order_relaxed)) {
                    value = (std::max<u64>)(value, 1ULL << 20);
                }
            }
            if (FullCapture::Active()) FullCapture::Event("query_guest_result", fmt::format(
                "{{\"address\":{},\"value\":{},\"timestamped\":{}}}",
                static_cast<u64>(address), value, timestamp.has_value()));
            {
                // Diagnostic: host counter results written back to the guest.
                static std::atomic<u64> written{};
                const u64 count = ++written;
                if (count <= 80 || (count & 127) == 0) {
                    LOG_WARNING(Render, "guest query result #{}: addr {:#x} value {} ({})", count,
                                static_cast<u64>(address), value,
                                timestamp ? "64-bit+timestamp" : "32-bit");
                }
            }
            if (timestamp) {
                u64 timestamp_value = *timestamp;
                device_memory.WriteBlockUnsafe(address + sizeof(u64), &timestamp_value,
                                               sizeof(u64));
                device_memory.WriteBlockUnsafe(address, &value, sizeof(u64));
                rasterizer.InvalidateRegion(address, sizeof(u64) * 2,
                                            VideoCommon::CacheType::NoQueryCache);
            } else {
                u32 small_value = static_cast<u32>(value);
                device_memory.WriteBlockUnsafe(address, &small_value, sizeof(u32));
                rasterizer.InvalidateRegion(address, sizeof(u32),
                                            VideoCommon::CacheType::NoQueryCache);
            }
        });
        rasterizer.SyncOperation(std::move(operation));
    }

    static constexpr std::uintptr_t YUZU_PAGESIZE = 4096;
    static constexpr unsigned YUZU_PAGEBITS = 12;

    Common::SlotVector<AsyncJob> slot_async_jobs;

    VideoCore::RasterizerInterface& rasterizer;
    Tegra::MaxwellDeviceMemoryManager& device_memory;

    mutable std::recursive_mutex mutex;

    ::Common::unordered_map<u64, std::vector<CachedQuery>> cached_queries;

    std::array<CounterStream, VideoCore::NumQueryTypes> streams;

    std::shared_ptr<std::vector<AsyncJobId>> uncommitted_flushes{};
    std::list<std::shared_ptr<std::vector<AsyncJobId>>> committed_flushes;
}; // namespace VideoCommon

/// A host query counting one slice of a guest counter, chained to the slices before it: its
/// value is its own count plus its dependency's. Every link is resolved and released in loops,
/// never by recursion, so a long chain costs neither a GPU wait to cut it short nor stack.
template <class QueryCache, class HostCounter>
class HostCounterBase {
public:
    explicit HostCounterBase(std::shared_ptr<HostCounter> dependency_)
        : dependency{std::move(dependency_)} {
        if (!dependency) {
            return;
        }
        depth = Link(dependency.get()).result ? 1 : Link(dependency.get()).depth + 1;
        if (depth > SETTLE_DEPTH) {
            // Settle the slices the GPU already counted instead of waiting for the whole chain.
            depth = SettleReady(dependency.get()) + 1;
        }
    }

    virtual ~HostCounterBase() {
        ReleaseChain(std::move(dependency));
    }

    /// Returns the current value of the query.
    u64 Query(bool async = false) {
        if (result) {
            return *result;
        }
        std::vector<HostCounterBase*> chain = Unread(this);
        u64 total = Settled(*chain.back());
        // Oldest first: the GPU counts them in order, so once the first wait returns the newer
        // slices are usually done too and read without one.
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            HostCounterBase& link = **it;
            const std::optional<u64> ready = link.TryQuery();
            total += ready ? *ready : link.BlockingQuery(async);
            link.Resolve(total);
        }
        return *result;
    }

    /// Returns true when flushing this query will potentially wait.
    bool WaitPending() const noexcept {
        return result.has_value();
    }

    u64 Depth() const noexcept {
        return depth;
    }

    /// The value, once Query or a newer counter's settling has read it.
    const std::optional<u64>& KnownResult() const noexcept {
        return result;
    }

    /// The counter this one continues from, while its value is still unread.
    const std::shared_ptr<HostCounter>& Dependency() const noexcept {
        return dependency;
    }

protected:
    /// Returns the value of query from the backend API blocking as needed.
    virtual u64 BlockingQuery(bool async = false) const = 0;

    /// Returns the value of query when the backend already has it, without waiting.
    virtual std::optional<u64> TryQuery() const {
        return std::nullopt;
    }

private:
    /// Unread links a new counter may sit on before it settles the ready ones below it.
    static constexpr u64 SETTLE_DEPTH = 16;

    static HostCounterBase& Link(HostCounter* counter) noexcept {
        return *counter;
    }

    /// The links from head down to the first one already read, newest first.
    static std::vector<HostCounterBase*> Unread(HostCounterBase* head) {
        std::vector<HostCounterBase*> chain;
        for (HostCounterBase* link = head; link && !link->result;
             link = link->dependency ? &Link(link->dependency.get()) : nullptr) {
            chain.push_back(link);
        }
        return chain;
    }

    /// The value below the oldest unread link: its read dependency's, or zero at the chain's end.
    static u64 Settled(const HostCounterBase& oldest) noexcept {
        return oldest.dependency ? *Link(oldest.dependency.get()).result : 0;
    }

    /// Reads, oldest first, the links below head the backend has finished without waiting, and
    /// returns how many are still unread.
    static u64 SettleReady(HostCounter* head) {
        std::vector<HostCounterBase*> chain = Unread(&Link(head));
        if (chain.empty()) {
            return 0;
        }
        u64 total = Settled(*chain.back());
        u64 unread = chain.size();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it, --unread) {
            const std::optional<u64> ready = (*it)->TryQuery();
            if (!ready) {
                break;
            }
            total += *ready;
            (*it)->Resolve(total);
        }
        for (u64 i = 0; i < unread; ++i) {
            chain[i]->depth = unread - i;
        }
        return unread;
    }

    /// Stores the value, which already includes the dependencies', and lets them go.
    void Resolve(u64 value) {
        result = value;
        depth = 0;
        ReleaseChain(std::move(dependency));
    }

    /// Drops one reference to a chain, destroying the links it was the last owner of one at a
    /// time: letting each destructor release the next would recurse once per link.
    static void ReleaseChain(std::shared_ptr<HostCounter> link) {
        while (link && link.use_count() == 1) {
            std::shared_ptr<HostCounter> next = std::move(Link(link.get()).dependency);
            link.reset();
            link = std::move(next);
        }
    }

    std::shared_ptr<HostCounter> dependency; ///< Counter to add to this value, while unread.
    std::optional<u64> result;               ///< The value, dependencies included, once read.
    u64 depth = 0;                           ///< Unread links below this one (may overcount).
};

template <class HostCounter>
class CachedQueryBase {
public:
    explicit CachedQueryBase(VAddr cpu_addr_, u8* host_ptr_)
        : cpu_addr{cpu_addr_}, host_ptr{host_ptr_} {}
    virtual ~CachedQueryBase() = default;

    CachedQueryBase(CachedQueryBase&&) noexcept = default;
    CachedQueryBase(const CachedQueryBase&) = delete;

    CachedQueryBase& operator=(CachedQueryBase&&) noexcept = default;
    CachedQueryBase& operator=(const CachedQueryBase&) = delete;

    /// Flushes the query to guest memory.
    virtual u64 Flush(bool async = false) {
        // When counter is nullptr it means that it's just been reset. We are supposed to write a
        // zero in these cases.
        const u64 value = counter ? counter->Query(async) : 0;
        if (async) {
            return value;
        }
        std::memcpy(host_ptr, &value, sizeof(u64));

        if (timestamp) {
            std::memcpy(host_ptr + TIMESTAMP_OFFSET, &*timestamp, sizeof(u64));
        }
        return value;
    }

    /// Binds a counter to this query.
    std::optional<u64> BindCounter(std::shared_ptr<HostCounter> counter_,
                                   std::optional<u64> timestamp_) {
        std::optional<u64> result{};
        if (counter) {
            // If there's an old counter set it means the query is being rewritten by the game.
            // To avoid losing the data forever, flush here.
            result = std::make_optional(Flush());
        }
        counter = std::move(counter_);
        timestamp = timestamp_;
        return result;
    }

    VAddr GetCpuAddr() const noexcept {
        return cpu_addr;
    }

    /// The counter whose value the guest reads here (nullptr after a reset: the value is zero).
    const std::shared_ptr<HostCounter>& GetCounter() const noexcept {
        return counter;
    }

    u64 SizeInBytes() const noexcept {
        return SizeInBytes(timestamp.has_value());
    }

    static constexpr u64 SizeInBytes(bool with_timestamp) noexcept {
        return with_timestamp ? LARGE_QUERY_SIZE : SMALL_QUERY_SIZE;
    }

    void SetAsyncJob(AsyncJobId assigned_async_job_) {
        assigned_async_job = assigned_async_job_;
    }

    AsyncJobId GetAsyncJob() const {
        return assigned_async_job;
    }

protected:
    /// Returns true when querying the counter may potentially block.
    bool WaitPending() const noexcept {
        return counter && counter->WaitPending();
    }

private:
    static constexpr std::size_t SMALL_QUERY_SIZE = 8;   // Query size without timestamp.
    static constexpr std::size_t LARGE_QUERY_SIZE = 16;  // Query size with timestamp.
    static constexpr std::intptr_t TIMESTAMP_OFFSET = 8; // Timestamp offset in a large query.

    VAddr cpu_addr;                       ///< Guest CPU address.
    u8* host_ptr;                         ///< Writable host pointer.
    std::shared_ptr<HostCounter> counter; ///< Host counter to query, owns the dependency tree.
    std::optional<u64> timestamp;         ///< Timestamp to flush to guest memory.
    AsyncJobId assigned_async_job;
};

} // namespace VideoCommon
