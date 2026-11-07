// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/permafrost.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Common::Permafrost {
namespace {

// Resident -> Probation and Swapped -> Probation belong to the swapper, which holds the decommit
// mutex. Probation -> Busy hands the granule to a writer, which takes no lock until it lets go.
// Probation -> Busy (a touch) and Swapped -> Busy (a read back) belong to the faulting thread or
// the prefetch thread. Busy is owned by whoever moved the granule into it.
enum State : u8 { Resident, Probation, Busy, Swapped };
using Mask = std::bitset<GRANULE_PAGES>;
constexpr u32 NO_SLOT = ~u32{};
constexpr u16 NO_PROBE = 0xFFFF;

// A read back covers whole chunks of the granule.
constexpr size_t CHUNK_PAGES = 16;
constexpr size_t GRANULE_CHUNKS = GRANULE_PAGES / CHUNK_PAGES;
// Chunks read on a first fault, doubled while the faults walk forward.
constexpr size_t BASE_WINDOW = 2;
// Chunks the prefetch thread reads at a time, letting the faults in between.
constexpr size_t PREFETCH_WINDOW = 8;
// A granule with this little left on disk comes back whole.
constexpr size_t SMALL_REST_PAGES = 2 * CHUNK_PAGES;
// Quick refaults that make a granule hot: the rest of it is prefetched after a fault.
constexpr u8 HOT_HEAT = 2;
constexpr u8 MAX_HEAT = 8;
// Buffers a read back lands in before any page of the granule is committed.
constexpr size_t READ_BUFFERS = 4;
constexpr size_t MAX_WRITERS = 4;
constexpr size_t PREFETCH_QUEUE = 256;
// Granules the scout looks at per hold of the decommit mutex.
constexpr size_t SCOUT_BATCH = 32;

struct Granule {
    std::atomic<u8> state{Resident};
    std::atomic<bool> has_data{}; // data.any(), readable without owning the granule
    std::atomic<bool> empty{};    // nothing committed at the last look, until a commit
    std::atomic<bool> prefetch_queued{};
    std::atomic<u16> probe{NO_PROBE}; // the no-access page the scout armed
    // Faulting threads between reading the state and acting on it. A granule leaves probation for
    // the disk only once none is, so no commit lands behind its scan.
    std::atomic<u32> in_flight{};
    std::atomic<u64> last_access{};
    std::atomic<u64> shield_until{};
    // Owned with the granule.
    u64 since{};           // when the probation began
    u64 probation_bytes{}; // committed bytes when it began
    u64 evicted_at{};      // when data last went out, until the first read back
    u32 slot{NO_SLOT};
    u8 heat{};
    u8 window{};           // chunks of the last read back
    u8 next_chunk{};       // the chunk after it
    Mask data;             // pages the slot holds; never committed while it does
};

struct Engine {
    u8* base{};
    size_t size{}; // whole granules only
    size_t granules{};
    Platform platform{};
    Config config;
    std::unique_ptr<Granule[]> table;
    std::unique_ptr<std::atomic<u64>[]> slots;
    size_t slot_words{};
    // Overlapped and unbuffered: reads back in several faults and the writers go at once.
    HANDLE file{INVALID_HANDLE_VALUE};
    u8* read_buffers[READ_BUFFERS]{};
    HANDLE read_events[READ_BUFFERS]{};
    std::atomic<u32> read_buffers_busy{};
    std::atomic<void*> reserve{};
    std::atomic<bool> reserve_released{};
    std::atomic<bool> stop{};

    // Granules handed to the writers, each Busy and owned by the queue until a writer takes it.
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::unique_ptr<u32[]> queue;
    size_t queue_head{};
    size_t queue_size{};
    std::atomic<u32> writer_jobs{};
    size_t writer_count{};
    // The span of a granule going out is gathered here and written at once.
    u8* staging[MAX_WRITERS]{};
    HANDLE write_events[MAX_WRITERS]{};
    std::thread writers[MAX_WRITERS];

    // Hot granules whose rest is read back ahead of the faults. Filled from the fault handler with
    // a try-lock: a busy queue skips the prefetch.
    std::mutex prefetch_mutex;
    std::condition_variable prefetch_ready;
    u32 prefetch_queue[PREFETCH_QUEUE]{};
    size_t prefetch_head{};
    size_t prefetch_size{};
    std::atomic<u32> prefetch_jobs{};
    std::thread prefetcher;

    // The swapper's own, under the step mutex.
    std::mutex step_mutex;
    std::vector<std::pair<u64, u32>> candidates;
    u64 next_scout{};
    u64 last_scout{};
    u64 scout_rounds{};
    u64 watch_epoch{}; // the scout's look began here: nothing was seen idle for longer
    u64 random{0x9E3779B97F4A7C15ULL};
    u64 thrash_at{};
    double thrash{};

    std::atomic<u64> released_pending{};
    std::atomic<u64> quick_pending{};
    std::atomic<u64> swapped_bytes{};
    std::atomic<u64> probation_bytes{};
    std::atomic<u64> writing_bytes{};
    std::atomic<u64> written_bytes{};
    std::atomic<u64> zero_bytes{};
    std::atomic<u64> read_bytes{};
    std::atomic<u64> swap_ins{};
    std::atomic<u64> window_reads{};
    std::atomic<u64> prefetch_reads{};
    std::atomic<u64> swap_in_us{};
    std::atomic<u64> swap_in_max_us{};
    std::atomic<u64> zero_faults{};
    std::atomic<u64> probation_starts{};
    std::atomic<u64> probation_touches{};
    std::atomic<u64> evictions{};
    std::atomic<u64> refaults{};
    std::atomic<u64> quick_refaults{};
    std::atomic<u64> probe_arms{};
    std::atomic<u64> probe_hits{};
    std::atomic<u64> write_failures{};
    std::atomic<u64> reserve_releases{};
    std::atomic<u64> file_slots{};
    std::atomic<u32> min_idle_ms{};
    std::atomic<u32> shielded{};
    std::atomic<u64> fault_stall_us{};
    // The batch being put on probation: since when and how much. Step only.
    u64 batch_at{};
    u64 batch_used{};
    // The last Describe, for the stall since it.
    std::atomic<u64> describe_at{};
    std::atomic<u64> describe_stall_us{};
};

std::atomic<Engine*> g_engine{};

// Diagnostic: the latest reads back, for a crash report. Written without a lock; a torn entry
// only misreports one line.
struct ReadRecord {
    u32 granule{};
    u16 first{}; // pages [first, last) of the read
    u16 last{};
    u32 thread{};
    bool prefetch{};
    bool ok{};
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point end{};
};
constexpr size_t READ_RECORDS = 2048;
std::array<ReadRecord, READ_RECORDS> g_reads{};
std::atomic<u64> g_read_serial{};

// The threads that run guest code. A read back pauses the others while it fills: a committed
// private page reads zero to every thread until it is filled, and the guest takes that zero for
// a pointer. The pause mutex keeps two fillers from suspending each other; a thread is only
// unregistered under it, so a paused handle is never closed meanwhile.
struct GuestThread {
    std::atomic<DWORD> id{};
    HANDLE handle{};
};
constexpr size_t MAX_GUEST_THREADS = 16;
std::array<GuestThread, MAX_GUEST_THREADS> g_guest_threads{};
std::mutex g_pause_mutex;

// Suspends every other guest thread and waits for each suspension to take effect. Nothing may
// allocate, lock or log until they are resumed: a paused thread may hold the heap lock.
size_t PauseGuests(std::array<HANDLE, MAX_GUEST_THREADS>& paused) {
    const DWORD self = GetCurrentThreadId();
    size_t count = 0;
    for (GuestThread& thread : g_guest_threads) {
        const DWORD id = thread.id.load(std::memory_order_acquire);
        if (id == 0 || id == self || SuspendThread(thread.handle) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(thread.handle, &context);
        paused[count++] = thread.handle;
    }
    return count;
}

void ResumeGuests(const std::array<HANDLE, MAX_GUEST_THREADS>& paused, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        ResumeThread(paused[i]);
    }
}

// How often a new batch may go on probation.
constexpr u64 BATCH_MS = 500;

u64 NowMs() {
    return GetTickCount64();
}

void Wait(const std::atomic<u8>& state) {
    for (u32 spin = 0; state.load(std::memory_order_acquire) == Busy; ++spin) {
        if (spin < 4096) {
            YieldProcessor();
        } else {
            Sleep(spin < 4160 ? 0 : 1);
        }
    }
}

// The first and one past the last page in the mask.
std::pair<size_t, size_t> Span(const Mask& mask) {
    size_t first = 0;
    while (!mask[first]) ++first;
    size_t last = GRANULE_PAGES;
    while (!mask[last - 1]) --last;
    return {first, last};
}

template <typename F>
void ForEachRun(const Mask& mask, F&& f) {
    for (size_t first = 0; first < GRANULE_PAGES;) {
        if (!mask[first]) {
            ++first;
            continue;
        }
        size_t last = first + 1;
        while (last < GRANULE_PAGES && mask[last]) ++last;
        f(first, last - first);
        first = last;
    }
}

Mask CommittedPages(const u8* begin) {
    Mask committed;
    for (size_t page = 0; page < GRANULE_PAGES;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(begin + page * PAGE_SIZE, &info, sizeof(info)) == 0) {
            break;
        }
        const size_t end = (std::min)(
            GRANULE_PAGES,
            static_cast<size_t>(static_cast<const u8*>(info.BaseAddress) + info.RegionSize - begin) /
                PAGE_SIZE);
        if (info.State == MEM_COMMIT) {
            for (size_t i = page; i < end; ++i) committed.set(i);
        }
        page = (std::max)(end, page + 1);
    }
    return committed;
}

