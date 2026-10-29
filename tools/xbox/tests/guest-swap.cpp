// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Guest swap on a real demand-committed reservation: data survives probation, write-out, read
// back, clears and concurrent writers, and the commit really goes down.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include "common/guest_swap.cpp"

namespace {
using namespace Common::GuestSwap;
constexpr size_t MiB = 1024 * 1024;
constexpr size_t BACKING = 256 * MiB;
constexpr size_t PAGES = BACKING / PAGE_SIZE;
constexpr size_t CHUNK = 64 * 1024;
u8* g_base{};
std::mutex g_decommit;

// The backing's demand commit: the 64 KiB chunk, or the page alone in a partly committed chunk.
bool CommitAround(u8* address) {
    const size_t offset = static_cast<size_t>(address - g_base);
    u8* chunk = g_base + (offset & ~(CHUNK - 1));
    size_t size = CHUNK;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(chunk, &info, sizeof(info)) != 0 &&
        !(info.State == MEM_RESERVE && static_cast<u8*>(info.BaseAddress) + info.RegionSize >= chunk + size)) {
        chunk = g_base + (offset & ~(PAGE_SIZE - 1));
        size = PAGE_SIZE;
    }
    return VirtualAlloc(chunk, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

LONG NTAPI Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    auto* const address = reinterpret_cast<u8*>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (address < g_base || address >= g_base + BACKING) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return OnFault(address, CommitAround) ? EXCEPTION_CONTINUE_EXECUTION
                                          : EXCEPTION_CONTINUE_SEARCH;
}

const Platform PLATFORM{
    [](void* address, size_t size) {
        return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    },
    [](void* address, size_t size, u32 protection) {
        DWORD old{};
        return VirtualProtect(address, size, protection, &old) != FALSE;
    },
    [](size_t size) { return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); },
};

// The kernel clearing pages: what is on disk for them goes, then they are decommitted.
void Clear(size_t first_page, size_t pages) {
    u8* const begin = g_base + first_page * PAGE_SIZE;
    std::scoped_lock lock{g_decommit};
    ForgetPages(begin, begin + pages * PAGE_SIZE);
    VirtualFree(begin, pages * PAGE_SIZE, MEM_DECOMMIT);
}

u64 Committed() {
    u64 committed = 0;
    for (u8* address = g_base; address < g_base + BACKING;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0) break;
        u8* const end = (std::min)(static_cast<u8*>(info.BaseAddress) + info.RegionSize, g_base + BACKING);
        if (info.State == MEM_COMMIT) committed += static_cast<u64>(end - address);
        address = end;
    }
    return committed;
}

u64 Word(size_t page, u32 version, size_t word) {
    return version == 0 ? 0 : ((u64{page} << 24 | version) * 0x9E3779B97F4A7C15ULL + word) | 1;
}

// Swapped (or moving): the reserved pages of the granule are not zero.
bool Swapped(size_t granule) {
    const u8* const begin = g_base + granule * GRANULE_SIZE;
    return ClipUntouched(begin, begin + GRANULE_SIZE) == begin;
}

volatile u64* PageWords(size_t page) {
    return reinterpret_cast<volatile u64*>(g_base + page * PAGE_SIZE);
}

void Fill(size_t page, u32 version) {
    volatile u64* const words = PageWords(page);
    for (size_t w = 0; w < PAGE_SIZE / 8; ++w) words[w] = Word(page, version, w);
}

bool Check(size_t page, u32 version) {
    volatile u64* const words = PageWords(page);
    for (size_t w = 0; w < PAGE_SIZE / 8; ++w) {
        if (words[w] != Word(page, version, w)) return false;
    }
    return true;
}

// What a page that failed its check holds: which version, and how many of its words.
void Report(size_t page, u32 expected) {
    volatile u64* const words = PageWords(page);
    u32 found = ~u32{};
    size_t matching = 0;
    for (u32 v = 0; v <= expected + 2 && found == ~u32{}; ++v) {
        if (words[PAGE_SIZE / 8 - 1] == Word(page, v, PAGE_SIZE / 8 - 1)) found = v;
    }
    size_t first_bad = PAGE_SIZE / 8;
    for (size_t w = 0; w < PAGE_SIZE / 8; ++w) {
        if (words[w] == Word(page, expected, w)) {
            ++matching;
        } else if (first_bad == PAGE_SIZE / 8) {
            first_bad = w;
        }
    }
    std::printf("mismatch: page %zu (granule %zu, page %zu in it) expected v%u, last word is v%d, "
                "%zu words right, first wrong %zu (%016llx), now %s\n",
                page, page / GRANULE_PAGES, page % GRANULE_PAGES, expected, static_cast<int>(found),
                matching, first_bad,
                static_cast<unsigned long long>(first_bad < PAGE_SIZE / 8 ? words[first_bad] : 0),
                Check(page, expected) ? "right" : "still wrong");
}

