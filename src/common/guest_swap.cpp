// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/guest_swap.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace Common::GuestSwap {
namespace {

// Resident -> Probation and Probation -> Busy -> Swapped belong to the swapper, which holds the
// decommit mutex. Probation -> Busy -> Resident (a touch) and Swapped -> Busy -> Resident (a read
// back) belong to the faulting thread. Busy is owned by whoever moved the granule into it.
enum State : u8 { Resident, Probation, Busy, Swapped };
using Mask = std::bitset<GRANULE_PAGES>;
constexpr u32 NO_SLOT = ~u32{};

struct Granule {
    std::atomic<u8> state{Resident};
    std::atomic<u64> last_touch{};
    u64 since{};           // when the probation began
    u64 probation_bytes{}; // committed bytes when it began
    u32 slot{NO_SLOT};
    Mask data; // pages the slot holds; the slot has zeros between them
};

// Buffers a read back lands in before any page of the granule is committed.
constexpr size_t READ_BUFFERS = 2;
// A read back commits and fills its pages this many at a time.
constexpr size_t FILL_PAGES = 16;

struct Engine {
    u8* base{};
    size_t size{}; // whole granules only
    size_t granules{};
    Platform platform{};
    Config config;
    std::unique_ptr<Granule[]> table;
    std::unique_ptr<std::atomic<u64>[]> slots;
    size_t slot_words{};
    HANDLE writer{INVALID_HANDLE_VALUE};
    HANDLE reader{INVALID_HANDLE_VALUE};
    // The span of a granule going out is gathered here and written at once: a read back is then
    // one read whatever the zero pages in between. Used under the decommit mutex.
    u8* staging{};
    u8* read_buffers[READ_BUFFERS]{};
    std::atomic<u32> read_buffers_busy{};
    // Faulting threads between reading a granule's state and acting on it. A granule leaves
    // probation for the disk only once none is, so no commit lands behind its scan.
    std::atomic<u32> in_flight{};
    std::atomic<void*> reserve{};
    std::atomic<bool> reserve_released{};
    size_t hand{};