// Whether the whole granule is reserved, from its first region alone.
bool NothingCommitted(const u8* begin) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(begin, &info, sizeof(info)) != 0 && info.State != MEM_COMMIT &&
           static_cast<const u8*>(info.BaseAddress) + info.RegionSize >= begin + GRANULE_SIZE;
}

DWORD PageProtection(const u8* page) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(page, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
        return 0;
    }
    return info.Protect;
}

bool ProtectRuns(const Engine& e, u8* begin, const Mask& mask, DWORD protection) {
    bool ok = true;
    ForEachRun(mask, [&](size_t first, size_t count) {
        ok &= e.platform.protect(begin + first * PAGE_SIZE, count * PAGE_SIZE, protection);
    });
    return ok;
}

bool IsClear(const u8* page) {
    const auto* const words = reinterpret_cast<const volatile u64*>(page);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(u64); i += 4) {
        if ((words[i] | words[i + 1] | words[i + 2] | words[i + 3]) != 0) {
            return false;
        }
    }
    return true;
}

u32 AllocSlot(Engine& e) {
    for (size_t word = 0; word < e.slot_words; ++word) {
        u64 bits = e.slots[word].load(std::memory_order_relaxed);
        while (bits != ~u64{}) {
            const int bit = std::countr_one(bits);
            if (e.slots[word].compare_exchange_weak(bits, bits | (u64{1} << bit))) {
                const u64 slot = word * 64 + static_cast<u64>(bit);
                for (u64 grown = e.file_slots.load(); grown < slot + 1 &&
                                                      !e.file_slots.compare_exchange_weak(grown, slot + 1);) {
                }
                return static_cast<u32>(slot);
            }
        }
    }
    return NO_SLOT;
}

void FreeSlot(Engine& e, u32 slot) {
    e.slots[slot / 64].fetch_and(~(u64{1} << (slot % 64)), std::memory_order_release);
}