// Steps until `want` is given back or the passes run out.
StepResult Drain(u64 want, u32 probation_ms, int passes = 8) {
    StepResult total;
    for (int pass = 0; pass < passes && total.released < want; ++pass) {
        const StepResult step = Step(want - total.released, false, g_decommit);
        total.released += step.released;
        total.swapped += step.swapped;
        total.zero += step.zero;
        total.probation += step.probation;
        std::this_thread::sleep_for(std::chrono::milliseconds(probation_ms + 5));
    }
    return total;
}

void TestWriteOutAndReadBack(std::vector<u32>& version) {
    // 96 MiB written, every 7th page only read (committed zero), granules 0..3 all zero: they come
    // first, so the drain reaches them.
    for (size_t page = 0; page < 96 * MiB / PAGE_SIZE; ++page) {
        const size_t granule = page / GRANULE_PAGES;
        if (page % 7 == 0 || granule < 4) {
            (void)*PageWords(page);
            version[page] = 0;
        } else {
            Fill(page, version[page] = 1);
        }
    }
    const u64 before = Committed();
    assert(before >= 96 * MiB);
    const auto start = std::chrono::steady_clock::now();
    const StepResult drained = Drain(64 * MiB, 40);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const u64 after = Committed();
    const Stats stats = GetStats();
    std::printf("write-out: released %llu MiB (%u swapped, %u zero, %u on probation), commit %llu -> "
                "%llu MiB, %.0f ms with the probation\n",
                static_cast<unsigned long long>(drained.released >> 20), drained.swapped,
                drained.zero, drained.probation, static_cast<unsigned long long>(before >> 20),
                static_cast<unsigned long long>(after >> 20), seconds * 1000);
    assert(drained.released >= 64 * MiB);
    assert(before - after >= 64 * MiB);
    assert(stats.swapped_bytes > 0 && stats.written_bytes >= stats.swapped_bytes);
    assert(stats.zero_bytes > 0);
    assert(drained.zero >= 4); // the all-zero granules cost no disk
    // Something is out: the run up to it reads as zero, the swapped granule does not.
    size_t swapped_granule = 0;
    while (!Swapped(swapped_granule)) ++swapped_granule;
    assert(swapped_granule < 48);
    assert(ClipUntouched(g_base, g_base + BACKING) <= g_base + swapped_granule * GRANULE_SIZE);
    const u64 swap_ins = stats.swap_ins;
    const auto read_start = std::chrono::steady_clock::now();
    for (size_t page = 0; page < 96 * MiB / PAGE_SIZE; ++page) {
        assert(Check(page, version[page]));
    }
    const double read_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - read_start).count();
    const Stats read = GetStats();
    std::printf("read back: %llu granules, %llu MiB in %.0f ms (avg %llu us, max %llu us)\n",
                static_cast<unsigned long long>(read.swap_ins - swap_ins),
                static_cast<unsigned long long>(read.read_bytes >> 20), read_seconds * 1000,
                static_cast<unsigned long long>(read.swap_ins ? read.swap_in_us / read.swap_ins : 0),
                static_cast<unsigned long long>(read.swap_in_max_us));
    assert(read.swap_ins > swap_ins);
    assert(read.swapped_bytes == 0);
    assert(read.read_bytes == read.written_bytes);
    assert(IsResident(g_base + swapped_granule * GRANULE_SIZE));
}

void TestTouchOnProbation(std::vector<u32>& version) {
    // Everything on probation, then granule 5 is read: it must stay in memory. All is resident
    // after the read back; the wait lets the second chances run out.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const Stats before = GetStats();
    Step(BACKING, false, g_decommit);
    assert(GetStats().probation_bytes > 0);
    assert(!IsResident(g_base + 5 * GRANULE_SIZE));
    assert(Check(5 * GRANULE_PAGES + 1, version[5 * GRANULE_PAGES + 1]));
    assert(IsResident(g_base + 5 * GRANULE_SIZE));
    assert(GetStats().probation_touches > before.probation_touches);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Step(BACKING, false, g_decommit);
    assert(IsResident(g_base + 5 * GRANULE_SIZE)); // second chance, not taken right back
    for (size_t page = 0; page < 96 * MiB / PAGE_SIZE; ++page) {
        assert(Check(page, version[page]));
    }
}

