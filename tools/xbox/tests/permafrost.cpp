// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Permafrost on a real demand-committed reservation: data survives probation, write-out, windowed
// read back, prefetch, clears and concurrent writers; the commit really goes down; the scout's
// probes see use; quick refaults shield a granule; faults count their stall; a write-out goes a
// batch at a time unless in an emergency.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include "common/permafrost.cpp"

namespace {
using namespace Common::Permafrost;
constexpr size_t MiB = 1024 * 1024;
constexpr size_t BACKING = 256 * MiB;
constexpr size_t PAGES = BACKING / PAGE_SIZE;
constexpr size_t CHUNK = 64 * 1024;
constexpr u32 PROBATION_MS = 40;
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

std::atomic<u32> g_named{};

const Platform PLATFORM{
    [](void* address, size_t size) {
        return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    },
    [](void* address, size_t size, u32 protection) {
        DWORD old{};
        return VirtualProtect(address, size, protection, &old) != FALSE;
    },
    [](size_t size) { return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); },
    [](const char*) { g_named.fetch_add(1); },
};

// The kernel clearing pages: what is on disk for them goes, then they are decommitted.
void Clear(size_t first_page, size_t pages) {
    u8* const begin = g_base + first_page * PAGE_SIZE;
    std::scoped_lock lock{g_decommit};
    ForgetPages(begin, begin + pages * PAGE_SIZE);
    VirtualFree(begin, pages * PAGE_SIZE, MEM_DECOMMIT);
}

u64 Committed(const u8* begin, size_t size) {
    u64 committed = 0;
    for (const u8* address = begin; address < begin + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0) break;
        const u8* const end = (std::min)(static_cast<const u8*>(info.BaseAddress) + info.RegionSize, begin + size);
        if (info.State == MEM_COMMIT) committed += static_cast<u64>(end - address);
        address = end;
    }
    return committed;
}

u64 Committed() {
    return Committed(g_base, BACKING);
}

u64 Word(size_t page, u32 version, size_t word) {
    return version == 0 ? 0 : ((u64{page} << 24 | version) * 0x9E3779B97F4A7C15ULL + word) | 1;
}

// Something of the granule is on disk (or moving): its reserved pages are not zero.
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