bool Transfer(HANDLE file, HANDLE event, bool write, u8* memory, u64 offset, size_t size) {
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    at.hEvent = event;
    const BOOL started = write ? WriteFile(file, memory, static_cast<DWORD>(size), nullptr, &at)
                               : ReadFile(file, memory, static_cast<DWORD>(size), nullptr, &at);
    if (!started && GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    DWORD done{};
    return GetOverlappedResultEx(file, &at, &done, INFINITE, FALSE) && done == size;
}

void UpdateMax(std::atomic<u64>& max, u64 value) {
    for (u64 seen = max.load(); seen < value && !max.compare_exchange_weak(seen, value);) {
    }
}

void Shield(Granule& g, u64 until) {
    for (u64 seen = g.shield_until.load(std::memory_order_relaxed);
         seen < until && !g.shield_until.compare_exchange_weak(seen, until);) {
    }
}

// Lets go of an owned granule.
void Settle(Granule& g, State state) {
    g.has_data.store(g.data.any(), std::memory_order_relaxed);
    g.state.store(state, std::memory_order_release);
}

void SettleOut(Granule& g) {
    Settle(g, g.data.any() ? Swapped : Resident);
}

// The granule was touched on probation: its pages get their access back and it is left alone a
// while. A clear may decommit some of them meanwhile, failing their protect: the access is
// retried all the same, and a page left reserved faults again into the demand commit.
bool Restore(Engine& e, Granule& g, u8* begin, u64 now) {
    ProtectRuns(e, begin, CommittedPages(begin), PAGE_READWRITE);
    e.probation_bytes.fetch_sub(g.probation_bytes, std::memory_order_relaxed);
    e.probation_touches.fetch_add(1, std::memory_order_relaxed);
    g.last_access.store(now, std::memory_order_relaxed);
    Shield(g, now + e.config.shield_ms);
    SettleOut(g);
    return true;
}

size_t AcquireReadBuffer(Engine& e) {
    for (u32 spin = 0;; ++spin) {
        u32 busy = e.read_buffers_busy.load(std::memory_order_relaxed);
        for (size_t i = 0; i < READ_BUFFERS; ++i) {
            const u32 bit = u32{1} << i;
            if ((busy & bit) == 0 &&
                e.read_buffers_busy.compare_exchange_strong(busy, busy | bit, std::memory_order_acquire)) {
                return i;
            }
        }
        Sleep(spin < 64 ? 0 : 1);
    }
}

void ReleaseReadBuffer(Engine& e, size_t index) {
    e.read_buffers_busy.fetch_and(~(u32{1} << index), std::memory_order_release);
}

bool CommitPages(Engine& e, u8* address, size_t size) {
    return e.platform.commit(address, size) || (ReleaseReserve() && e.platform.commit(address, size));
}

// Reads back the data pages of chunks [first_chunk, last_chunk). Granule owned.
//
// Pages become visible to every thread the moment they are committed, before anything is copied
// into them: a private page cannot be filled out of sight. So the data is read first, and the
// guest threads are paused while the pages are committed and filled. Without the pause they go a
// few at a time, which still leaves another thread touching the same granule a window to see them
// zero, longer when the filler is preempted. Only data pages are committed: a reserved page next
// to them may be on disk too.
bool ReadBack(Engine& e, Granule& g, u8* begin, size_t first_chunk, size_t last_chunk,
              bool prefetch) {
    Mask want;
    for (size_t page = first_chunk * CHUNK_PAGES; page < last_chunk * CHUNK_PAGES; ++page) {
        if (g.data[page]) want.set(page);
    }
    if (want.none()) {
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto [first, last] = Span(want);
    const u64 record = g_read_serial.fetch_add(1, std::memory_order_relaxed);
    const size_t buffer_index = AcquireReadBuffer(e);
    u8* const buffer = e.read_buffers[buffer_index];
    bool ok = Transfer(e.file, e.read_events[buffer_index], false, buffer + first * PAGE_SIZE,
                       u64{g.slot} * GRANULE_SIZE + first * PAGE_SIZE, (last - first) * PAGE_SIZE);
    Mask filled;
    std::unique_lock<std::mutex> pause_lock;
    std::array<HANDLE, MAX_GUEST_THREADS> paused{};
    size_t paused_count = 0;
    if (ok && e.config.pause_fill) {
        pause_lock = std::unique_lock{g_pause_mutex};
        paused_count = PauseGuests(paused);
    }
    ForEachRun(want, [&](size_t run, size_t count) {
        const size_t fill_pages =
            e.config.pause_fill ? count : (std::max)(size_t{1}, size_t{e.config.fill_pages});
        for (size_t page = run; ok && page < run + count; page += fill_pages) {
            const size_t size = (std::min)(fill_pages, run + count - page) * PAGE_SIZE;
            u8* const target = begin + page * PAGE_SIZE;
            ok = CommitPages(e, target, size);
            if (ok) {
                std::memcpy(target, buffer + page * PAGE_SIZE, size);
                for (size_t i = page; i < page + size / PAGE_SIZE; ++i) filled.set(i);
            }
        }
    });
    ResumeGuests(paused, paused_count);
    if (pause_lock.owns_lock()) {
        pause_lock.unlock();
    }
    ReleaseReadBuffer(e, buffer_index);
    g_reads[record % READ_RECORDS] = ReadRecord{
        .granule = static_cast<u32>((begin - e.base) / GRANULE_SIZE),
        .first = static_cast<u16>(first),
        .last = static_cast<u16>(last),
        .thread = GetCurrentThreadId(),
        .prefetch = prefetch,
        .ok = ok,
        .start = start,
        .end = std::chrono::steady_clock::now(),
    };
    if (!ok) {
        // The slot still holds everything: what was filled goes again.
        ForEachRun(filled, [&](size_t run, size_t count) {
            VirtualFree(begin + run * PAGE_SIZE, count * PAGE_SIZE, MEM_DECOMMIT);
        });
        return false;
    }
    const u64 bytes = want.count() * PAGE_SIZE;
    g.empty.store(false, std::memory_order_relaxed);
    g.data &= ~want;
    if (g.data.none()) {
        FreeSlot(e, g.slot);
        g.slot = NO_SLOT;
    }
    e.swapped_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    e.read_bytes.fetch_add(bytes, std::memory_order_relaxed);
    e.swap_ins.fetch_add(1, std::memory_order_relaxed);
    if (g.data.any()) {
        e.window_reads.fetch_add(1, std::memory_order_relaxed);
    }
    if (prefetch) {
        e.prefetch_reads.fetch_add(1, std::memory_order_relaxed);
    } else {
        const auto us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
        e.swap_in_us.fetch_add(us, std::memory_order_relaxed);
        UpdateMax(e.swap_in_max_us, us);
    }
    return true;
}

void QueuePrefetch(Engine& e, size_t index) {
    Granule& g = e.table[index];
    if (g.prefetch_queued.exchange(true)) {
        return;
    }
    std::unique_lock lock{e.prefetch_mutex, std::try_to_lock};
    if (!lock.owns_lock() || e.prefetch_size == PREFETCH_QUEUE) {
        g.prefetch_queued.store(false);
        return;
    }
    e.prefetch_queue[(e.prefetch_head + e.prefetch_size++) % PREFETCH_QUEUE] = static_cast<u32>(index);
    e.prefetch_jobs.fetch_add(1);
    lock.unlock();
    e.prefetch_ready.notify_one();
}

// A fault in a granule with data on disk. Granule owned.
bool SwapIn(Engine& e, Granule& g, size_t index, u8* begin, size_t page, u64 now) {
    if (!g.data[page]) {
        // A zero page: committed alone, as the rest of its chunk may be on disk.
        bool ok = true;
        u8* const target = begin + page * PAGE_SIZE;
        if (PageProtection(target) == 0) {
            ok = CommitPages(e, target, PAGE_SIZE);
            g.empty.store(false, std::memory_order_relaxed);
            e.zero_faults.fetch_add(1, std::memory_order_relaxed);
        }
        SettleOut(g);
        return ok;
    }
    if (g.evicted_at != 0) {
        // The first read back since it went out: soon after means it went out hot.
        const u64 distance = now - g.evicted_at;
        g.evicted_at = 0;
        e.refaults.fetch_add(1, std::memory_order_relaxed);
        if (distance < e.config.quick_refault_ms) {
            g.heat = static_cast<u8>((std::min)(g.heat + 1, int{MAX_HEAT}));
            const u64 shield = (std::min)(u64{e.config.shield_ms} << (g.heat - 1),
                                          u64{e.config.shield_max_ms});
            Shield(g, now + shield);
            e.quick_refaults.fetch_add(1, std::memory_order_relaxed);
            e.quick_pending.fetch_add(1, std::memory_order_relaxed);
        } else if (distance > 4 * u64{e.config.quick_refault_ms}) {
            g.heat >>= 1;
        }
        g.window = 0;
    }
    g.last_access.store(now, std::memory_order_relaxed);
    const size_t chunk = page / CHUNK_PAGES;
    size_t first_chunk = chunk;
    size_t window = GRANULE_CHUNKS;
    bool prefetch = false;
    if (g.data.count() <= SMALL_REST_PAGES) {
        first_chunk = 0;
    } else {
        window = g.window != 0 && chunk == g.next_chunk
                     ? (std::min)(size_t{g.window} * 2, GRANULE_CHUNKS)
                     : BASE_WINDOW;
        prefetch = g.heat >= HOT_HEAT || window >= PREFETCH_WINDOW;
    }
    const size_t last_chunk = (std::min)(first_chunk + window, GRANULE_CHUNKS);
    g.window = static_cast<u8>(window);
    g.next_chunk = static_cast<u8>(last_chunk);
    const bool ok = ReadBack(e, g, begin, first_chunk, last_chunk, false);
    SettleOut(g);
    if (ok && prefetch && e.config.prefetch && g.has_data.load(std::memory_order_relaxed)) {
        QueuePrefetch(e, index);
    }
    return ok;
}

void PrefetchLoop(Engine* e) {
    if (e->platform.name_thread) e->platform.name_thread("PermafrostThaw");
    for (;;) {
        u32 index{};
        {
            std::unique_lock lock{e->prefetch_mutex};
            e->prefetch_ready.wait(lock, [e] { return e->stop.load() || e->prefetch_size != 0; });
            if (e->prefetch_size == 0) {
                return;
            }
            index = e->prefetch_queue[e->prefetch_head];
            e->prefetch_head = (e->prefetch_head + 1) % PREFETCH_QUEUE;
            --e->prefetch_size;
        }
        Granule& g = e->table[index];
        g.prefetch_queued.store(false);
        u8* const begin = e->base + size_t{index} * GRANULE_SIZE;
        // A window at a time, letting go in between: a fault elsewhere in it waits one window.
        for (size_t chunk = 0; chunk < GRANULE_CHUNKS && !e->stop.load();) {
            u8 state = Swapped;
            if (!g.state.compare_exchange_strong(state, Busy)) {
                if (state != Busy) break;
                Wait(g.state);
                continue;
            }
            const auto holds_data = [&](size_t c) {
                for (size_t page = c * CHUNK_PAGES; page < (c + 1) * CHUNK_PAGES; ++page) {
                    if (g.data[page]) return true;
                }
                return false;
            };
            while (chunk < GRANULE_CHUNKS && !holds_data(chunk)) ++chunk;
            const size_t last = (std::min)(chunk + PREFETCH_WINDOW, GRANULE_CHUNKS);
            const bool ok = chunk == GRANULE_CHUNKS || ReadBack(*e, g, begin, chunk, last, true);
            const bool rest = g.data.any();
            SettleOut(g);
            if (!ok || !rest) break;
            chunk = last;
            SwitchToThread();
        }
        e->prefetch_jobs.fetch_sub(1);
    }
}

// A granule off probation, Busy and handed over by the swapper. Takes no lock: a clear waiting on
// the granule holds the decommit mutex.
u64 Evict(Engine& e, Granule& g, u8* begin, size_t writer) {
    for (u32 spin = 0; g.in_flight.load() != 0; ++spin) {
        if (spin < 4096) {
            YieldProcessor();
        } else {
            Sleep(0);
        }
    }
    const u64 now = NowMs();
    const Mask committed = CommittedPages(begin);
    if (committed.none()) {
        g.empty.store(true, std::memory_order_relaxed);
        SettleOut(g);
        return 0;
    }
    const auto keep = [&] {
        ProtectRuns(e, begin, committed, PAGE_READWRITE);
        g.last_access.store(now, std::memory_order_relaxed);
        SettleOut(g);
        return u64{0};
    };
    // Read-only from here: readers go on, writers wait in the handler until the granule settles.
    if (!ProtectRuns(e, begin, committed, PAGE_READONLY)) {
        return keep();
    }
    Mask fresh;
    for (size_t i = 0; i < GRANULE_PAGES; ++i) {
        if (committed[i] && !IsClear(begin + i * PAGE_SIZE)) fresh.set(i);
    }
    u32 slot = g.slot;
    if (fresh.any()) {
        const bool new_slot = slot == NO_SLOT;
        if (new_slot) slot = AllocSlot(e);
        bool ok = slot != NO_SLOT;
        u8* const staging = e.staging[writer];
        // What the slot holds already stays: the new pages go in the stretches between.
        for (size_t first = 0; ok && first < GRANULE_PAGES;) {
            if (g.data[first]) {
                ++first;
                continue;
            }
            size_t last = first + 1;
            while (last < GRANULE_PAGES && !g.data[last]) ++last;
            size_t from = first;
            while (from < last && !fresh[from]) ++from;
            if (from < last) {
                size_t to = last;
                while (!fresh[to - 1]) --to;
                for (size_t i = from; i < to; ++i) {
                    if (fresh[i]) std::memcpy(staging + i * PAGE_SIZE, begin + i * PAGE_SIZE, PAGE_SIZE);
                }
                ok = Transfer(e.file, e.write_events[writer], true, staging + from * PAGE_SIZE,
                              u64{slot} * GRANULE_SIZE + from * PAGE_SIZE, (to - from) * PAGE_SIZE);
            }
            first = last;
        }
        if (!ok) {
            if (new_slot && slot != NO_SLOT) FreeSlot(e, slot);
            e.write_failures.fetch_add(1, std::memory_order_relaxed);
            return keep();
        }
        e.written_bytes.fetch_add(fresh.count() * PAGE_SIZE, std::memory_order_relaxed);
    }
    Mask released;
    ForEachRun(committed, [&](size_t first, size_t count) {
        u8* const run = begin + first * PAGE_SIZE;
        if (VirtualFree(run, count * PAGE_SIZE, MEM_DECOMMIT)) {
            for (size_t i = first; i < first + count; ++i) released.set(i);
        } else {
            e.platform.protect(run, count * PAGE_SIZE, PAGE_READWRITE);
        }
    });
    // A page that stayed committed keeps its data in memory.
    fresh &= released;
    g.data |= fresh;
    if (g.data.none() && slot != NO_SLOT) {
        FreeSlot(e, slot);
        slot = NO_SLOT;
    }
    g.slot = slot;
    if (fresh.any()) {
        g.evicted_at = now;
        g.window = 0;
        e.evictions.fetch_add(1, std::memory_order_relaxed);
    }
    g.empty.store(released == committed, std::memory_order_relaxed);
    e.swapped_bytes.fetch_add(fresh.count() * PAGE_SIZE, std::memory_order_relaxed);
    e.zero_bytes.fetch_add((released & ~fresh).count() * PAGE_SIZE, std::memory_order_relaxed);
    SettleOut(g);
    return released.count() * PAGE_SIZE;
}

void WriterLoop(Engine* e, size_t writer) {
    if (e->platform.name_thread) e->platform.name_thread("PermafrostFreeze");
    for (;;) {
        u32 index{};
        {
            std::unique_lock lock{e->queue_mutex};
            // What was handed over is written out even at shutdown: each granule is Busy.
            e->queue_ready.wait(lock, [e] { return e->stop.load() || e->queue_size != 0; });
            if (e->queue_size == 0) {
                return;
            }
            index = e->queue[e->queue_head];
            e->queue_head = (e->queue_head + 1) % e->granules;
            --e->queue_size;
        }
        Granule& g = e->table[index];
        const u64 bytes = g.probation_bytes;
        e->released_pending.fetch_add(Evict(*e, g, e->base + size_t{index} * GRANULE_SIZE, writer),
                                      std::memory_order_relaxed);
        e->writing_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        e->writer_jobs.fetch_sub(1);
    }
}

void QueueWrite(Engine& e, size_t index) {
    e.writer_jobs.fetch_add(1);
    {
        std::scoped_lock lock{e.queue_mutex};
        e.queue[(e.queue_head + e.queue_size++) % e.granules] = static_cast<u32>(index);
    }
    e.queue_ready.notify_one();
}

u64 NextRandom(Engine& e) {
    e.random ^= e.random << 13;
    e.random ^= e.random >> 7;
    e.random ^= e.random << 17;
    return e.random;
}

bool ArmPage(Engine& e, Granule& g, u8* begin, size_t page) {
    g.probe.store(static_cast<u16>(page));
    if (!e.platform.protect(begin + page * PAGE_SIZE, PAGE_SIZE, PAGE_NOACCESS)) {
        u16 armed = static_cast<u16>(page);
        g.probe.compare_exchange_strong(armed, NO_PROBE);
        return false;
    }
    e.probe_arms.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// One random committed page of a resident granule turns no-access. Decommit mutex held: no page
// of it is decommitted meanwhile, and only the swapper moves it off resident.
void Arm(Engine& e, Granule& g, u8* begin) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const size_t page = NextRandom(e) % GRANULE_PAGES;
        if (PageProtection(begin + page * PAGE_SIZE) == PAGE_READWRITE) {
            ArmPage(e, g, begin, page);
            return;
        }
    }
    // Sparse: one of the pages it has.
    const Mask committed = CommittedPages(begin);
    const size_t count = committed.count();
    if (count == 0) {
        g.empty.store(true, std::memory_order_relaxed);
        return;
    }
    size_t nth = NextRandom(e) % count;
    for (size_t page = 0; page < GRANULE_PAGES; ++page) {
        if (committed[page] && nth-- == 0) {
            if (PageProtection(begin + page * PAGE_SIZE) == PAGE_READWRITE) {
                ArmPage(e, g, begin, page);
            }
            return;
        }
    }
}

// Samples the resident granules: a probe still armed from the last round says untouched since.
void Scout(Engine& e, u64 now, std::mutex& decommit_mutex) {
    if (now - e.last_scout > 4 * u64{e.config.scout_ms}) {
        // A new look after a pause: what happened in it is unknown.
        e.watch_epoch = now;
    }
    e.last_scout = now;
    e.next_scout = now + e.config.scout_ms;
    const u64 round = e.scout_rounds++;
    // A granule hit lately is hot: armed again after a while, which halves the faults it costs.
    const u64 rearm_after = 2 * u64{e.config.scout_ms};
    for (size_t index = 0; index < e.granules;) {
        std::scoped_lock lock{decommit_mutex};
        for (size_t batch = 0; batch < SCOUT_BATCH && index < e.granules; ++batch, ++index) {
            Granule& g = e.table[index];
            if (g.state.load(std::memory_order_acquire) != Resident) {
                continue;
            }
            u8* const begin = e.base + index * GRANULE_SIZE;
            if (g.empty.load(std::memory_order_relaxed)) {
                // Something may commit pages without a fault: looked at again now and then.
                if (round % 8 != index % 8 || NothingCommitted(begin)) {
                    continue;
                }
                g.empty.store(false, std::memory_order_relaxed);
            }
            u16 probe = g.probe.load(std::memory_order_relaxed);
            if (probe != NO_PROBE) {
                // A trim of the page may have given its access back: checked every other round.
                if (round % 2 != index % 2 ||
                    PageProtection(begin + size_t{probe} * PAGE_SIZE) == PAGE_NOACCESS) {
                    continue;
                }
                g.probe.compare_exchange_strong(probe, NO_PROBE);
            }
            if (now - g.last_access.load(std::memory_order_relaxed) < rearm_after) {
                continue;
            }
            Arm(e, g, begin);
        }
    }
}

// Quick refaults fade with a half-life of ten seconds.
void UpdateThrash(Engine& e, u64 now) {
    const double elapsed = static_cast<double>(now - e.thrash_at);
    e.thrash_at = now;
    e.thrash = e.thrash * std::exp2(-elapsed / 10000.0) +
               static_cast<double>(e.quick_pending.exchange(0));
}

// While granules come back soon after they go out, the victims must have been idle for longer:
// up to four times the base.
u32 MinIdle(Engine& e, Urgency urgency) {
    const double scale = 1.0 + (std::min)(e.thrash / 4.0, 3.0);
    u32 idle = static_cast<u32>(e.config.min_idle_ms * scale);
    if (urgency == Urgency::Urgent) idle /= 4;
    if (urgency == Urgency::Emergency) idle = 0;
    e.min_idle_ms.store(idle, std::memory_order_relaxed);
    return idle;
}

// Decommit mutex held.
u64 StartProbation(Engine& e, Granule& g, u8* begin, u64 now) {
    u8 state = g.state.load(std::memory_order_acquire);
    const bool partial = state == Swapped;
    if (partial) {
        // Owned while its pages turn: a read back would commit behind the scan.
        if (!g.state.compare_exchange_strong(state, Busy)) return 0;
    } else if (state != Resident) {
        return 0;
    }
    const auto back = [&] {
        if (partial) Settle(g, Swapped);
        return u64{0};
    };
    // The probe goes with the other pages.
    g.probe.store(NO_PROBE, std::memory_order_relaxed);
    const Mask committed = CommittedPages(begin);
    if (committed.none()) {
        g.empty.store(true, std::memory_order_relaxed);
        return back();
    }
    // The pages lose their access before the state says so: a fault in between sees a resident
    // granule, commits nothing new and retries until the probation shows.
    if (!ProtectRuns(e, begin, committed, PAGE_NOACCESS)) {
        ProtectRuns(e, begin, committed, PAGE_READWRITE);
        return back();
    }
    g.since = now;
    g.probation_bytes = committed.count() * PAGE_SIZE;
    e.probation_bytes.fetch_add(g.probation_bytes, std::memory_order_relaxed);
    e.probation_starts.fetch_add(1, std::memory_order_relaxed);
    Settle(g, Probation);
    return g.probation_bytes;
}

Engine* Find(const u8* address, size_t& index) {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr || address < e->base || address >= e->base + e->size) {
        return nullptr;
    }
    index = static_cast<size_t>(address - e->base) / GRANULE_SIZE;
    return e;
}