    std::atomic<u64> swapped_bytes{};
    std::atomic<u64> probation_bytes{};
    std::atomic<u64> written_bytes{};
    std::atomic<u64> zero_bytes{};
    std::atomic<u64> read_bytes{};
    std::atomic<u64> swap_ins{};
    std::atomic<u64> swap_in_us{};
    std::atomic<u64> swap_in_max_us{};
    std::atomic<u64> probation_starts{};
    std::atomic<u64> probation_touches{};
    std::atomic<u64> write_failures{};
    std::atomic<u64> reserve_releases{};
    std::atomic<u64> file_slots{};
};

std::atomic<Engine*> g_engine{};

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

bool Transfer(HANDLE file, bool write, u8* memory, u64 offset, size_t size) {
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done{};
    const BOOL ok = write ? WriteFile(file, memory, static_cast<DWORD>(size), &done, &at)
                          : ReadFile(file, memory, static_cast<DWORD>(size), &done, &at);
    return ok && done == size;
}

void UpdateMax(std::atomic<u64>& max, u64 value) {
    for (u64 seen = max.load(); seen < value && !max.compare_exchange_weak(seen, value);) {
    }
}

// The granule was touched on probation: its pages get their access back. A clear may decommit
// some of them meanwhile, failing their protect: the access is retried all the same, and a page
// left reserved or without access faults again into the demand commit of a resident granule.
bool Restore(Engine& e, Granule& g, u8* begin) {
    ProtectRuns(e, begin, CommittedPages(begin), PAGE_READWRITE);
    e.probation_bytes.fetch_sub(g.probation_bytes, std::memory_order_relaxed);
    e.probation_touches.fetch_add(1, std::memory_order_relaxed);
    g.last_touch.store(NowMs(), std::memory_order_relaxed);
    g.state.store(Resident, std::memory_order_release);
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

// Pages become visible to every thread the moment they are committed, before anything is copied
// into them: a private page cannot be filled out of sight. So the data is read first and the pages
// are committed and filled a few at a time, which leaves another thread touching the same granule
// a window of microseconds per batch, not the length of the read, to see them still zero.
bool SwapIn(Engine& e, Granule& g, u8* begin) {
    const auto start = std::chrono::steady_clock::now();
    const auto [first, last] = Span(g.data);
    const size_t buffer_index = AcquireReadBuffer(e);
    u8* const buffer = e.read_buffers[buffer_index];
    bool ok = Transfer(e.reader, false, buffer + first * PAGE_SIZE,
                       u64{g.slot} * GRANULE_SIZE + first * PAGE_SIZE, (last - first) * PAGE_SIZE);
    Mask filled;
    ForEachRun(g.data, [&](size_t run, size_t count) {
        for (size_t page = run; ok && page < run + count; page += FILL_PAGES) {
            const size_t size = (std::min)(FILL_PAGES, run + count - page) * PAGE_SIZE;
            u8* const target = begin + page * PAGE_SIZE;
            ok = e.platform.commit(target, size) || (ReleaseReserve() && e.platform.commit(target, size));
            if (ok) {
                std::memcpy(target, buffer + page * PAGE_SIZE, size);
                for (size_t i = page; i < page + size / PAGE_SIZE; ++i) filled.set(i);
            }
        }
    });
    ReleaseReadBuffer(e, buffer_index);
    if (!ok) {
        // The slot still holds everything: what was filled goes again.
        ForEachRun(filled, [&](size_t run, size_t count) {
            VirtualFree(begin + run * PAGE_SIZE, count * PAGE_SIZE, MEM_DECOMMIT);
        });
        g.state.store(Swapped, std::memory_order_release);
        return false;
    }
    const u64 bytes = g.data.count() * PAGE_SIZE;
    FreeSlot(e, g.slot);
    g.slot = NO_SLOT;
    g.data.reset();
    const auto us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
    e.swapped_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    e.read_bytes.fetch_add(bytes, std::memory_order_relaxed);
    e.swap_ins.fetch_add(1, std::memory_order_relaxed);
    e.swap_in_us.fetch_add(us, std::memory_order_relaxed);
    UpdateMax(e.swap_in_max_us, us);
    g.last_touch.store(NowMs(), std::memory_order_relaxed);
    g.state.store(Resident, std::memory_order_release);
    return true;
}

// Decommit mutex held.
u64 StartProbation(Engine& e, Granule& g, u8* begin, u64 now) {
    if (g.state.load(std::memory_order_acquire) != Resident) {
        return 0;
    }
    const Mask committed = CommittedPages(begin);
    if (committed.none()) {
        return 0;
    }
    // The pages lose their access before the state says so: a fault in between sees a resident
    // granule, commits nothing new and retries until the probation shows.
    if (!ProtectRuns(e, begin, committed, PAGE_NOACCESS)) {
        ProtectRuns(e, begin, committed, PAGE_READWRITE);
        return 0;
    }
    g.since = now;
    g.probation_bytes = committed.count() * PAGE_SIZE;
    e.probation_bytes.fetch_add(g.probation_bytes, std::memory_order_relaxed);
    e.probation_starts.fetch_add(1, std::memory_order_relaxed);
    g.state.store(Probation, std::memory_order_release);
    return g.probation_bytes;
}

struct Evicted {
    u64 released{};
    bool swapped{};
};

// Decommit mutex held.
Evicted Evict(Engine& e, Granule& g, u8* begin) {
    u8 expected = Probation;
    if (!g.state.compare_exchange_strong(expected, Busy)) {
        return {};
    }
    for (u32 spin = 0; e.in_flight.load() != 0; ++spin) {
        if (spin < 4096) {
            YieldProcessor();
        } else {
            Sleep(0);
        }
    }
    e.probation_bytes.fetch_sub(g.probation_bytes, std::memory_order_relaxed);
    const auto keep = [&](const Mask& committed) {
        ProtectRuns(e, begin, committed, PAGE_READWRITE);
        g.last_touch.store(NowMs(), std::memory_order_relaxed);
        g.state.store(Resident, std::memory_order_release);
        return Evicted{};
    };
    const Mask committed = CommittedPages(begin);
    if (committed.none()) {
        g.state.store(Resident, std::memory_order_release);
        return {};
    }
    // Read-only from here: readers go on, writers wait in the handler until the granule settles.
    if (!ProtectRuns(e, begin, committed, PAGE_READONLY)) {
        return keep(committed);
    }
    Mask data;
    for (size_t i = 0; i < GRANULE_PAGES; ++i) {
        if (committed[i] && !IsClear(begin + i * PAGE_SIZE)) data.set(i);
    }
    u32 slot = NO_SLOT;
    if (data.any()) {
        slot = AllocSlot(e);
        const auto [first, last] = Span(data);
        for (size_t i = first; i < last; ++i) {
            u8* const page = e.staging + i * PAGE_SIZE;
            if (data[i]) {
                std::memcpy(page, begin + i * PAGE_SIZE, PAGE_SIZE);
            } else {
                std::memset(page, 0, PAGE_SIZE);
            }
        }
        const bool ok = slot != NO_SLOT &&
                        Transfer(e.writer, true, e.staging + first * PAGE_SIZE,
                                 u64{slot} * GRANULE_SIZE + first * PAGE_SIZE, (last - first) * PAGE_SIZE);
        if (!ok) {
            if (slot != NO_SLOT) FreeSlot(e, slot);
            e.write_failures.fetch_add(1, std::memory_order_relaxed);
            return keep(committed);
        }
        e.written_bytes.fetch_add(data.count() * PAGE_SIZE, std::memory_order_relaxed);
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
    data &= released;
    if (data.none() && slot != NO_SLOT) {
        FreeSlot(e, slot);
        slot = NO_SLOT;
    }
    g.data = data;
    g.slot = slot;
    e.swapped_bytes.fetch_add(data.count() * PAGE_SIZE, std::memory_order_relaxed);
    e.zero_bytes.fetch_add((released & ~data).count() * PAGE_SIZE, std::memory_order_relaxed);
    g.state.store(data.any() ? Swapped : Resident, std::memory_order_release);
    return {released.count() * PAGE_SIZE, data.any()};
}

Engine* Find(const u8* address, size_t& index) {
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr || address < e->base || address >= e->base + e->size) {
        return nullptr;
    }
    index = static_cast<size_t>(address - e->base) / GRANULE_SIZE;
    return e;
}

} // namespace

bool Init(u8* base, size_t size, const Platform& platform, const Config& config) {
    if (g_engine.load() != nullptr || base == nullptr || size < GRANULE_SIZE ||
        reinterpret_cast<uintptr_t>(base) % (64 * 1024) != 0 || !platform.commit ||
        !platform.protect || !platform.allocate) {
        return false;
    }
    auto e = std::make_unique<Engine>();
    e->base = base;
    e->granules = size / GRANULE_SIZE;
    e->size = e->granules * GRANULE_SIZE;
    e->platform = platform;
    e->config = config;
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    // Unbuffered: what goes to disk must not come back as file cache.
    params.dwFileFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE;
    constexpr DWORD SHARE = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    e->writer = CreateFile2(config.path.c_str(), GENERIC_READ | GENERIC_WRITE, SHARE, CREATE_ALWAYS,
                            &params);
    if (e->writer == INVALID_HANDLE_VALUE) {
        return false;
    }
    // A second handle so reads back do not queue behind a write on the swapper's.
    params.dwFileFlags = FILE_FLAG_NO_BUFFERING;
    e->reader = CreateFile2(config.path.c_str(), GENERIC_READ, SHARE, OPEN_EXISTING, &params);
    if (e->reader == INVALID_HANDLE_VALUE) {
        CloseHandle(e->writer);
        return false;
    }
    e->staging = static_cast<u8*>(platform.allocate(GRANULE_SIZE));
    if (e->staging == nullptr) {
        CloseHandle(e->reader);
        CloseHandle(e->writer);
        return false;
    }
    for (u8*& buffer : e->read_buffers) {
        buffer = static_cast<u8*>(platform.allocate(GRANULE_SIZE));
    }
    if (std::find(std::begin(e->read_buffers), std::end(e->read_buffers), nullptr) !=
        std::end(e->read_buffers)) {
        for (u8* const buffer : e->read_buffers) {
            if (buffer != nullptr) VirtualFree(buffer, 0, MEM_RELEASE);
        }
        VirtualFree(e->staging, 0, MEM_RELEASE);
        CloseHandle(e->reader);
        CloseHandle(e->writer);
        return false;
    }
    e->table = std::make_unique<Granule[]>(e->granules);
    e->slot_words = (e->granules + 63) / 64;
    e->slots = std::make_unique<std::atomic<u64>[]>(e->slot_words);
    for (size_t word = 0; word < e->slot_words; ++word) {
        const size_t used = (std::min<size_t>)(64, e->granules - word * 64);
        e->slots[word].store(used == 64 ? 0 : ~u64{} << used);
    }
    g_engine.store(e.release(), std::memory_order_release);
    RefillReserve();
    return true;
}

void Shutdown() {
    Engine* const e = g_engine.exchange(nullptr);
    if (e == nullptr) {
        return;
    }
    while (e->in_flight.load() != 0) {
        Sleep(1);
    }
    if (void* const reserve = e->reserve.exchange(nullptr)) {
        VirtualFree(reserve, 0, MEM_RELEASE);
    }
    CloseHandle(e->reader);
    CloseHandle(e->writer);
    VirtualFree(e->staging, 0, MEM_RELEASE);
    for (u8* const buffer : e->read_buffers) {
        VirtualFree(buffer, 0, MEM_RELEASE);
    }
    delete e;
}

bool Active() {
    return g_engine.load(std::memory_order_acquire) != nullptr;
}

bool OnFault(u8* address, bool (*commit)(u8* address)) {
    size_t index{};
    Engine* const e = Find(address, index);
    if (e == nullptr) {
        return commit(address);
    }
    Granule& g = e->table[index];
    u8* const begin = e->base + index * GRANULE_SIZE;
    for (;;) {
        e->in_flight.fetch_add(1);
        u8 state = g.state.load();
        if (state == Resident) {
            const bool ok = commit(address);
            e->in_flight.fetch_sub(1);
            return ok;
        }
        if (state == Busy) {
            e->in_flight.fetch_sub(1);
            Wait(g.state);
            return true;
        }
        if (g.state.compare_exchange_strong(state, Busy)) {
            const bool ok = state == Probation ? Restore(*e, g, begin) : SwapIn(*e, g, begin);
            e->in_flight.fetch_sub(1);
            return ok;
        }
        e->in_flight.fetch_sub(1);
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
        for (;;) {
            u8 state = g.state.load();
            if (state == Busy) {
                Wait(g.state);
                continue;
            }
            if (state != Swapped) {
                break;
            }
            if (!g.state.compare_exchange_strong(state, Busy)) {
                continue;
            }
            const size_t first = static_cast<size_t>((std::max)(begin, granule) - granule) / PAGE_SIZE;
            const size_t last = static_cast<size_t>((std::min)(end, granule + GRANULE_SIZE) - granule) /
                                PAGE_SIZE;
            u64 dropped = 0;
            for (size_t i = first; i < last; ++i) {
                dropped += g.data[i];
                g.data.reset(i);
            }
            e->swapped_bytes.fetch_sub(dropped * PAGE_SIZE, std::memory_order_relaxed);
            if (g.data.none()) {
                FreeSlot(*e, g.slot);
                g.slot = NO_SLOT;
                g.state.store(Resident, std::memory_order_release);
            } else {
                g.state.store(Swapped, std::memory_order_release);
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
        const u8 state = e->table[index].state.load(std::memory_order_acquire);
        if (state == Swapped || state == Busy) {
            return (std::max)(begin, granule);
        }
    }
    return end;
}

StepResult Step(u64 want, bool urgent, std::mutex& decommit_mutex) {
    StepResult result;
    Engine* const e = g_engine.load(std::memory_order_acquire);
    if (e == nullptr || want == 0) {
        return result;
    }
    const u64 now = NowMs();
    const u64 age = urgent ? e->config.probation_ms / 4 : e->config.probation_ms;
    for (size_t index = 0; index < e->granules && result.released < want; ++index) {
        Granule& g = e->table[index];
        if (g.state.load(std::memory_order_acquire) != Probation || now - g.since < age) {
            continue;
        }
        std::scoped_lock lock{decommit_mutex};
        const Evicted evicted = Evict(*e, g, e->base + index * GRANULE_SIZE);
        result.released += evicted.released;
        if (evicted.released != 0) {
            ++(evicted.swapped ? result.swapped : result.zero);
        }
    }
    if (result.released >= want) {
        return result;
    }
    // Twice the shortfall goes on probation, as part of it is touched again.
    const u64 target = 2 * (want - result.released);
    u64 pending = e->probation_bytes.load(std::memory_order_relaxed);
    for (size_t visited = 0; visited < e->granules && pending < target; ++visited) {
        const size_t index = e->hand;
        e->hand = (e->hand + 1) % e->granules;
        Granule& g = e->table[index];
        if (g.state.load(std::memory_order_acquire) != Resident) {
            continue;
        }
        // Second chance for a granule touched lately.
        const u64 touched = g.last_touch.load(std::memory_order_relaxed);
        if (touched != 0 && now - touched < 4 * age) {
            continue;
        }
        std::scoped_lock lock{decommit_mutex};
        if (const u64 bytes = StartProbation(*e, g, e->base + index * GRANULE_SIZE, now)) {
            pending += bytes;
            ++result.probation;
        }
    }
    return result;
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
    stats.written_bytes = e->written_bytes.load(std::memory_order_relaxed);
    stats.zero_bytes = e->zero_bytes.load(std::memory_order_relaxed);
    stats.read_bytes = e->read_bytes.load(std::memory_order_relaxed);
    stats.swap_ins = e->swap_ins.load(std::memory_order_relaxed);
    stats.swap_in_us = e->swap_in_us.load(std::memory_order_relaxed);
    stats.swap_in_max_us = e->swap_in_max_us.load(std::memory_order_relaxed);
    stats.probation_starts = e->probation_starts.load(std::memory_order_relaxed);
    stats.probation_touches = e->probation_touches.load(std::memory_order_relaxed);
    stats.write_failures = e->write_failures.load(std::memory_order_relaxed);
    stats.reserve_releases = e->reserve_releases.load(std::memory_order_relaxed);
    stats.file_slots = e->file_slots.load(std::memory_order_relaxed);
    return stats;
}

std::string Describe() {
    if (!Active()) {
        return "guest swap off";
    }
    const Stats s = GetStats();
    char text[384];
    std::snprintf(
        text, sizeof(text),
        "guest swap: %llu MiB on disk (file %llu MiB), %llu MiB on probation, written %llu MiB, "
        "zero %llu MiB, read back %llu MiB in %llu reads (avg %llu us, max %llu us), touched on probation "
        "%llu of %llu, %llu write failures, reserve %s (%llu releases)",
        static_cast<unsigned long long>(s.swapped_bytes >> 20),
        static_cast<unsigned long long>((s.file_slots * GRANULE_SIZE) >> 20),
        static_cast<unsigned long long>(s.probation_bytes >> 20),
        static_cast<unsigned long long>(s.written_bytes >> 20),
        static_cast<unsigned long long>(s.zero_bytes >> 20),
        static_cast<unsigned long long>(s.read_bytes >> 20),
        static_cast<unsigned long long>(s.swap_ins),
        static_cast<unsigned long long>(s.swap_ins ? s.swap_in_us / s.swap_ins : 0),
        static_cast<unsigned long long>(s.swap_in_max_us),
        static_cast<unsigned long long>(s.probation_touches),
        static_cast<unsigned long long>(s.probation_starts),
        static_cast<unsigned long long>(s.write_failures),
        ReserveReleased() ? "given up" : "held",
        static_cast<unsigned long long>(s.reserve_releases));
    return text;
}

} // namespace Common::GuestSwap

#else

namespace Common::GuestSwap {
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
StepResult Step(u64, bool, std::mutex&) {
    return {};
}
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
std::string Describe() {
    return "guest swap off";
}
} // namespace Common::GuestSwap

#endif