void TestClearSwapped(std::vector<u32>& version) {
    // Granule 10 out, then its first half cleared: that half reads zero, the rest its data.
    const size_t first = 10 * GRANULE_PAGES;
    for (size_t page = first; page < first + GRANULE_PAGES; ++page) Fill(page, version[page] = 7);
    for (int pass = 0; pass < 20 && !Swapped(10); ++pass) {
        Step(BACKING, false, g_decommit);
        std::this_thread::sleep_for(std::chrono::milliseconds(45));
    }
    if (Swapped(10)) {
        const u64 before = GetStats().swapped_bytes;
        Clear(first, GRANULE_PAGES / 2);
        assert(GetStats().swapped_bytes + GRANULE_SIZE / 2 == before);
        for (size_t page = first; page < first + GRANULE_PAGES / 2; ++page) version[page] = 0;
        Clear(first + GRANULE_PAGES / 2, GRANULE_PAGES / 2);
        assert(IsResident(g_base + 10 * GRANULE_SIZE)); // nothing left on disk: slot freed
        for (size_t page = first + GRANULE_PAGES / 2; page < first + GRANULE_PAGES; ++page) {
            version[page] = 0;
        }
        std::printf("clear of a swapped granule: ok\n");
    } else {
        std::printf("clear of a swapped granule: granule stayed resident\n");
        assert(false);
    }
    for (size_t page = 0; page < 96 * MiB / PAGE_SIZE; ++page) {
        assert(Check(page, version[page]));
    }
}

void TestConcurrent(std::vector<u32>& version) {
    // Four writers share every granule of the first 128 MiB (page % 4). Almost every operation
    // lands in the hot first 16 MiB; one in 4096 goes anywhere, so a cold granule is touched about
    // every 100 ms, past its probation, and comes back from disk. One in 64 clears the page.
    constexpr size_t STRESS_PAGES = 128 * MiB / PAGE_SIZE;
    constexpr int THREADS = 4;
    std::atomic<bool> stop{false};
    std::atomic<u64> ops{0};
    std::atomic<u64> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 random(0x5EED + t);
            u64 done = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const bool hot = random() % 4096 != 0;
                size_t page = random() % (hot ? STRESS_PAGES / 8 : STRESS_PAGES);
                page = page - page % THREADS + static_cast<size_t>(t);
                if (!Check(page, version[page])) {
                    if (mismatches.fetch_add(1) < 8) Report(page, version[page]);
                }
                const u64 action = random() % 64;
                if (action == 0) {
                    Clear(page, 1);
                    version[page] = 0;
                } else if (action < 16) {
                    Fill(page, ++version[page]);
                }
                ++done;
            }
            ops.fetch_add(done);
        });
    }
    std::atomic<u64> steps{0};
    std::thread swapper([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            Step(64 * MiB, steps.fetch_add(1) % 8 == 0, g_decommit);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const Stats before = GetStats();
    std::this_thread::sleep_for(std::chrono::seconds(4));
    stop = true;
    for (auto& thread : threads) thread.join();
    swapper.join();
    const Stats after = GetStats();
    std::printf("concurrent: %llu operations, %llu mismatches, %llu swap-ins (max %llu us), "
                "written %llu MiB, touched on probation %llu of %llu\n",
                static_cast<unsigned long long>(ops.load()),
                static_cast<unsigned long long>(mismatches.load()),
                static_cast<unsigned long long>(after.swap_ins - before.swap_ins),
                static_cast<unsigned long long>(after.swap_in_max_us),
                static_cast<unsigned long long>((after.written_bytes - before.written_bytes) >> 20),
                static_cast<unsigned long long>(after.probation_touches - before.probation_touches),
                static_cast<unsigned long long>(after.probation_starts - before.probation_starts));
    assert(mismatches.load() == 0);
    assert(after.swap_ins > before.swap_ins);
    assert(after.written_bytes > before.written_bytes);
    assert(after.probation_touches > before.probation_touches);
    assert(after.write_failures == 0);
    for (size_t page = 0; page < PAGES; ++page) {
        assert(Check(page, version[page]));
    }
}

void TestReserve() {
    assert(!ReserveReleased());
    assert(ReleaseReserve());
    assert(ReserveReleased());
    assert(!ReleaseReserve());
    assert(RefillReserve());
    assert(!ReserveReleased());
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_base = static_cast<u8*>(VirtualAlloc(nullptr, BACKING, MEM_RESERVE, PAGE_READWRITE));
    assert(g_base != nullptr);
    void* const handler = AddVectoredExceptionHandler(1, Handler);
    assert(handler != nullptr);
    wchar_t temp[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temp) != 0);
    Config config;
    config.path = std::wstring{temp} + L"jadyx-guest-swap-gate.bin";
    config.reserve_bytes = 8 * MiB;
    config.probation_ms = 40;
    assert(!Active());
    assert(Init(g_base, BACKING, PLATFORM, config));
    assert(Active());
    assert(!Init(g_base, BACKING, PLATFORM, config)); // one backing only

    std::vector<u32> version(PAGES, 0);
    TestReserve();
    TestWriteOutAndReadBack(version);
    TestTouchOnProbation(version);
    TestClearSwapped(version);
    TestConcurrent(version);
    std::cout << Describe() << '\n';

    Shutdown();
    assert(!Active());
    assert(GetFileAttributesW(config.path.c_str()) == INVALID_FILE_ATTRIBUTES); // deleted on close
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(g_base, 0, MEM_RELEASE);
    std::cout << "guest swap gates passed\n";
    return 0;
}