void Destroy(Engine* e) {
    e->stop.store(true);
    {
        std::scoped_lock lock{e->queue_mutex};
    }
    e->queue_ready.notify_all();
    {
        std::scoped_lock lock{e->prefetch_mutex};
    }
    e->prefetch_ready.notify_all();
    for (std::thread& writer : e->writers) {
        if (writer.joinable()) writer.join();
    }
    if (e->prefetcher.joinable()) e->prefetcher.join();
    for (size_t index = 0; e->table && index < e->granules; ++index) {
        while (e->table[index].in_flight.load() != 0) {
            Sleep(1);
        }
    }
    if (void* const reserve = e->reserve.exchange(nullptr)) {
        VirtualFree(reserve, 0, MEM_RELEASE);
    }
    if (e->file != INVALID_HANDLE_VALUE) CloseHandle(e->file);
    for (size_t i = 0; i < READ_BUFFERS; ++i) {
        if (e->read_buffers[i] != nullptr) VirtualFree(e->read_buffers[i], 0, MEM_RELEASE);
        if (e->read_events[i] != nullptr) CloseHandle(e->read_events[i]);
    }
    for (size_t i = 0; i < MAX_WRITERS; ++i) {
        if (e->staging[i] != nullptr) VirtualFree(e->staging[i], 0, MEM_RELEASE);
        if (e->write_events[i] != nullptr) CloseHandle(e->write_events[i]);
    }
    delete e;
}

