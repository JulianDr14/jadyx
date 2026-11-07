// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/memory_edit/jme_scanner.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "common/permafrost.h"
#include "jadyx/memory_edit/jme_matcher.h"

namespace Jadyx::MemoryEdit {

namespace {

namespace Permafrost = Common::Permafrost;

// How much memory the mutex stays held for.
constexpr u64 SLICE_BYTES = 64 * 1024 * 1024;
constexpr u32 DISK_WORKERS = 2;
constexpr u32 PAGE_BITS = 12;

u64 NowMs() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// The same job on the caller and on `workers - 1` threads, one job at a time.
class Crew {
public:
    explicit Crew(u32 workers) {
        for (u32 i = 1; i < workers; ++i) {
            threads.emplace_back([this, i] { Serve(i); });
        }
    }

    ~Crew() {
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }
        wake.notify_all();
        threads.clear();
    }

    void Run(u32 workers, const std::function<void(u32)>& job_) {
        const u32 helpers = (std::min)(workers - 1, static_cast<u32>(threads.size()));
        {
            std::scoped_lock lock{mutex};
            job = &job_;
            helpers_wanted = helpers;
            pending = helpers;
            ++generation;
        }
        wake.notify_all();
        job_(0);
        std::unique_lock lock{mutex};
        finished.wait(lock, [this] { return pending == 0; });
        job = nullptr;
    }

private:
    void Serve(u32 index) {
        u64 seen = 0;
        std::unique_lock lock{mutex};
        for (;;) {
            wake.wait(lock, [&] { return closing || generation != seen; });
            if (closing) return;
            seen = generation;
            if (index > helpers_wanted) continue;
            const auto* const current = job;
            lock.unlock();
            (*current)(index);
            lock.lock();
            if (--pending == 0) finished.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    const std::function<void(u32)>* job{};
    u32 helpers_wanted{};
    u32 pending{};
    u64 generation{};
    bool closing{};
    std::vector<std::jthread> threads; // last: they stop before the rest goes
};

/// What one thread writes: a chunk at a time, into the shared file and list.
class Sink {
public:
    Sink(u8* buffer_, ResultFile& file_, ResultList& list_, std::mutex& list_mutex_)
        : buffer{buffer_}, writer{buffer_}, file{file_}, list{list_}, list_mutex{list_mutex_} {}

    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    ~Sink() {
        FreeChunkBuffer(buffer);
    }

    [[nodiscard]] bool Add(u64 page, ValueKind kind, const u64* bits, u32 hits) {
        if (writer.Append(page, kind, bits, hits)) return true;
        return Flush() && writer.Append(page, kind, bits, hits);
    }

    [[nodiscard]] bool Flush() {
        if (writer.Hits() == 0) return true;
        const auto slot = file.Write(buffer);
        if (!slot) return false;
        {
            std::scoped_lock lock{list_mutex};
            list.push_back({*slot, writer.Hits()});
        }
        writer.Reset();
        return true;
    }

private:
    u8* buffer;
    ChunkWriter writer;
    ResultFile& file;
    ResultList& list;
    std::mutex& list_mutex;
};

struct Search {
    const ScanParams& params;
    std::unique_ptr<std::atomic<u64>[]> marks;
    std::vector<std::unique_ptr<Sink>> sinks;
    std::atomic<u64> hits[VALUE_KINDS]{};
    std::atomic<u64> disk_hits{};
    std::atomic<u64> bytes{};
    std::atomic<bool> stop{};
    std::atomic<ScanFailure> failure{ScanFailure::None};

    [[nodiscard]] bool Stopped() const {
        return stop.load(std::memory_order_relaxed) ||
               (params.cancel != nullptr && params.cancel->load(std::memory_order_relaxed));
    }

    // The list could not take a result.
    void Fail() {
        Fail(params.file.Full() ? ScanFailure::TooManyResults : ScanFailure::DiskError);
    }

    void Fail(ScanFailure why) {
        failure.store(why);
        stop.store(true, std::memory_order_relaxed);
    }

    void Count(ValueKind kind, u32 found, bool from_disk) {
        hits[static_cast<size_t>(kind)].fetch_add(found, std::memory_order_relaxed);
        if (from_disk) disk_hits.fetch_add(found, std::memory_order_relaxed);
    }

    void Report(ScanPhase phase) const {
        if (params.progress == nullptr) return;
        params.progress->phase.store(phase, std::memory_order_relaxed);
        params.progress->permille.store(0, std::memory_order_relaxed);
    }

    void Report(size_t done, size_t total) const {
        if (params.progress == nullptr || total == 0) return;
        params.progress->permille.store(static_cast<u32>((std::min)(done, total) * 1000 / total),
                                        std::memory_order_relaxed);
    }

    // True the first time.
    [[nodiscard]] bool Mark(u32 mark) {
        const u64 bit = u64{1} << (mark % 64);
        std::atomic<u64>& word = marks[mark / 64];
        // Read first: a second pass finds nearly all of them set.
        if ((word.load(std::memory_order_relaxed) & bit) != 0) return false;
        return (word.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
    }
};

struct Walk {
    Search& search;
    Sink& sink;
    const GuestSpan& span;
    bool from_disk{};
};

// Runs with the granule held: it touches nothing of the backing but `data`.
void VisitPages(void* context, size_t first_page, size_t count, const u8* data) {
    auto& walk = *static_cast<Walk*>(context);
    Search& search = walk.search;
    const QuerySet& queries = search.params.queries;
    u64 bits[MATCH_WORDS];
    u64 compared = 0;
    for (size_t i = 0; i < count && !search.Stopped(); ++i) {
        const size_t index = first_page + i;
        if (!search.Mark(walk.span.first_mark + static_cast<u32>(index))) continue;
        const u8* const page = data + i * Permafrost::PAGE_SIZE;
        for (u32 q = 0; q < queries.count; ++q) {
            const Query& query = queries.items[q];
            const u32 found = MatchPage(query, page, bits);
            if (found == 0) continue;
            if (!walk.sink.Add((walk.span.va >> PAGE_BITS) + index, query.kind, bits, found)) {
                search.Fail();
                return;
            }
            search.Count(query.kind, found, walk.from_disk);
        }
        compared += Permafrost::PAGE_SIZE;
    }
    search.bytes.fetch_add(compared, std::memory_order_relaxed);
}

// Everything committed, a slice at a time. The caller holds the mutex for its workers: none of them
// may take it, and a writer or a read back holding a granule never does either.
void MemoryPass(Search& search, Crew& crew, u32 workers) {
    const auto spans = search.params.spans;
    for (size_t next = 0; next < spans.size() && !search.Stopped();) {
        search.Report(next, spans.size());
        size_t end = next;
        for (u64 bytes = 0; end < spans.size() && bytes < SLICE_BYTES; ++end) {
            bytes += u64{spans[end].pages} * Permafrost::PAGE_SIZE;
        }
        std::atomic<size_t> cursor{next};
        std::scoped_lock lock{search.params.decommit_mutex};
        crew.Run(workers, [&](u32 worker) {
            while (!search.Stopped()) {
                const size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
                if (index >= end) return;
                Walk walk{search, *search.sinks[worker], spans[index]};
                Permafrost::InspectCommitted(spans[index].host, spans[index].pages, VisitPages, &walk);
            }
        });
        next = end;
    }
}

// What the swap file holds. No lock: each read holds its granule only until it is in the buffer.
void DiskPass(Search& search, Crew& crew, u32 workers) {
    const auto spans = search.params.spans;
    std::atomic<size_t> cursor{0};
    crew.Run((std::min)(workers, DISK_WORKERS), [&](u32 worker) {
        while (!search.Stopped()) {
            const size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
            if (index >= spans.size()) return;
            search.Report(index, spans.size());
            Walk walk{search, *search.sinks[worker], spans[index], true};
            if (!Permafrost::InspectSwapped(spans[index].host, spans[index].pages, VisitPages, &walk)) {
                search.Fail(ScanFailure::DiskError);
            }
        }
    });
}

// One sink per thread into `list`. False when there is no memory for them.
bool MakeSinks(const ScanParams& params, u32 workers, ResultList& list, std::mutex& list_mutex,
               std::vector<std::unique_ptr<Sink>>& sinks) {
    for (u32 i = 0; i < workers; ++i) {
        u8* const buffer = NewChunkBuffer();
        if (buffer == nullptr) return false;
        sinks.push_back(std::make_unique<Sink>(buffer, params.file, list, list_mutex));
    }
    return true;
}

template <typename Pass>
void TimedPass(Search& search, u64& bytes_out, u64& ms_out, Pass&& pass) {
    const u64 start = NowMs();
    search.bytes.store(0, std::memory_order_relaxed);
    pass();
    ms_out = NowMs() - start;
    bytes_out = search.bytes.load(std::memory_order_relaxed);
}

void FlushAll(Search& search, const std::vector<std::unique_ptr<Sink>>& sinks) {
    for (const auto& sink : sinks) {
        if (!sink->Flush()) search.Fail();
    }
}

void FillStats(const Search& search, ScanStats& stats) {
    for (size_t kind = 0; kind < VALUE_KINDS; ++kind) stats.hits[kind] = search.hits[kind].load();
    stats.disk_hits = search.disk_hits.load();
    stats.failure = search.failure.load();
    stats.cancelled = search.params.cancel != nullptr && search.params.cancel->load();
}

// What a page holds when it is in neither memory nor the swap.
alignas(MATCH_PAGE_SIZE) constexpr u8 ZERO_PAGE[MATCH_PAGE_SIZE]{};

struct ChunkBufferFree {
    void operator()(u8* buffer) const {
        FreeChunkBuffer(buffer);
    }
};
using ChunkBuffer = std::unique_ptr<u8, ChunkBufferFree>;

struct Refining {
    Search& search;
    const Query* by_kind[VALUE_KINDS]{};
    /// Candidates the pass over memory did not find, to look for on disk. They use the file too:
    /// a list of thousands of pages on disk is what must not stay in RAM.
    std::vector<std::unique_ptr<Sink>> later;
    ResultList later_list;
    std::vector<ChunkBuffer> reads;
};

struct Candidate {
    Search& search;
    Sink& sink;
    const ChunkRecord& record;
    const Query& query;
    bool from_disk{};
    bool seen{};
};

void CompareCandidate(Candidate& candidate, const u8* page) {
    const ChunkRecord& record = candidate.record;
    Search& search = candidate.search;
    search.bytes.fetch_add(MATCH_PAGE_SIZE, std::memory_order_relaxed);
    u64 bits[MATCH_WORDS];
    if (MatchPage(candidate.query, page, bits) == 0) return;
    u32 kept = 0;
    for (size_t word = 0; word < MatchWords(record.kind); ++word) {
        bits[word] &= record.bits[word];
        kept += static_cast<u32>(std::popcount(bits[word]));
    }
    if (kept == 0) return;
    if (!candidate.sink.Add(record.page, record.kind, bits, kept)) {
        search.Fail();
        return;
    }
    search.Count(record.kind, kept, candidate.from_disk);
}

void VisitCandidate(void* context, size_t, size_t, const u8* data) {
    auto& candidate = *static_cast<Candidate*>(context);
    candidate.seen = true;
    CompareCandidate(candidate, data);
}

// Where the page lives now, null when it is no longer mapped as memory a game keeps values in.
const u8* HostOfPage(std::span<const GuestSpan> spans, u64 page) {
    const u64 va = page << PAGE_BITS;
    const auto after = std::upper_bound(spans.begin(), spans.end(), va,
                                        [](u64 value, const GuestSpan& span) { return value < span.va; });
    if (after == spans.begin()) return nullptr;
    const GuestSpan& span = *(after - 1);
    const u64 index = (va - span.va) >> PAGE_BITS;
    return index < span.pages ? span.host + index * Permafrost::PAGE_SIZE : nullptr;
}

// Calls `use(record, query, host)` for the candidates of a chunk that can still be one.
template <typename Use>
void ForEachCandidate(Refining& refining, const u8* chunk, Use&& use) {
    Search& search = refining.search;
    ChunkReader reader{chunk};
    ChunkRecord record;
    while (!search.Stopped() && reader.Next(record)) {
        const Query* const query = refining.by_kind[static_cast<size_t>(record.kind)];
        const u8* const host = HostOfPage(search.params.spans, record.page);
        if (query != nullptr && host != nullptr) use(record, *query, host);
    }
    if (reader.Broken()) search.Fail(ScanFailure::DiskError);
}

// Reads a chunk of a list per thread, without the mutex, then holds it while they compare: an
// emergency waits for this many chunks at most.
void RefineMemoryPass(Refining& refining, Crew& crew, u32 workers, const ResultList& previous) {
    Search& search = refining.search;
    const ScanParams& params = search.params;
    for (size_t next = 0; next < previous.size() && !search.Stopped(); next += workers) {
        search.Report(next, previous.size());
        crew.Run(workers, [&](u32 worker) {
            const size_t index = next + worker;
            if (index < previous.size() && !params.file.Read(previous[index].slot, refining.reads[worker].get())) {
                search.Fail(ScanFailure::DiskError);
            }
        });
        if (search.Stopped()) return;
        std::scoped_lock lock{params.decommit_mutex};
        crew.Run(workers, [&](u32 worker) {
            if (next + worker >= previous.size()) return;
            Sink& sink = *search.sinks[worker];
            Sink& later = *refining.later[worker];
            ForEachCandidate(refining, refining.reads[worker].get(),
                             [&](const ChunkRecord& record, const Query& query, const u8* host) {
                Candidate candidate{search, sink, record, query};
                Permafrost::InspectCommitted(host, 1, VisitCandidate, &candidate);
                if (!candidate.seen && !later.Add(record.page, record.kind, record.bits, record.hits)) {
                    search.Fail();
                }
            });
        });
    }
}

// The candidates memory did not have. Each is on disk, or came back meanwhile, or its page holds
// nothing but zeros.
void RefineDiskPass(Refining& refining, Crew& crew, u32 workers) {
    Search& search = refining.search;
    const ScanParams& params = search.params;
    const ResultList& list = refining.later_list;
    std::atomic<size_t> cursor{0};
    crew.Run((std::min)(workers, DISK_WORKERS), [&](u32 worker) {
        Sink& sink = *search.sinks[worker];
        u8* const buffer = refining.reads[worker].get();
        while (!search.Stopped()) {
            const size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
            if (index >= list.size()) return;
            search.Report(index, list.size());
            if (!params.file.Read(list[index].slot, buffer)) {
                search.Fail(ScanFailure::DiskError);
                return;
            }
            ForEachCandidate(refining, buffer, [&](const ChunkRecord& record, const Query& query, const u8* host) {
                Candidate candidate{search, sink, record, query, true};
                if (!Permafrost::InspectSwapped(host, 1, VisitCandidate, &candidate)) {
                    search.Fail(ScanFailure::DiskError);
                    return;
                }
                if (candidate.seen) return;
                candidate.from_disk = false;
                {
                    std::scoped_lock lock{params.decommit_mutex};
                    Permafrost::InspectCommitted(host, 1, VisitCandidate, &candidate);
                }
                if (!candidate.seen) CompareCandidate(candidate, ZERO_PAGE);
            });
        }
    });
}

} // namespace

ScanStats RunScan(const ScanParams& params, ResultList& list) {
    ScanStats stats;
    const u32 workers = (std::max)(params.workers, u32{1});
    u64 total_pages = 0;
    for (const GuestSpan& span : params.spans) {
        total_pages = (std::max)(total_pages, u64{span.first_mark} + span.pages);
    }

    std::mutex list_mutex;
    Search search{params};
    search.marks.reset(new (std::nothrow) std::atomic<u64>[(total_pages + 63) / 64]());
    if (search.marks == nullptr || !MakeSinks(params, workers, list, list_mutex, search.sinks)) {
        stats.failure = ScanFailure::NoMemory;
        return stats;
    }

    Crew crew{workers};
    const u64 swap_ins = Permafrost::GetStats().swap_ins;
    search.Report(ScanPhase::Memory);
    TimedPass(search, stats.memory_bytes, stats.memory_ms, [&] { MemoryPass(search, crew, workers); });
    stats.memory_complete = !search.Stopped();
    if (Permafrost::GetStats().swapped_bytes > 0) {
        search.Report(ScanPhase::Disk);
        TimedPass(search, stats.disk_bytes, stats.disk_ms, [&] { DiskPass(search, crew, workers); });
    }
    // A page read back after the first pass went by it and before the disk pass reached it was in
    // neither. A read back is the only way, and the bitmap makes this pass cheap.
    if (Permafrost::GetStats().swap_ins != swap_ins) {
        TimedPass(search, stats.sweep_bytes, stats.sweep_ms, [&] { MemoryPass(search, crew, workers); });
    }

    FlushAll(search, search.sinks);
    FillStats(search, stats);
    return stats;
}

ScanStats RunRefine(const ScanParams& params, const ResultList& previous, ResultList& list) {
    ScanStats stats;
    const u32 workers = (std::max)(params.workers, u32{1});
    std::mutex list_mutex;
    Search search{params};
    Refining refining{search};
    for (u32 q = 0; q < params.queries.count; ++q) {
        const Query& query = params.queries.items[q];
        refining.by_kind[static_cast<size_t>(query.kind)] = &query;
    }
    bool have_memory = MakeSinks(params, workers, list, list_mutex, search.sinks) &&
                       MakeSinks(params, workers, refining.later_list, list_mutex, refining.later);
    for (u32 i = 0; have_memory && i < workers; ++i) {
        refining.reads.emplace_back(NewChunkBuffer());
        have_memory = refining.reads.back() != nullptr;
    }
    if (!have_memory) {
        stats.failure = ScanFailure::NoMemory;
        return stats;
    }

    Crew crew{workers};
    search.Report(ScanPhase::Memory);
    TimedPass(search, stats.memory_bytes, stats.memory_ms,
              [&] { RefineMemoryPass(refining, crew, workers, previous); });
    stats.memory_complete = !search.Stopped();
    FlushAll(search, refining.later);
    if (!search.Stopped() && !refining.later_list.empty()) {
        // Without swap they are pages of zeros, which needs no word about a disk.
        if (Permafrost::GetStats().swapped_bytes > 0) search.Report(ScanPhase::Disk);
        TimedPass(search, stats.disk_bytes, stats.disk_ms, [&] { RefineDiskPass(refining, crew, workers); });
    }

    FlushAll(search, search.sinks);
    FillStats(search, stats);
    return stats;
}

} // namespace Jadyx::MemoryEdit