void CheckAll(const std::vector<u32>& version) {
    for (size_t page = 0; page < PAGES; ++page) {
        assert(Check(page, version[page]));
    }
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

void Pause(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Steps until `want` is given back or the passes run out; the writers finish before it returns.
u64 Drain(u64 want, Urgency urgency = Urgency::Normal, int passes = 12) {
    u64 released = 0;
    for (int pass = 0; pass < passes && released < want; ++pass) {
        released += Step(want - released, urgency, g_decommit).released;
        Pause(PROBATION_MS + 5);
    }
    Settle();
    return released + Step(0, Urgency::Watch, g_decommit).released;
}

// Steps until the granule has data on disk, with the writers done.
bool SwapOut(size_t granule) {
    for (int pass = 0; pass < 40; ++pass) {
        Step(BACKING, Urgency::Normal, g_decommit);
        Pause(PROBATION_MS + 5);
        Settle();
        if (Swapped(granule) && !IsResident(g_base + granule * GRANULE_SIZE)) {
            return true;
        }
    }
    return false;
}

void TestWriteOutAndReadBack(std::vector<u32>& version) {
    // 96 MiB written, every 7th page only read (committed zero), granules 0..3 all zero.
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
    const u64 released = Drain(64 * MiB);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const u64 after = Committed();
    const Stats stats = GetStats();
    std::printf("write-out: released %llu MiB, commit %llu -> %llu MiB, %.0f ms with the probation; "
                "%llu evictions\n",
                static_cast<unsigned long long>(released >> 20),
                static_cast<unsigned long long>(before >> 20),
                static_cast<unsigned long long>(after >> 20), seconds * 1000,
                static_cast<unsigned long long>(stats.evictions));
    assert(released >= 64 * MiB);
    assert(before - after >= 64 * MiB);
    assert(stats.swapped_bytes > 0 && stats.written_bytes >= stats.swapped_bytes);
    assert(stats.zero_bytes > 0);
    assert(stats.writing_bytes == 0);
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
    Settle();
    const Stats read = GetStats();
    std::printf("read back: %llu reads (%llu windows, %llu prefetch), %llu MiB in %.0f ms (avg %llu us, "
                "max %llu us), %llu zero faults\n",
                static_cast<unsigned long long>(read.swap_ins - swap_ins),
                static_cast<unsigned long long>(read.window_reads),
                static_cast<unsigned long long>(read.prefetch_reads),
                static_cast<unsigned long long>(read.read_bytes >> 20), read_seconds * 1000,
                static_cast<unsigned long long>(
                    read.swap_ins > read.prefetch_reads ? read.swap_in_us / (read.swap_ins - read.prefetch_reads) : 0),
                static_cast<unsigned long long>(read.swap_in_max_us),
                static_cast<unsigned long long>(read.zero_faults));
    assert(read.swap_ins > swap_ins);
    assert(read.window_reads > 0); // walking forward reads a granule in growing windows
    assert(read.swapped_bytes == 0);
    assert(read.read_bytes == read.written_bytes);
    assert(IsResident(g_base + swapped_granule * GRANULE_SIZE));
}

void TestTouchOnProbation(std::vector<u32>& version) {
    // Everything on probation, then granule 5 is read: it must stay in memory and is shielded a
    // while. The wait lets the shields of the quick refaults above run out.
    Pause(900);
    const Stats before = GetStats();
    Step(BACKING, Urgency::Normal, g_decommit);
    assert(GetStats().probation_bytes > 0);
    assert(!IsResident(g_base + 5 * GRANULE_SIZE));
    assert(Check(5 * GRANULE_PAGES + 1, version[5 * GRANULE_PAGES + 1]));
    assert(IsResident(g_base + 5 * GRANULE_SIZE));
    assert(GetStats().probation_touches > before.probation_touches);
    Pause(50);
    Step(BACKING, Urgency::Normal, g_decommit);
    assert(IsResident(g_base + 5 * GRANULE_SIZE)); // shielded, not taken right back
    Settle();
    CheckAll(version);
    std::printf("touch on probation: ok\n");
}

void TestClearSwapped(std::vector<u32>& version) {
    // Granule 50, never out before, goes out; then its first half is cleared: that half reads
    // zero, the rest its data.
    const size_t first = 50 * GRANULE_PAGES;
    for (size_t page = first; page < first + GRANULE_PAGES; ++page) Fill(page, version[page] = 7);
    Pause(250);
    assert(SwapOut(50));
    const u64 before = GetStats().swapped_bytes;
    Clear(first, GRANULE_PAGES / 2);
    assert(GetStats().swapped_bytes + GRANULE_SIZE / 2 == before);
    for (size_t page = first; page < first + GRANULE_PAGES / 2; ++page) version[page] = 0;
    Clear(first + GRANULE_PAGES / 2, GRANULE_PAGES / 2);
    assert(IsResident(g_base + 50 * GRANULE_SIZE)); // nothing left on disk: slot freed
    for (size_t page = first + GRANULE_PAGES / 2; page < first + GRANULE_PAGES; ++page) {
        version[page] = 0;
    }
    CheckAll(version);
    std::printf("clear of a swapped granule: ok\n");
}

void TestPartialSwapIn(std::vector<u32>& version) {
    // A fault in granule 60 reads back a window of it, not all of it.
    const size_t first = 60 * GRANULE_PAGES;
    for (size_t page = first; page < first + GRANULE_PAGES; ++page) Fill(page, version[page] = 9);
    Pause(250);
    assert(SwapOut(60));
    const Stats before = GetStats();
    assert(Check(first + 200, version[first + 200]));
    Settle();
    const Stats after = GetStats();
    const u64 read = after.read_bytes - before.read_bytes;
    std::printf("partial read back: %llu KiB of 2048\n", static_cast<unsigned long long>(read >> 10));
    assert(read > 0 && read < GRANULE_SIZE / 4);
    assert(Swapped(60)); // the rest is still on disk
    assert(after.window_reads > before.window_reads);
    assert(after.refaults > before.refaults);
    CheckAll(version);
}

void TestZeroPageInPartial(std::vector<u32>& version) {
    // Granule 70: data on the even pages, zero on the odd ones. Out, a fault on an odd page
    // commits that page alone, as its neighbours are on disk.
    const size_t first = 70 * GRANULE_PAGES;
    for (size_t page = first; page < first + GRANULE_PAGES; ++page) {
        if (page % 2 == 0) {
            Fill(page, version[page] = 11);
        } else {
            (void)*PageWords(page);
            version[page] = 0;
        }
    }
    Pause(250);
    assert(SwapOut(70));
    u8* const granule = g_base + 70 * GRANULE_SIZE;
    assert(Committed(granule, GRANULE_SIZE) == 0);
    const Stats before = GetStats();
    assert(Check(first + 101, 0));
    const Stats after = GetStats();
    assert(after.zero_faults == before.zero_faults + 1);
    assert(after.read_bytes == before.read_bytes);
    assert(Committed(granule, GRANULE_SIZE) == PAGE_SIZE);
    assert(Swapped(70));
    // A write there stays, and the data around it comes back right.
    Fill(first + 101, version[first + 101] = 12);
    CheckAll(version);
    std::printf("zero page in a granule on disk: ok\n");
}

void TestRefaultShield(std::vector<u32>& version) {
    // Granule 80 comes back right after it went out: an ordinary step leaves it alone after
    // that, an emergency does not.
    const size_t first = 80 * GRANULE_PAGES;
    for (size_t page = first; page < first + GRANULE_PAGES; ++page) Fill(page, version[page] = 13);
    Pause(250);
    assert(SwapOut(80));
    const Stats before = GetStats();
    for (size_t page = first; page < first + GRANULE_PAGES; ++page) {
        assert(Check(page, version[page]));
    }
    Settle();
    assert(GetStats().quick_refaults > before.quick_refaults);
    assert(IsResident(g_base + 80 * GRANULE_SIZE));
    Step(BACKING, Urgency::Normal, g_decommit);
    assert(IsResident(g_base + 80 * GRANULE_SIZE));
    assert(GetStats().shielded > 0);
    Step(BACKING, Urgency::Emergency, g_decommit);
    assert(!IsResident(g_base + 80 * GRANULE_SIZE));
    CheckAll(version);
    Settle();
    std::printf("refault shield: ok\n");
}

void TestProbes(std::vector<u32>& version) {
    // All back in memory and left idle; the scout arms a probe per granule, and the guest using
    // them hits the probes.
    CheckAll(version);
    Settle();
    Pause(150);
    const Stats before = GetStats();
    Step(0, Urgency::Watch, g_decommit);
    const Stats armed = GetStats();
    assert(armed.probe_arms > before.probe_arms);
    CheckAll(version);
    const Stats hit = GetStats();
    std::printf("probes: %llu armed, %llu hit\n",
                static_cast<unsigned long long>(armed.probe_arms - before.probe_arms),
                static_cast<unsigned long long>(hit.probe_hits - before.probe_hits));
    assert(hit.probe_hits > before.probe_hits);
    // A write on an armed page lands too.
    Pause(150);
    Step(0, Urgency::Watch, g_decommit);
    for (size_t page = 0; page < 48 * GRANULE_PAGES; page += 1) {
        if (version[page] != 0) Fill(page, ++version[page]);
    }
    CheckAll(version);
}

void TestCommitOverNoAccess() {
    // A demand commit racing a probe or a probation lands on a committed no-access page: the
    // commit gives it its access back, which the fault paths rely on.
    auto* const page = static_cast<u8*>(VirtualAlloc(nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(page != nullptr);
    page[0] = 42;
    DWORD old{};
    assert(VirtualProtect(page, PAGE_SIZE, PAGE_NOACCESS, &old));
    assert(VirtualAlloc(page, PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE) == page);
    MEMORY_BASIC_INFORMATION info{};
    assert(VirtualQuery(page, &info, sizeof(info)) != 0 && info.Protect == PAGE_READWRITE);
    assert(page[0] == 42);
    VirtualFree(page, 0, MEM_RELEASE);
}

// What a look at a granule must leave as it was.
struct GranuleLook {
    u8 state{};
    u16 probe{};
    u64 last_access{};
    u64 shield_until{};
    u32 slot{};
    u8 heat{};
    bool has_data{};
    size_t on_disk{};
    u64 committed{};
    bool operator==(const GranuleLook&) const = default;
};

GranuleLook Look(size_t granule) {
    const Granule& g = g_engine.load()->table[granule];
    return {g.state.load(), g.probe.load(), g.last_access.load(), g.shield_until.load(), g.slot, g.heat,
            g.has_data.load(), g.data.count(), Committed(g_base + granule * GRANULE_SIZE, GRANULE_SIZE)};
}

// The counters a look must not move, for the engine as a whole.
void AssertStatsKept(const Stats& a, const Stats& b) {
    assert(a.swapped_bytes == b.swapped_bytes && a.probation_bytes == b.probation_bytes);
    assert(a.read_bytes == b.read_bytes && a.swap_ins == b.swap_ins && a.refaults == b.refaults);
    assert(a.probation_touches == b.probation_touches && a.probe_hits == b.probe_hits);
    assert(a.zero_faults == b.zero_faults && a.fault_stall_us == b.fault_stall_us);
}

// Pages a look was shown and whether each held what it should, as seen from the page's first word.
struct Shown {
    size_t first_page{};
    const std::vector<u32>* version{};
    std::vector<u32> times;
    u32 wrong{};
    static void Visit(void* context, size_t first, size_t count, const u8* data) {
        auto& self = *static_cast<Shown*>(context);
        for (size_t i = 0; i < count; ++i) {
            const size_t page = first + i;
            ++self.times[page];
            const auto* const words = reinterpret_cast<const u64*>(data + i * PAGE_SIZE);
            const u32 version_now = (*self.version)[self.first_page + page];
            for (size_t w = 0; w < PAGE_SIZE / 8; ++w) {
                if (words[w] != Word(self.first_page + page, version_now, w)) {
                    ++self.wrong;
                    break;
                }
            }
        }
    }
};

Shown LookInMemory(size_t first_page, size_t pages, const std::vector<u32>& version) {
    Shown shown{first_page, &version, std::vector<u32>(pages)};
    std::scoped_lock lock{g_decommit};
    InspectCommitted(g_base + first_page * PAGE_SIZE, pages, Shown::Visit, &shown);
    return shown;
}

Shown LookOnDisk(size_t first_page, size_t pages, const std::vector<u32>& version) {
    Shown shown{first_page, &version, std::vector<u32>(pages)};
    assert(InspectSwapped(g_base + first_page * PAGE_SIZE, pages, Shown::Visit, &shown));
    return shown;
}

u64 Total(const Shown& shown) {
    u64 total = 0;
    for (const u32 times : shown.times) total += times;
    return total;
}

void TestInspect(std::vector<u32>& version) {
    // Everything back in memory first, and the shields of the earlier tests gone.
    CheckAll(version);
    Settle();
    Pause(900);

    // Resident, with a probe armed: every page is shown, and the probe is still armed.
    const size_t resident = 90;
    const size_t resident_first = resident * GRANULE_PAGES;
    for (size_t page = resident_first; page < resident_first + GRANULE_PAGES; ++page) {
        Fill(page, version[page] = 21);
    }
    Pause(150);
    Step(0, Urgency::Watch, g_decommit);
    GranuleLook before = Look(resident);
    assert(before.state == Resident && before.probe != NO_PROBE);
    const u8* const probe_page = g_base + resident * GRANULE_SIZE + size_t{before.probe} * PAGE_SIZE;
    assert(PageProtection(probe_page) == PAGE_NOACCESS);
    Stats stats = GetStats();
    Shown shown = LookInMemory(resident_first, GRANULE_PAGES, version);
    assert(shown.wrong == 0 && Total(shown) == GRANULE_PAGES);
    assert(Look(resident) == before);
    assert(PageProtection(probe_page) == PAGE_NOACCESS);
    AssertStatsKept(stats, GetStats());

    // A range inside the granule shows only its pages, numbered from the range.
    shown = LookInMemory(resident_first + 100, 50, version);
    assert(shown.wrong == 0 && Total(shown) == 50);
    assert(Look(resident) == before);

    // A range across two granules, from the middle of one to the middle of the next.
    const size_t next = resident + 1;
    for (size_t page = next * GRANULE_PAGES; page < (next + 1) * GRANULE_PAGES; ++page) {
        Fill(page, version[page] = 22);
    }
    shown = LookInMemory(next * GRANULE_PAGES - 10, 20, version);
    assert(shown.wrong == 0 && Total(shown) == 20);

    // Nothing committed: nothing shown, no page gets committed.
    const size_t reserved = 100;
    Clear(reserved * GRANULE_PAGES, GRANULE_PAGES);
    for (size_t page = reserved * GRANULE_PAGES; page < (reserved + 1) * GRANULE_PAGES; ++page) {
        version[page] = 0;
    }
    assert(Committed(g_base + reserved * GRANULE_SIZE, GRANULE_SIZE) == 0);
    shown = LookInMemory(reserved * GRANULE_PAGES, GRANULE_PAGES, version);
    assert(Total(shown) == 0);
    assert(Committed(g_base + reserved * GRANULE_SIZE, GRANULE_SIZE) == 0);

    // Partly committed: the committed ones only.
    Fill(reserved * GRANULE_PAGES + 7, version[reserved * GRANULE_PAGES + 7] = 23);
    const u64 committed = Committed(g_base + reserved * GRANULE_SIZE, GRANULE_SIZE);
    shown = LookInMemory(reserved * GRANULE_PAGES, GRANULE_PAGES, version);
    assert(shown.wrong == 0 && Total(shown) == committed / PAGE_SIZE && shown.times[7] == 1);

    // On probation: shown, and no less on probation afterwards. No touch is counted.
    Step(BACKING, Urgency::Normal, g_decommit);
    before = Look(resident);
    assert(before.state == Probation);
    stats = GetStats();
    shown = LookInMemory(resident_first, GRANULE_PAGES, version);
    assert(shown.wrong == 0 && Total(shown) == GRANULE_PAGES);
    assert(Look(resident) == before);
    AssertStatsKept(stats, GetStats());
    assert(PageProtection(g_base + resident * GRANULE_SIZE) == PAGE_NOACCESS);

    // On disk: shown from there, with nothing committed, nothing read back and its time out kept.
    Pause(PROBATION_MS + 5);
    assert(SwapOut(resident));
    before = Look(resident);
    assert(before.on_disk == GRANULE_PAGES && before.committed == 0);
    stats = GetStats();
    assert(Total(LookInMemory(resident_first, GRANULE_PAGES, version)) == 0);
    shown = LookOnDisk(resident_first, GRANULE_PAGES, version);
    assert(shown.wrong == 0 && Total(shown) == GRANULE_PAGES);
    assert(Look(resident) == before);
    AssertStatsKept(stats, GetStats());
    shown = LookOnDisk(resident_first + 300, 12, version);
    assert(shown.wrong == 0 && Total(shown) == 12);

    // Part on disk, part back: each page shown once, by one look or the other.
    assert(Check(resident_first + 200, version[resident_first + 200]));
    Settle();
    before = Look(resident);
    assert(before.on_disk != 0 && before.on_disk < GRANULE_PAGES && before.committed != 0);
    stats = GetStats();
    const Shown memory = LookInMemory(resident_first, GRANULE_PAGES, version);
    const Shown disk = LookOnDisk(resident_first, GRANULE_PAGES, version);
    assert(memory.wrong == 0 && disk.wrong == 0);
    for (size_t page = 0; page < GRANULE_PAGES; ++page) {
        assert(memory.times[page] + disk.times[page] == 1);
    }
    assert(Look(resident) == before);
    AssertStatsKept(stats, GetStats());
    CheckAll(version);
    Settle();
    std::printf("inspect: ok\n");
}

void TestInspectConcurrent(std::vector<u32>& version) {
    // Looks at the whole backing, with the mutex in slices and without it, while writers fault and
    // clear and the swapper moves granules. No deadlock, and the guest's data stays right. What a
    // look shows is not checked here: the writers change it under it.
    constexpr size_t STRESS_PAGES = 128 * MiB / PAGE_SIZE;
    constexpr size_t SLICE = 16 * GRANULE_PAGES;
    constexpr int THREADS = 4;
    std::atomic<bool> stop{false};
    std::atomic<u64> mismatches{0};
    std::atomic<u64> looks{0};
    std::atomic<u64> pages_shown{0};
    std::vector<std::thread> threads;
    const auto count_pages = [](void* context, size_t, size_t count, const u8* data) {
        static_cast<std::atomic<u64>*>(context)->fetch_add(count, std::memory_order_relaxed);
        // Read it for real: a page that stopped being readable would crash the gate.
        volatile u8 sink = data[(count - 1) * PAGE_SIZE];
        (void)sink;
    };
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            RegisterGuestThread();
            std::mt19937_64 random(0xCAFE + t);
            while (!stop.load(std::memory_order_relaxed)) {
                size_t page = random() % STRESS_PAGES;
                page = page - page % THREADS + static_cast<size_t>(t);
                if (!Check(page, version[page])) mismatches.fetch_add(1);
                const u64 action = random() % 64;
                if (action == 0) {
                    Clear(page, 1);
                    version[page] = 0;
                } else if (action < 16) {
                    Fill(page, ++version[page]);
                }
            }
            UnregisterGuestThread();
        });
    }
    std::thread swapper([&] {
        constexpr Urgency CYCLE[]{Urgency::Normal, Urgency::Urgent, Urgency::Watch, Urgency::Emergency};
        for (u32 step = 0; !stop.load(std::memory_order_relaxed); ++step) {
            Step(64 * MiB, CYCLE[step % 4], g_decommit);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    std::vector<std::thread> lookers;
    for (int l = 0; l < 2; ++l) {
        lookers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (size_t page = 0; page < STRESS_PAGES; page += SLICE) {
                    {
                        std::scoped_lock lock{g_decommit};
                        InspectCommitted(g_base + page * PAGE_SIZE, SLICE, count_pages, &pages_shown);
                    }
                    assert(InspectSwapped(g_base + page * PAGE_SIZE, SLICE, count_pages, &pages_shown));
                }
                looks.fetch_add(1);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop = true;
    for (auto& thread : threads) thread.join();
    for (auto& thread : lookers) thread.join();
    swapper.join();
    Settle();
    std::printf("inspect under load: %llu whole looks, %llu pages shown, %llu mismatches\n",
                static_cast<unsigned long long>(looks.load()),
                static_cast<unsigned long long>(pages_shown.load()),
                static_cast<unsigned long long>(mismatches.load()));
    assert(mismatches.load() == 0);
    assert(looks.load() > 0);
    assert(GetStats().write_failures == 0);
    CheckAll(version);
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
            // As the game's threads are: a read back pauses them, or they would see its pages zero.
            RegisterGuestThread();
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
            UnregisterGuestThread();
        });
    }
    std::atomic<u64> steps{0};
    std::thread swapper([&] {
        constexpr Urgency CYCLE[]{Urgency::Normal, Urgency::Normal, Urgency::Watch, Urgency::Normal,
                                  Urgency::Urgent, Urgency::Normal, Urgency::Normal, Urgency::Emergency};
        while (!stop.load(std::memory_order_relaxed)) {
            Step(64 * MiB, CYCLE[steps.fetch_add(1) % 8], g_decommit);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const Stats before = GetStats();
    std::this_thread::sleep_for(std::chrono::seconds(4));
    stop = true;
    for (auto& thread : threads) thread.join();
    swapper.join();
    Settle();
    const Stats after = GetStats();
    std::printf("concurrent: %llu operations, %llu mismatches, %llu swap-ins (max %llu us), "
                "written %llu MiB, touched on probation %llu of %llu, %llu probe hits, %llu quick refaults\n",
                static_cast<unsigned long long>(ops.load()),
                static_cast<unsigned long long>(mismatches.load()),
                static_cast<unsigned long long>(after.swap_ins - before.swap_ins),
                static_cast<unsigned long long>(after.swap_in_max_us),
                static_cast<unsigned long long>((after.written_bytes - before.written_bytes) >> 20),
                static_cast<unsigned long long>(after.probation_touches - before.probation_touches),
                static_cast<unsigned long long>(after.probation_starts - before.probation_starts),
                static_cast<unsigned long long>(after.probe_hits - before.probe_hits),
                static_cast<unsigned long long>(after.quick_refaults - before.quick_refaults));
    assert(mismatches.load() == 0);
    assert(after.swap_ins > before.swap_ins);
    assert(after.written_bytes > before.written_bytes);
    assert(after.probation_touches > before.probation_touches);
    assert(after.write_failures == 0);
    assert(after.fault_stall_us > before.fault_stall_us);
    CheckAll(version);
}

// A second engine with small batches: one step puts a batch on probation and no more until the
// next batch is due, far short twice that, and an emergency all the shortfall asks for.
void TestBatches(const Config& base_config) {
    constexpr size_t BATCH = 8 * MiB;
    constexpr size_t USED = 96 * MiB;
    VirtualFree(g_base, BACKING, MEM_DECOMMIT);
    Config config = base_config;
    config.batch_bytes = BATCH;
    assert(Init(g_base, BACKING, PLATFORM, config));
    std::vector<u32> version(PAGES, 0);
    for (size_t page = 0; page < USED / PAGE_SIZE; page += 8) Fill(page, version[page] = 3);
    Pause(60);
    // Counted in granules: the zero pages of a granule are not its bytes on probation.
    constexpr u64 PER_BATCH = BATCH / GRANULE_SIZE + 1;
    const auto starts = [] { return GetStats().probation_starts; };
    const u64 start = starts();
    Step(BACKING, Urgency::Normal, g_decommit);
    const u64 normal = starts() - start;
    assert(normal > 0 && normal <= PER_BATCH);
    Step(BACKING, Urgency::Normal, g_decommit);
    assert(starts() - start == normal); // the batch is spent
    Pause(520);
    Step(BACKING, Urgency::Urgent, g_decommit);
    const u64 urgent = starts() - start - normal;
    assert(urgent > 0 && urgent <= 2 * PER_BATCH);
    Step(BACKING, Urgency::Emergency, g_decommit);
    assert(starts() - start - normal - urgent > 2 * PER_BATCH);
    Drain(USED, Urgency::Emergency);
    for (size_t page = 0; page < USED / PAGE_SIZE; page += 8) {
        assert(Check(page, version[page]));
    }
    Settle();
    std::printf("batches: ok (%s)\n", Describe(true).c_str());
    Shutdown();
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
    config.path = std::wstring{temp} + L"jadyx-permafrost-gate.bin";
    config.reserve_bytes = 8 * MiB;
    config.probation_ms = PROBATION_MS;
    config.min_idle_ms = 0;
    config.scout_ms = 50;
    config.shield_ms = 200;
    config.shield_max_ms = 800;
    config.batch_bytes = 0;
    assert(!Active());
    assert(Init(g_base, BACKING, PLATFORM, config));
    assert(Active());
    assert(!Init(g_base, BACKING, PLATFORM, config)); // one backing only
    Pause(50);
    assert(g_named.load() == 1 + config.writers); // the writers and the prefetch thread
    // Reads guest memory as a guest thread does: a fill on the prefetch thread must pause it.
    RegisterGuestThread();

    std::vector<u32> version(PAGES, 0);
    TestCommitOverNoAccess();
    TestReserve();
    TestWriteOutAndReadBack(version);
    TestTouchOnProbation(version);
    TestClearSwapped(version);
    TestPartialSwapIn(version);
    TestZeroPageInPartial(version);
    TestRefaultShield(version);
    TestProbes(version);
    TestInspect(version);
    TestInspectConcurrent(version);
    TestConcurrent(version);
    std::cout << Describe() << '\n';

    Shutdown();
    assert(!Active());
    assert(GetFileAttributesW(config.path.c_str()) == INVALID_FILE_ATTRIBUTES); // deleted on close
    TestBatches(config);
    UnregisterGuestThread();
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(g_base, 0, MEM_RELEASE);
    std::cout << "permafrost gates passed\n";
    return 0;
}