HANDLE NewEvent() {
    return CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
}

} // namespace

bool Init(u8* base, size_t size, const Platform& platform, const Config& config) {
    if (g_engine.load() != nullptr || base == nullptr || size < GRANULE_SIZE ||
        reinterpret_cast<uintptr_t>(base) % (64 * 1024) != 0 || !platform.commit ||
        !platform.protect || !platform.allocate) {
        return false;
    }
    auto* const e = new Engine();
    e->base = base;
    e->granules = size / GRANULE_SIZE;
    e->size = e->granules * GRANULE_SIZE;
    e->platform = platform;
    e->config = config;
    e->writer_count = std::clamp<size_t>(config.writers, 1, MAX_WRITERS);
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    // Unbuffered: what goes to disk must not come back as file cache.
    params.dwFileFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE;
    constexpr DWORD SHARE = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    e->file = CreateFile2(config.path.c_str(), GENERIC_READ | GENERIC_WRITE, SHARE, CREATE_ALWAYS,
                          &params);
    bool ok = e->file != INVALID_HANDLE_VALUE;
    for (size_t i = 0; ok && i < READ_BUFFERS; ++i) {
        e->read_buffers[i] = static_cast<u8*>(platform.allocate(GRANULE_SIZE));
        e->read_events[i] = NewEvent();
        ok = e->read_buffers[i] != nullptr && e->read_events[i] != nullptr;
    }
    for (size_t i = 0; ok && i < e->writer_count; ++i) {
        e->staging[i] = static_cast<u8*>(platform.allocate(GRANULE_SIZE));
        e->write_events[i] = NewEvent();
        ok = e->staging[i] != nullptr && e->write_events[i] != nullptr;
    }
    if (!ok) {
        const DWORD error = GetLastError();
        Destroy(e);
        SetLastError(error);
        return false;
    }
    e->table = std::make_unique<Granule[]>(e->granules);
    e->queue = std::make_unique<u32[]>(e->granules);
    e->candidates.reserve(e->granules);
    e->slot_words = (e->granules + 63) / 64;
    e->slots = std::make_unique<std::atomic<u64>[]>(e->slot_words);
    for (size_t word = 0; word < e->slot_words; ++word) {
        const size_t used = (std::min<size_t>)(64, e->granules - word * 64);
        e->slots[word].store(used == 64 ? 0 : ~u64{} << used);
    }
    e->thrash_at = NowMs();
    e->describe_at.store(e->thrash_at);
    e->min_idle_ms.store(config.min_idle_ms);
    for (size_t i = 0; i < e->writer_count; ++i) {
        e->writers[i] = std::thread{WriterLoop, e, i};
    }
    e->prefetcher = std::thread{PrefetchLoop, e};
    g_engine.store(e, std::memory_order_release);
    RefillReserve();
    return true;
}

void Shutdown() {
    Engine* const e = g_engine.exchange(nullptr);
    if (e != nullptr) {
        Destroy(e);
    }
}

bool Active() {
    return g_engine.load(std::memory_order_acquire) != nullptr;
}

void AddStall(Engine& e, std::chrono::steady_clock::time_point start) {
    e.fault_stall_us.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() - start)
                                                    .count()),
                               std::memory_order_relaxed);
}

bool OnFault(u8* address, bool (*commit)(u8* address)) {
    size_t index{};
    Engine* const e = Find(address, index);
    if (e == nullptr) {
        return commit(address);
    }
    Granule& g = e->table[index];
    u8* const begin = e->base + index * GRANULE_SIZE;
    const size_t page = static_cast<size_t>(address - begin) / PAGE_SIZE;
    for (;;) {
        g.in_flight.fetch_add(1);
        u8 state = g.state.load();
        if (state == Resident) {
            bool ok;
            u16 probe = g.probe.load(std::memory_order_relaxed);
            if (probe == page && g.probe.compare_exchange_strong(probe, NO_PROBE)) {
                // The scout's probe: the granule is in use. A clear may have taken the page.
                g.last_access.store(NowMs(), std::memory_order_relaxed);
                ok = e->platform.protect(address - static_cast<size_t>(address - begin) % PAGE_SIZE,
                                         PAGE_SIZE, PAGE_READWRITE) ||
                     commit(address);
                e->probe_hits.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (g.empty.load(std::memory_order_relaxed)) {
                    g.empty.store(false, std::memory_order_relaxed);
                }
                ok = commit(address);
            }
            g.in_flight.fetch_sub(1);
            return ok;
        }
        if (state == Busy) {
            g.in_flight.fetch_sub(1);
            const auto start = std::chrono::steady_clock::now();
            Wait(g.state);
            AddStall(*e, start);
            return true;
        }
        if (g.state.compare_exchange_strong(state, Busy)) {
            const auto start = std::chrono::steady_clock::now();
            const u64 now = NowMs();
            const bool ok = state == Probation ? Restore(*e, g, begin, now)
                                               : SwapIn(*e, g, index, begin, page, now);
            g.in_flight.fetch_sub(1);
            AddStall(*e, start);
            return ok;
        }
        g.in_flight.fetch_sub(1);
    }
}

void ForgetPages(u8* begin, u8* end) {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr) {
        return;
    }
    begin = (std::max)(begin, e->base);
    end = (std::min)(end, e->base + e->size);
    if (begin >= end) {
        return;
    }
    const size_t first_granule = static_cast<size_t>(begin - e->base) / GRANULE_SIZE;
    const size_t last_granule = static_cast<size_t>(end - 1 - e->base) / GRANULE_SIZE;
    for (size_t index = first_granule; index <= last_granule; ++index) {
        Granule& g = e->table[index];
        u8* const granule = e->base + index * GRANULE_SIZE;
        const size_t first = static_cast<size_t>((std::max)(begin, granule) - granule) / PAGE_SIZE;
        const size_t last =
            static_cast<size_t>((std::min)(end, granule + GRANULE_SIZE) - granule) / PAGE_SIZE;
        // An armed probe on a page that goes is no probe any more.
        if (u16 probe = g.probe.load(); probe != NO_PROBE && probe >= first && probe < last) {
            g.probe.compare_exchange_strong(probe, NO_PROBE);
        }
        for (;;) {
            u8 state = g.state.load();
            if (state == Busy) {
                Wait(g.state);
                continue;
            }
            if (state != Swapped && !(state == Probation && g.has_data.load())) {
                break;
            }
            if (!g.state.compare_exchange_strong(state, Busy)) {
                continue;
            }
            u64 dropped = 0;
            for (size_t i = first; i < last; ++i) {
                dropped += g.data[i];
                g.data.reset(i);
            }
            e->swapped_bytes.fetch_sub(dropped * PAGE_SIZE, std::memory_order_relaxed);
            if (g.data.none() && g.slot != NO_SLOT) {
                FreeSlot(*e, g.slot);
                g.slot = NO_SLOT;
            }
            if (state == Probation) {
                Settle(g, Probation);
            } else {
                SettleOut(g);
            }
            break;
        }
    }
}

bool IsResident(const u8* address) {
    size_t index{};
    Engine* const e = Find(address, index);
    return e == nullptr || e->table[index].state.load(std::memory_order_acquire) == Resident;
}

const u8* ClipUntouched(const u8* begin, const u8* end) {
    size_t index{};
    Engine* const e = Find(begin, index);
    if (e == nullptr) {
        return end;
    }
    for (; index < e->granules; ++index) {
        const u8* const granule = e->base + index * GRANULE_SIZE;
        if (granule >= end) {
            break;
        }
        const Granule& g = e->table[index];
        const u8 state = g.state.load(std::memory_order_acquire);
        if (state == Swapped || state == Busy ||
            (state == Probation && g.has_data.load(std::memory_order_relaxed))) {
            return (std::max)(begin, granule);
        }
    }
    return end;
}

namespace {

// Owns the granule, whatever its state. Under the decommit mutex this waits on writers and reads
// back only, and those never take it.
u8 Hold(Granule& g) {
    for (;;) {
        u8 state = g.state.load(std::memory_order_acquire);
        if (state == Busy) {
            Wait(g.state);
        } else if (g.state.compare_exchange_strong(state, Busy)) {
            return state;
        }
    }
}

// The pages [first, last) of the granule that are committed, and those of them with no access.
void LookAtPages(const u8* granule, size_t first, size_t last, Mask& committed, Mask& no_access) {
    for (size_t page = first; page < last;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(granule + page * PAGE_SIZE, &info, sizeof(info)) == 0) {
            break;
        }
        const size_t end = (std::min)(
            last, static_cast<size_t>(static_cast<const u8*>(info.BaseAddress) + info.RegionSize - granule) /
                      PAGE_SIZE);
        if (info.State == MEM_COMMIT) {
            for (size_t i = page; i < end; ++i) committed.set(i);
            if (info.Protect == PAGE_NOACCESS) {
                for (size_t i = page; i < end; ++i) no_access.set(i);
            }
        }
        page = (std::max)(end, page + 1);
    }
}

// Pages [first, last) of a granule; `shown` is how many pages of the range came before them.
void InspectGranuleCommitted(Engine& e, size_t index, size_t first, size_t last, size_t shown,
                             PageVisitor visit, void* context) {
    Granule& g = e.table[index];
    u8* const granule = e.base + index * GRANULE_SIZE;
    const u8 state = Hold(g);
    Mask committed;
    Mask no_access;
    LookAtPages(granule, first, last, committed, no_access);
    Mask opened;
    ForEachRun(no_access, [&](size_t run, size_t count) {
        if (e.platform.protect(granule + run * PAGE_SIZE, count * PAGE_SIZE, PAGE_READONLY)) {
            for (size_t i = run; i < run + count; ++i) opened.set(i);
        }
    });
    ForEachRun(committed & (~no_access | opened), [&](size_t run, size_t count) {
        visit(context, shown + run - first, count, granule + run * PAGE_SIZE);
    });
    if (state == Probation) {
        ProtectRuns(e, granule, opened, PAGE_NOACCESS);
    } else {
        // Only the probe stays closed. If its fault came meanwhile the probe is gone, and if it comes
        // between this check and the protect the commit in OnFault gives the access back.
        const u16 probe = g.probe.load();
        if (probe != NO_PROBE && opened[probe]) {
            opened.reset(probe);
            e.platform.protect(granule + size_t{probe} * PAGE_SIZE, PAGE_SIZE, PAGE_NOACCESS);
        }
        ProtectRuns(e, granule, opened, PAGE_READWRITE);
    }
    Settle(g, static_cast<State>(state));
}

bool InspectGranuleSwapped(Engine& e, size_t index, size_t first, size_t last, size_t shown,
                           PageVisitor visit, void* context) {
    Granule& g = e.table[index];
    if (!g.has_data.load(std::memory_order_relaxed)) {
        return true;
    }
    const u8 state = Hold(g);
    Mask want;
    for (size_t page = first; page < last; ++page) {
        if (g.data[page]) want.set(page);
    }
    if (want.none()) {
        Settle(g, static_cast<State>(state));
        return true;
    }
    const auto [span_first, span_last] = Span(want);
    const size_t buffer_index = AcquireReadBuffer(e);
    u8* const buffer = e.read_buffers[buffer_index];
    const bool ok = Transfer(e.file, e.read_events[buffer_index], false, buffer + span_first * PAGE_SIZE,
                             u64{g.slot} * GRANULE_SIZE + span_first * PAGE_SIZE,
                             (span_last - span_first) * PAGE_SIZE);
    // The buffer is its own from here: a fault in the granule need not wait for the visitor.
    Settle(g, static_cast<State>(state));
    if (ok) {
        ForEachRun(want, [&](size_t run, size_t count) {
            visit(context, shown + run - first, count, buffer + run * PAGE_SIZE);
        });
    }
    ReleaseReadBuffer(e, buffer_index);
    return ok;
}

// Calls `f(index, first, last, shown)` for the part of the range in each granule of the backing.
template <typename F>
void ForEachGranulePart(const Engine& e, const u8* begin, size_t pages, F&& f) {
    for (size_t shown = 0; shown < pages;) {
        const size_t offset = static_cast<size_t>(begin - e.base) + shown * PAGE_SIZE;
        const size_t first = offset % GRANULE_SIZE / PAGE_SIZE;
        const size_t last = (std::min)(GRANULE_PAGES, first + pages - shown);
        if (!f(offset / GRANULE_SIZE, first, last, shown)) {
            return;
        }
        shown += last - first;
    }
}

} // namespace

void InspectCommitted(const u8* begin, size_t pages, PageVisitor visit, void* context) {
    size_t index{};
    Engine* const e = Find(begin, index);
    if (e == nullptr) {
        // Nothing here can turn no-access: only what is committed is left to tell.
        for (size_t page = 0; page < pages;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(begin + page * PAGE_SIZE, &info, sizeof(info)) == 0) {
                return;
            }
            const size_t end = (std::min)(
                pages, static_cast<size_t>(static_cast<const u8*>(info.BaseAddress) + info.RegionSize - begin) /
                           PAGE_SIZE);
            if (info.State == MEM_COMMIT && info.Protect != PAGE_NOACCESS && end > page) {
                visit(context, page, end - page, begin + page * PAGE_SIZE);
            }
            page = (std::max)(end, page + 1);
        }
        return;
    }
    ForEachGranulePart(*e, begin, pages, [&](size_t granule, size_t first, size_t last, size_t shown) {
        InspectGranuleCommitted(*e, granule, first, last, shown, visit, context);
        return true;
    });
}

bool InspectSwapped(const u8* begin, size_t pages, PageVisitor visit, void* context) {
    size_t index{};
    Engine* const e = Find(begin, index);
    if (e == nullptr) {
        return true;
    }
    bool ok = true;
    ForEachGranulePart(*e, begin, pages, [&](size_t granule, size_t first, size_t last, size_t shown) {
        ok = InspectGranuleSwapped(*e, granule, first, last, shown, visit, context);
        return ok;
    });
    return ok;
}

StepResult Step(u64 want, Urgency urgency, std::mutex& decommit_mutex) {
    StepResult result;
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr) {
        return result;
    }
    std::scoped_lock step{e->step_mutex};
    const u64 now = NowMs();
    result.released = e->released_pending.exchange(0, std::memory_order_relaxed);
    UpdateThrash(*e, now);
    if (now >= e->next_scout) {
        Scout(*e, now, decommit_mutex);
    }
    if (urgency == Urgency::Watch || want == 0) {
        return result;
    }
    const u32 probation = urgency == Urgency::Normal   ? e->config.probation_ms
                          : urgency == Urgency::Urgent ? e->config.probation_ms / 3
                                                       : e->config.probation_ms / 6;
    // A batch at a time, so the writers and the faults of the granules going out do not all land
    // in the same moment: the first write-out of a session can be hundreds of MiB.
    const u64 batch = e->config.batch_bytes == 0 || urgency == Urgency::Emergency ? ~u64{0}
                      : urgency == Urgency::Urgent ? e->config.batch_bytes * 2
                                                   : e->config.batch_bytes;
    if (now - e->batch_at >= BATCH_MS) {
        e->batch_at = now;
        e->batch_used = 0;
    }
    // The probations that ran out go to the writers, as many as the shortfall needs: what is
    // being written counts as given back already.
    const u64 write_limit = (std::min)(want, batch);
    for (size_t index = 0; index < e->granules; ++index) {
        if (e->writing_bytes.load(std::memory_order_relaxed) >= write_limit) {
            break;
        }
        Granule& g = e->table[index];
        u8 state = g.state.load(std::memory_order_acquire);
        if (state != Probation || now - g.since < probation ||
            !g.state.compare_exchange_strong(state, Busy)) {
            continue;
        }
        e->probation_bytes.fetch_sub(g.probation_bytes, std::memory_order_relaxed);
        e->writing_bytes.fetch_add(g.probation_bytes, std::memory_order_relaxed);
        QueueWrite(*e, index);
        ++result.queued;
    }
    // Half again the shortfall goes on probation, as part of it is touched again. The coldest
    // first: the longest since the scout or a fault last saw them in use.
    const u64 target = want + want / 2;
    u64 pending = e->probation_bytes.load(std::memory_order_relaxed) +
                  e->writing_bytes.load(std::memory_order_relaxed);
    const u32 min_idle = MinIdle(*e, urgency);
    const bool ignore_shields = urgency == Urgency::Emergency;
    u32 shielded = 0;
    e->candidates.clear();
    for (size_t index = 0; index < e->granules; ++index) {
        const Granule& g = e->table[index];
        const u8 state = g.state.load(std::memory_order_acquire);
        // A granule with nothing committed has nothing to give: one wholly on disk among them.
        if ((state != Resident && state != Swapped) || g.empty.load(std::memory_order_relaxed)) {
            continue;
        }
        if (g.shield_until.load(std::memory_order_relaxed) > now) {
            ++shielded;
            if (!ignore_shields) continue;
        }
        const u64 seen = (std::max)(g.last_access.load(std::memory_order_relaxed), e->watch_epoch);
        e->candidates.emplace_back(now > seen ? now - seen : 0, static_cast<u32>(index));
    }
    e->shielded.store(shielded, std::memory_order_relaxed);
    if (pending >= target) {
        return result;
    }
    std::sort(e->candidates.begin(), e->candidates.end(), std::greater<>{});
    for (const auto& [idle, index] : e->candidates) {
        if (pending >= target || e->batch_used >= batch) {
            break;
        }
        // Far short and with nothing on the way, the coldest go even when not idle long enough.
        if (idle < min_idle && (urgency == Urgency::Normal ||
                                (urgency == Urgency::Urgent && (pending != 0 || result.probation != 0)))) {
            break;
        }
        std::scoped_lock lock{decommit_mutex};
        if (const u64 bytes = StartProbation(*e, e->table[index],
                                             e->base + size_t{index} * GRANULE_SIZE, now)) {
            pending += bytes;
            e->batch_used += bytes;
            ++result.probation;
        }
    }
    return result;
}

void Settle() {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr) {
        return;
    }
    while (e->writer_jobs.load() != 0 || e->prefetch_jobs.load() != 0) {
        Sleep(1);
    }
}

bool ReleaseReserve() {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr) {
        return false;
    }
    void* const reserve = e->reserve.exchange(nullptr);
    if (reserve == nullptr) {
        return false;
    }
    VirtualFree(reserve, 0, MEM_RELEASE);
    e->reserve_released.store(true, std::memory_order_release);
    e->reserve_releases.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RefillReserve() {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr || e->config.reserve_bytes == 0) {
        return false;
    }
    if (e->reserve.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    void* const block = e->platform.allocate(e->config.reserve_bytes);
    if (block == nullptr) {
        return false;
    }
    void* expected = nullptr;
    if (!e->reserve.compare_exchange_strong(expected, block)) {
        VirtualFree(block, 0, MEM_RELEASE);
    }
    e->reserve_released.store(false, std::memory_order_release);
    return true;
}

bool ReserveReleased() {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    return e != nullptr && e->reserve_released.load(std::memory_order_acquire);
}

Stats GetStats() {
    Stats stats;
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr) {
        return stats;
    }
    stats.swapped_bytes = e->swapped_bytes.load(std::memory_order_relaxed);
    stats.probation_bytes = e->probation_bytes.load(std::memory_order_relaxed);
    stats.writing_bytes = e->writing_bytes.load(std::memory_order_relaxed);
    stats.written_bytes = e->written_bytes.load(std::memory_order_relaxed);
    stats.zero_bytes = e->zero_bytes.load(std::memory_order_relaxed);
    stats.read_bytes = e->read_bytes.load(std::memory_order_relaxed);
    stats.swap_ins = e->swap_ins.load(std::memory_order_relaxed);
    stats.window_reads = e->window_reads.load(std::memory_order_relaxed);
    stats.prefetch_reads = e->prefetch_reads.load(std::memory_order_relaxed);
    stats.swap_in_us = e->swap_in_us.load(std::memory_order_relaxed);
    stats.swap_in_max_us = e->swap_in_max_us.load(std::memory_order_relaxed);
    stats.zero_faults = e->zero_faults.load(std::memory_order_relaxed);
    stats.probation_starts = e->probation_starts.load(std::memory_order_relaxed);
    stats.probation_touches = e->probation_touches.load(std::memory_order_relaxed);
    stats.evictions = e->evictions.load(std::memory_order_relaxed);
    stats.refaults = e->refaults.load(std::memory_order_relaxed);
    stats.quick_refaults = e->quick_refaults.load(std::memory_order_relaxed);
    stats.probe_arms = e->probe_arms.load(std::memory_order_relaxed);
    stats.probe_hits = e->probe_hits.load(std::memory_order_relaxed);
    stats.write_failures = e->write_failures.load(std::memory_order_relaxed);
    stats.reserve_releases = e->reserve_releases.load(std::memory_order_relaxed);
    stats.file_slots = e->file_slots.load(std::memory_order_relaxed);
    stats.min_idle_ms = e->min_idle_ms.load(std::memory_order_relaxed);
    stats.shielded = e->shielded.load(std::memory_order_relaxed);
    stats.fault_stall_us = e->fault_stall_us.load(std::memory_order_relaxed);
    return stats;
}

std::string Describe(bool since_last) {
    if (!Active()) {
        return "permafrost off";
    }
    const Stats s = GetStats();
    const auto n = [](u64 value) { return static_cast<unsigned long long>(value); };
    // The stall since the last such call, per second of it: what the guest paid lately.
    u64 stall = 0;
    u64 span_ms = 0;
    if (since_last) {
        Engine* const e = g_engine.load(std::memory_order_acquire);
        const u64 now = NowMs();
        const u64 since = e->describe_at.exchange(now, std::memory_order_relaxed);
        stall = s.fault_stall_us -
                e->describe_stall_us.exchange(s.fault_stall_us, std::memory_order_relaxed);
        span_ms = now > since ? now - since : 0;
    }
    // The first part keeps its old shape for the log tools.
    char text[896];
    std::snprintf(
        text, sizeof(text),
        "permafrost: %llu MiB on disk (file %llu MiB), %llu MiB on probation, written %llu MiB, "
        "zero %llu MiB, read back %llu MiB in %llu reads (avg %llu us, max %llu us), touched on probation "
        "%llu of %llu, %llu write failures, reserve %s (%llu releases); %llu MiB writing, "
        "%llu evictions, refaults %llu quick of %llu, %u shielded, idle %u ms, windows %llu, "
        "prefetch %llu, zero faults %llu, probes %llu hit of %llu, stall %llu ms in all",
        n(s.swapped_bytes >> 20), n((s.file_slots * GRANULE_SIZE) >> 20), n(s.probation_bytes >> 20),
        n(s.written_bytes >> 20), n(s.zero_bytes >> 20), n(s.read_bytes >> 20), n(s.swap_ins),
        n(s.swap_ins > s.prefetch_reads ? s.swap_in_us / (s.swap_ins - s.prefetch_reads) : 0),
        n(s.swap_in_max_us), n(s.probation_touches), n(s.probation_starts), n(s.write_failures),
        ReserveReleased() ? "given up" : "held", n(s.reserve_releases), n(s.writing_bytes >> 20),
        n(s.evictions), n(s.quick_refaults), n(s.refaults), s.shielded, s.min_idle_ms,
        n(s.window_reads), n(s.prefetch_reads), n(s.zero_faults), n(s.probe_hits), n(s.probe_arms),
        n(s.fault_stall_us / 1000));
    if (!since_last || span_ms == 0) {
        return text;
    }
    char rate[96];
    std::snprintf(rate, sizeof(rate), " (%llu.%llu ms/s over the last %llu.%llu s)",
                  n(stall / span_ms), n(stall * 10 / span_ms % 10), n(span_ms / 1000),
                  n(span_ms / 100 % 10));
    return text + std::string{rate};
}

std::string DescribePage(const u8* address) {
    size_t index{};
    Engine* const e = Find(address, index);
    if (e == nullptr) {
        return {};
    }
    const Granule& g = e->table[index];
    const size_t page = static_cast<size_t>(address - e->base) % GRANULE_SIZE / PAGE_SIZE;
    const auto now = std::chrono::steady_clock::now();
    const auto ms_ago = [&](std::chrono::steady_clock::time_point at) {
        return std::chrono::duration<double, std::milli>(now - at).count();
    };
    static constexpr const char* STATES[] = {"resident", "probation", "busy", "swapped"};
    const u8 state = g.state.load(std::memory_order_acquire);
    std::string out = "granule " + std::to_string(index) + " page " + std::to_string(page) + " " +
                      (state < std::size(STATES) ? STATES[state] : "?") + ", " +
                      std::to_string(g.data.count()) + " pages on disk, touched " +
                      std::to_string(NowMs() - g.last_access.load(std::memory_order_relaxed)) +
                      " ms ago";
    const u64 serial = g_read_serial.load(std::memory_order_relaxed);
    u32 shown = 0;
    for (u64 i = 0; i < (std::min<u64>)(serial, READ_RECORDS) && shown < 4; ++i) {
        const ReadRecord r = g_reads[(serial - 1 - i) % READ_RECORDS];
        if (r.granule != index || r.end.time_since_epoch().count() == 0) {
            continue;
        }
        ++shown;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "; read back [%u, %u)%s by %s on thread %u %.3f ms ago in %.0f us%s",
                      unsigned{r.first}, unsigned{r.last},
                      page >= r.first && page < r.last ? " (this page's span)" : "",
                      r.prefetch ? "prefetch" : "fault", r.thread, ms_ago(r.end),
                      std::chrono::duration<double, std::micro>(r.end - r.start).count(),
                      r.ok ? "" : " FAILED");
        out += line;
    }
    if (shown == 0) {
        out += "; no read back in the last " + std::to_string((std::min<u64>)(serial, READ_RECORDS));
    }
    return out;
}

void RegisterGuestThread() {
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
        return;
    }
    std::scoped_lock lock{g_pause_mutex};
    for (GuestThread& thread : g_guest_threads) {
        if (thread.id.load(std::memory_order_relaxed) == 0) {
            thread.handle = handle;
            thread.id.store(GetCurrentThreadId(), std::memory_order_release);
            return;
        }
    }
    CloseHandle(handle);
}

void UnregisterGuestThread() {
    const DWORD self = GetCurrentThreadId();
    std::scoped_lock lock{g_pause_mutex};
    for (GuestThread& thread : g_guest_threads) {
        if (thread.id.load(std::memory_order_relaxed) == self) {
            thread.id.store(0, std::memory_order_release);
            CloseHandle(thread.handle);
            thread.handle = nullptr;
            return;
        }
    }
}

std::string DescribeRecentReads(size_t count) {
    const auto now = std::chrono::steady_clock::now();
    const u64 serial = g_read_serial.load(std::memory_order_relaxed);
    std::string out = std::to_string(serial) + " reads back so far";
    for (u64 i = 0; i < (std::min<u64>)({serial, READ_RECORDS, count}); ++i) {
        const ReadRecord r = g_reads[(serial - 1 - i) % READ_RECORDS];
        char line[160];
        std::snprintf(line, sizeof(line), "; granule %u [%u, %u) by %s on thread %u %.3f ms ago in %.0f us%s",
                      r.granule, unsigned{r.first}, unsigned{r.last},
                      r.prefetch ? "prefetch" : "fault", r.thread,
                      std::chrono::duration<double, std::milli>(now - r.end).count(),
                      std::chrono::duration<double, std::micro>(r.end - r.start).count(),
                      r.ok ? "" : " FAILED");
        out += line;
    }
    return out;
}

} // namespace Common::Permafrost

#else

namespace Common::Permafrost {
bool Init(u8*, size_t, const Platform&, const Config&) {
    return false;
}
void Shutdown() {}
bool Active() {
    return false;
}
bool OnFault(u8* address, bool (*commit)(u8* address)) {
    return commit(address);
}
void ForgetPages(u8*, u8*) {}
bool IsResident(const u8*) {
    return true;
}
const u8* ClipUntouched(const u8*, const u8* end) {
    return end;
}
void InspectCommitted(const u8* begin, size_t pages, PageVisitor visit, void* context) {
    visit(context, 0, pages, begin);
}
bool InspectSwapped(const u8*, size_t, PageVisitor, void*) {
    return true;
}
StepResult Step(u64, Urgency, std::mutex&) {
    return {};
}
void Settle() {}
bool ReleaseReserve() {
    return false;
}
bool RefillReserve() {
    return false;
}
bool ReserveReleased() {
    return false;
}
Stats GetStats() {
    return {};
}
std::string Describe(bool) {
    return "permafrost off";
}
std::string DescribePage(const u8*) {
    return {};
}
std::string DescribeRecentReads(size_t) {
    return {};
}
void RegisterGuestThread() {}
void UnregisterGuestThread() {}
} // namespace Common::Permafrost

#endif
