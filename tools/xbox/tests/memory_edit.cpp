// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Jadyx Memory Edit without the game: what a typed number matches, the comparison against its plain
// version, the result chunks, and a whole search over a real Permafrost reservation and swap file.

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "common/permafrost.cpp"
#include "jadyx/memory_edit/jme_page.h"
#include "jadyx/memory_edit/jme_matcher.cpp"
#include "jadyx/memory_edit/jme_results.cpp"
#include "jadyx/memory_edit/jme_scanner.cpp"
#include "jadyx/memory_edit/jme_session.cpp"

namespace {

using namespace Jadyx::MemoryEdit;
namespace PF = Common::Permafrost;

Query QueryFor(const char* text, ValueKind kind) {
    const auto number = ParseNumber(text);
    assert(number);
    const auto query = MakeQuery(*number, kind);
    assert(query);
    return *query;
}

bool Rejected(const char* text, ValueKind kind) {
    const auto number = ParseNumber(text);
    assert(number);
    return !MakeQuery(*number, kind);
}

void TestValues() {
    for (const char* bad : {"", "-", ".", "-.", "1.2.3", "12a", "+5", "1e5", " 5",
                            "123456789012345678901234567890123"}) {
        assert(!ParseNumber(bad));
    }
    assert(ParseNumber(".5"));
    assert(ParseNumber("5."));

    assert(QueryFor("5000", ValueKind::I32).bits == 5000);
    assert(QueryFor("-1", ValueKind::I8).bits == 0xFF);
    assert(QueryFor("-1", ValueKind::I16).bits == 0xFFFF);
    assert(QueryFor("-1", ValueKind::I32).bits == 0xFFFFFFFF);
    assert(QueryFor("-1", ValueKind::I64).bits == ~u64{});
    assert(QueryFor("255", ValueKind::I8).bits == 0xFF);
    assert(QueryFor("-128", ValueKind::I8).bits == 0x80);
    assert(QueryFor("18446744073709551615", ValueKind::I64).bits == ~u64{});
    assert(Rejected("256", ValueKind::I8));
    assert(Rejected("-129", ValueKind::I8));
    assert(Rejected("4294967296", ValueKind::I32));
    assert(Rejected("18446744073709551616", ValueKind::I64));
    assert(Rejected("1.5", ValueKind::I32));
    assert(Rejected("5.", ValueKind::I32));

    const Query hundred = QueryFor("100", ValueKind::F32);
    assert(hundred.low32 == 99.5f && hundred.high32 == 101.0f);
    const Query hundred64 = QueryFor("100", ValueKind::F64);
    assert(hundred64.low64 == 99.5 && hundred64.high64 == 101.0);
    const Query decimal = QueryFor("12.5", ValueKind::F64);
    assert(decimal.low64 <= 12.45 && decimal.low64 > 12.44 && decimal.high64 >= 12.6 && decimal.high64 < 12.61);
    const Query negative = QueryFor("-5", ValueKind::F64);
    assert(negative.low64 == -6.0 && negative.high64 == -4.5);
    const Query zero = QueryFor("0", ValueKind::F64);
    assert(zero.low64 == -0.5 && zero.high64 == 1.0);
    assert(Rejected("99999999999999999999999999999999", ValueKind::I64));
    // Past what a float tells apart, the float nearest to the number still matches.
    const Query coarse = QueryFor("1000000.01", ValueKind::F32);
    assert(1000000.01f >= coarse.low32 && 1000000.01f < coarse.high32);
    const Query huge = QueryFor("12345678901234567890.5", ValueKind::F64);
    assert(huge.low64 < huge.high64);

    const QuerySet narrow = AutoQueries(*ParseNumber("5000"));
    assert(narrow.count == 3 && narrow.items[0].kind == ValueKind::I32 && narrow.items[1].kind == ValueKind::F32 &&
           narrow.items[2].kind == ValueKind::F64);
    const QuerySet wide = AutoQueries(*ParseNumber("5000000000"));
    assert(wide.count == 3 && wide.items[0].kind == ValueKind::I64);
    const QuerySet point = AutoQueries(*ParseNumber("1.5"));
    assert(point.count == 2 && point.items[0].kind == ValueKind::F32 && point.items[1].kind == ValueKind::F64);
    std::printf("values: ok\n");
}

template <typename T>
void Plant(std::vector<u8>& page, size_t index, T value) {
    std::memcpy(page.data() + index * sizeof(T), &value, sizeof(T));
}

void Same(const Query& query, const std::vector<u8>& page) {
    u64 fast[MATCH_WORDS]{};
    u64 plain[MATCH_WORDS]{};
    const u32 fast_hits = MatchPage(query, page.data(), fast);
    const u32 plain_hits = MatchPageScalar(query, page.data(), plain);
    assert(fast_hits == plain_hits);
    assert(std::memcmp(fast, plain, MatchWords(query.kind) * sizeof(u64)) == 0);
}

void TestMatcher() {
#ifndef __AVX2__
#error "This gate compares the AVX2 path: build it with /arch:AVX2"
#endif
    const std::vector<Query> queries{
        QueryFor("-1", ValueKind::I8),      QueryFor("100", ValueKind::I8),
        QueryFor("-2", ValueKind::I16),     QueryFor("5000", ValueKind::I16),
        QueryFor("5000", ValueKind::I32),   QueryFor("-5000", ValueKind::I32),
        QueryFor("5000", ValueKind::I64),   QueryFor("-5000", ValueKind::I64),
        QueryFor("100", ValueKind::F32),    QueryFor("-12.5", ValueKind::F32),
        QueryFor("100", ValueKind::F64),    QueryFor("-12.5", ValueKind::F64),
    };
    std::mt19937_64 random(0x4A4D45);
    std::vector<u8> page(MATCH_PAGE_SIZE);
    // Random data: nearly nothing matches.
    for (int round = 0; round < 200; ++round) {
        for (u8& byte : page) byte = static_cast<u8>(random());
        for (const Query& query : queries) Same(query, page);
    }
    // Small values everywhere, so that everything matches somewhere.
    for (int round = 0; round < 200; ++round) {
        for (u8& byte : page) byte = static_cast<u8>(random() % 4 == 0 ? random() : 0);
        for (size_t i = 0; i < 80; ++i) {
            switch (random() % 6) {
            case 0: Plant<u8>(page, random() % 4096, 100); break;
            case 1: Plant<u16>(page, random() % 2048, 5000); break;
            case 2: Plant<u32>(page, random() % 1024, 5000); break;
            case 3: Plant<u64>(page, random() % 512, 5000); break;
            case 4: Plant<float>(page, random() % 1024, 99.5f + static_cast<float>(random() % 100) * 0.02f); break;
            default: Plant<double>(page, random() % 512, -12.5 - static_cast<double>(random() % 4) * 0.03); break;
            }
        }
        for (const Query& query : queries) Same(query, page);
    }

    // The edges of the page, and an unaligned value that must not count.
    std::fill(page.begin(), page.end(), u8{0});
    u64 bits[MATCH_WORDS];
    Plant<u32>(page, 0, 5000);
    Plant<u32>(page, 1023, 5000);
    const Query int32 = QueryFor("5000", ValueKind::I32);
    assert(MatchPage(int32, page.data(), bits) == 2);
    assert((bits[0] & 1) != 0 && (bits[15] >> 63) != 0);
    std::fill(page.begin(), page.end(), u8{0});
    const u32 value = 5000;
    std::memcpy(page.data() + 2, &value, sizeof(value));
    assert(MatchPage(int32, page.data(), bits) == 0);
    Same(int32, page);

    // Bounds: low in, high out, and NaN and infinity nowhere.
    std::fill(page.begin(), page.end(), u8{0});
    Plant<float>(page, 0, 99.5f);
    Plant<float>(page, 1, 101.0f);
    Plant<float>(page, 2, std::numeric_limits<float>::quiet_NaN());
    Plant<float>(page, 3, std::numeric_limits<float>::infinity());
    Plant<float>(page, 4, std::nextafter(101.0f, 0.0f));
    const Query real = QueryFor("100", ValueKind::F32);
    assert(MatchPage(real, page.data(), bits) == 2);
    assert(bits[0] == 0b10001);
    Same(real, page);
    std::fill(page.begin(), page.end(), u8{0});
    Plant<double>(page, 0, 99.5);
    Plant<double>(page, 1, 101.0);
    Plant<double>(page, 2, std::numeric_limits<double>::quiet_NaN());
    const Query real64 = QueryFor("100", ValueKind::F64);
    assert(MatchPage(real64, page.data(), bits) == 1);
    assert(bits[0] == 1);
    // All the page.
    std::fill(page.begin(), page.end(), u8{0xFF});
    const Query full = QueryFor("-1", ValueKind::I8);
    assert(MatchPage(full, page.data(), bits) == MATCH_PAGE_SIZE);
    Same(full, page);
    std::printf("matcher: ok\n");
}

struct Expected {
    u64 page;
    ValueKind kind;
    std::vector<u64> bits;
};

void RoundTrip(const std::vector<Expected>& records, u8* chunk) {
    ChunkWriter writer{chunk};
    u32 total = 0;
    for (const Expected& record : records) {
        u32 hits = 0;
        for (const u64 word : record.bits) hits += static_cast<u32>(std::popcount(word));
        assert(writer.Append(record.page, record.kind, record.bits.data(), hits));
        total += hits;
    }
    assert(writer.Hits() == total);
    ChunkReader reader{chunk};
    assert(reader.Hits() == total);
    ChunkRecord out;
    u32 read_hits = 0;
    for (const Expected& record : records) {
        assert(reader.Next(out));
        assert(out.page == record.page && out.kind == record.kind);
        assert(std::memcmp(out.bits, record.bits.data(), record.bits.size() * sizeof(u64)) == 0);
        read_hits += out.hits;
    }
    assert(!reader.Next(out));
    assert(!reader.Broken());
    assert(read_hits == total);
}

size_t UsedBytes(const u8* chunk) {
    u32 used{};
    std::memcpy(&used, chunk, sizeof(used));
    return used;
}

void TestCodec() {
    u8* const chunk = NewChunkBuffer();
    assert(chunk != nullptr);
    const auto map = [](ValueKind kind) { return std::vector<u64>(MatchWords(kind), u64{0}); };

    // One sparse hit, a dense page, pages going backwards, and the same page with another kind.
    auto sparse = map(ValueKind::I32);
    sparse[3] = u64{1} << 17;
    auto dense = map(ValueKind::I8);
    for (u64& word : dense) word = 0x5555555555555555;
    auto last_bits = map(ValueKind::F64);
    last_bits[7] = u64{1} << 63;
    auto first_bit = map(ValueKind::I16);
    first_bit[0] = 1;
    RoundTrip({{0x12345, ValueKind::I32, sparse},
               {0x12345, ValueKind::I8, dense},
               {0x10000, ValueKind::F64, last_bits},
               {0x7FFFFFFFF, ValueKind::I16, first_bit},
               {0, ValueKind::I32, sparse}},
              chunk);

    // A thousand scattered hits take what the plan promised: well under a chunk.
    {
        ChunkWriter writer{chunk};
        for (u64 i = 0; i < 1000; ++i) {
            assert(writer.Append(0x100000 + i * 5, ValueKind::I32, sparse.data(), 1));
        }
        assert(UsedBytes(chunk) < 7 * 1024);
    }
    // The shorter format is chosen: a dense page is its map, not a longer list.
    {
        ChunkWriter writer{chunk};
        assert(writer.Append(1, ValueKind::I8, dense.data(), 2048));
        assert(UsedBytes(chunk) <= 8 + 2 + 512);
    }
    // A chunk ends cleanly, however it fills up.
    {
        ChunkWriter writer{chunk};
        u64 added = 0;
        while (writer.Append(added, ValueKind::I8, dense.data(), 2048)) ++added;
        assert(added > 100 && UsedBytes(chunk) <= CHUNK_SIZE);
        writer.Reset();
        assert(writer.Hits() == 0);
        assert(writer.Append(5, ValueKind::I8, dense.data(), 2048));
    }
    // A broken chunk is told, not walked past its end.
    {
        ChunkWriter writer{chunk};
        assert(writer.Append(5, ValueKind::I8, dense.data(), 2048));
        const u32 bad_used = CHUNK_SIZE + 1;
        std::memcpy(chunk, &bad_used, sizeof(bad_used));
        ChunkReader reader{chunk};
        ChunkRecord record;
        assert(!reader.Next(record) && reader.Broken());
    }
    FreeChunkBuffer(chunk);
    std::printf("codec: ok\n");
}

std::wstring TempFile(const wchar_t* name) {
    wchar_t folder[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder);
    return std::wstring{folder} + name;
}

void TestFile() {
    u8* const chunk = NewChunkBuffer();
    u8* const back = NewChunkBuffer();
    const std::wstring path = TempFile(L"jme_gate_results.bin");
    {
        ResultFile file;
        assert(file.Open(path, 3 * CHUNK_SIZE));
        for (u8 i = 0; i < 3; ++i) {
            std::memset(chunk, 0x40 + i, CHUNK_SIZE);
            const auto slot = file.Write(chunk);
            assert(slot && *slot == i);
        }
        assert(!file.Full());
        assert(!file.Write(chunk));
        assert(file.Full());
        assert(file.Bytes() == 3 * CHUNK_SIZE);
        assert(file.Read(1, back));
        assert(back[0] == 0x41 && back[CHUNK_SIZE - 1] == 0x41);

        // A list takes the slots it was given, and no more.
        file.BeginList({1, 2});
        assert(!file.Full());
        std::memset(chunk, 0x50, CHUNK_SIZE);
        const auto slot = file.Write(chunk);
        assert(slot && *slot == 1);
        assert(!file.Write(chunk));
        assert(file.Full());
        assert((file.ListRange() == SlotRange{1, 2}));
        assert(file.Read(0, back) && back[0] == 0x40);
        assert(file.Read(1, back) && back[0] == 0x50);
        assert(file.Read(2, back) && back[0] == 0x42);
    }
    // Deleted with its handle.
    assert(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    // The next list never overlaps the one it comes from, and goes back to the start when it can.
    assert((NextListRange({0, 2048}, 4096) == SlotRange{2048, 4096}));
    assert((NextListRange({2048, 4096}, 4096) == SlotRange{0, 2048}));
    assert((NextListRange({2048, 2100}, 4096) == SlotRange{0, 2048}));
    assert((NextListRange({0, 5}, 4096) == SlotRange{5, 4096}));
    assert((NextListRange({5, 10}, 4096) == SlotRange{10, 4096}));
    assert((NextListRange({100, 105}, 4096) == SlotRange{0, 100}));
    assert((NextListRange({1000, 4000}, 4096) == SlotRange{0, 1000}));
    FreeChunkBuffer(chunk);
    FreeChunkBuffer(back);
    std::printf("file: ok\n");
}

constexpr size_t MiB_ = 1024 * 1024;
constexpr size_t BACKING = 128 * MiB_;
constexpr size_t CHUNK_COMMIT = 64 * 1024;
u8* g_base{};
std::mutex g_decommit;

bool CommitAround(u8* address) {
    const size_t offset = static_cast<size_t>(address - g_base);
    u8* page = g_base + (offset & ~(PF::PAGE_SIZE - 1));
    return VirtualAlloc(page, PF::PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

LONG NTAPI Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    auto* const address = reinterpret_cast<u8*>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (address < g_base || address >= g_base + BACKING) return EXCEPTION_CONTINUE_SEARCH;
    return PF::OnFault(address, CommitAround) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

const PF::Platform PLATFORM{
    [](void* address, size_t size) { return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; },
    [](void* address, size_t size, u32 protection) {
        DWORD old{};
        return VirtualProtect(address, size, protection, &old) != FALSE;
    },
    [](size_t size) { return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); },
    nullptr,
};

constexpr u64 GUEST_BASE = 0x80000000;

// The guest's memory cut into spans the way the walk does, with a hole and a jump in guest addresses.
std::vector<GuestSpan> MakeSpans() {
    std::vector<GuestSpan> spans;
    u32 mark = 0;
    for (size_t offset = 0; offset < BACKING; offset += PF::GRANULE_SIZE) {
        const u32 pages = static_cast<u32>(PF::GRANULE_PAGES);
        const u64 va = GUEST_BASE + offset * 2;
        spans.push_back({va, g_base + offset, pages, mark});
        mark += pages;
    }
    return spans;
}

u64 VaOf(size_t page) {
    return GUEST_BASE + (page / PF::GRANULE_PAGES) * PF::GRANULE_SIZE * 2 + (page % PF::GRANULE_PAGES) * PF::PAGE_SIZE;
}

using Found = std::multiset<std::pair<u64, ValueKind>>;

Found ReadList(const ResultFile& file, const ResultList& list) {
    Found found;
    u8* const buffer = NewChunkBuffer();
    ChunkRecord record;
    for (const ResultChunk& chunk : list) {
        assert(file.Read(chunk.slot, buffer));
        ChunkReader reader{buffer};
        while (reader.Next(record)) {
            const u32 width = ValueWidth(record.kind);
            for (size_t word = 0; word < MatchWords(record.kind); ++word) {
                for (u64 rest = record.bits[word]; rest != 0; rest &= rest - 1) {
                    const u64 index = word * 64 + static_cast<u64>(std::countr_zero(rest));
                    found.insert({(record.page << 12) + index * width, record.kind});
                }
            }
        }
        assert(!reader.Broken());
    }
    FreeChunkBuffer(buffer);
    return found;
}

void AssertQuiet(const PF::Stats& before, const PF::Stats& after) {
    assert(after.swap_ins == before.swap_ins);
    assert(after.read_bytes == before.read_bytes);
    assert(after.probation_touches == before.probation_touches);
    assert(after.refaults == before.refaults);
    assert(after.probe_hits == before.probe_hits);
    assert(after.zero_faults == before.zero_faults);
    assert(after.swapped_bytes == before.swapped_bytes);
    assert(after.evictions == before.evictions);
}

// Waits for the search as the menu does, without blocking on it.
SearchEnd Await(Session& session) {
    while (!session.Poll()) {
        assert(session.Running());
        const SearchProgress now = session.Progress();
        assert(now.permille <= 1000);
        std::this_thread::yield();
    }
    assert(!session.Running());
    return session.LastEnd();
}

std::set<std::pair<u64, ValueKind>> AllEntries(const Session& session) {
    std::set<std::pair<u64, ValueKind>> all;
    std::vector<ResultEntry> entries;
    // A step that cuts chunks and records anywhere.
    constexpr u32 STEP = 37;
    for (u64 first = 0; first < session.Hits(); first += STEP) {
        assert(session.ReadEntries(first, STEP, entries));
        assert(entries.size() == (std::min)(u64{STEP}, session.Hits() - first));
        for (const ResultEntry& entry : entries) {
            assert(all.insert({entry.va, entry.kind}).second);
        }
    }
    assert(session.ReadEntries(session.Hits(), STEP, entries) && entries.empty());
    return all;
}

template <typename T>
void GuestStore(const std::vector<GuestSpan>& spans, u64 va, T value) {
    const u8* const host = HostOfPage(spans, va >> 12);
    assert(host != nullptr);
    std::memcpy(const_cast<u8*>(host) + (va & 0xFFF), &value, sizeof(T));
}

void TestSession(const std::vector<GuestSpan>& all_spans, const std::set<std::pair<u64, ValueKind>>& planted,
                 const std::function<void()>& evict) {
    constexpr std::string_view FIRST_NUMBER = "305441741";
    constexpr std::string_view SECOND_NUMBER = "305441742";

    std::set<std::pair<u64, ValueKind>> ints;
    for (const auto& hit : planted) {
        if (hit.second == ValueKind::I32) ints.insert(hit);
    }

    size_t unmapped_granule = SIZE_MAX;
    const auto walk = [&] {
        GuestMemoryMap map;
        for (const GuestSpan& span : all_spans) {
            if (static_cast<size_t>(span.host - g_base) / PF::GRANULE_SIZE == unmapped_granule) continue;
            map.spans.push_back(span);
            map.pages += span.pages;
        }
        return map;
    };
    const std::wstring path = TempFile(L"jme_gate_results.bin");

    // Nothing runs and nothing is kept before the first search.
    {
        Session session{{path, g_decommit, walk}};
        assert(!session.HasList() && !session.Running());
        assert(session.Refine("5") == SearchStart::NoList);
        assert(session.Search("") == SearchStart::BadNumber);
        assert(session.Search("12a") == SearchStart::BadNumber);
        assert(session.Search("0") == SearchStart::Zero);
        assert(session.Search("0.0") == SearchStart::Zero);
        assert(session.Search("1.5", ValueKind::I32) == SearchStart::BadNumber);
        assert(!session.Running());
        assert(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    Session session{{path, g_decommit, walk}};
    assert(session.Search(FIRST_NUMBER, ValueKind::I32) == SearchStart::Started);
    assert(session.Search(FIRST_NUMBER) == SearchStart::Busy);
    assert(session.Refine(FIRST_NUMBER) == SearchStart::Busy);
    std::vector<ResultEntry> entries;
    assert(!session.ReadEntries(0, 1, entries));
    assert(Await(session) == SearchEnd::Done);
    assert(!session.Poll());
    assert(session.Hits() == ints.size() && !session.IsPartial());
    assert(session.ShownHits() == session.Hits() && !session.MustRefine());
    assert(AllEntries(session) == ints);

    // Refining to the same number: all stay, whether the page is in memory or on disk, and nothing
    // moves. Some of the second half stay on disk from what the scanner test evicted.
    {
        const PF::Stats before = PF::GetStats();
        assert(session.Refine(FIRST_NUMBER) == SearchStart::Started);
        assert(Await(session) == SearchEnd::Done);
        AssertQuiet(before, PF::GetStats());
        assert(session.LastStats().disk_bytes > 0 && session.LastStats().disk_hits > 0);
        assert(session.LastStats().disk_hits < session.Hits());
        assert(session.Hits() == ints.size());
        assert(AllEntries(session) == ints);
    }

    // A kind the number does not fit in, and a number nothing holds: the list stays for another try.
    assert(session.Refine("99999") == SearchStart::Started);
    assert(Await(session) == SearchEnd::NoMatches);
    assert(session.Hits() == ints.size());
    assert(AllEntries(session) == ints);
    assert(session.Refine("1.5") == SearchStart::Started);
    assert(Await(session) == SearchEnd::NoMatches);
    assert(AllEntries(session) == ints);

    // The game changes a value: one out of every 3 goes on to the second number. Writing brings the
    // pages back, then the rest of the memory goes out again, the new values among it.
    std::set<std::pair<u64, ValueKind>> changed;
    {
        size_t index = 0;
        for (const auto& hit : ints) {
            if (index++ % 3 != 0) continue;
            GuestStore<u32>(all_spans, hit.first, 0x1234ABCE);
            changed.insert(hit);
        }
    }
    evict();
    {
        const PF::Stats before = PF::GetStats();
        assert(session.Refine(SECOND_NUMBER) == SearchStart::Started);
        assert(Await(session) == SearchEnd::Done);
        AssertQuiet(before, PF::GetStats());
        assert(session.Hits() == changed.size());
        assert(AllEntries(session) == changed);
    }

    // To zero, which a first search refuses: a value written as zero, and a page in neither memory
    // nor swap, which reads as zeros for the game.
    std::set<std::pair<u64, ValueKind>> zeros;
    {
        size_t index = 0;
        for (const auto& hit : changed) {
            if (index++ % 2 != 0) continue;
            GuestStore<u32>(all_spans, hit.first, 0);
            zeros.insert(hit);
        }
        const u64 va = zeros.begin()->first;
        u8* const host = const_cast<u8*>(HostOfPage(all_spans, va >> 12));
        std::scoped_lock lock{g_decommit};
        assert(VirtualFree(host, PF::PAGE_SIZE, MEM_DECOMMIT));
    }
    assert(session.Refine("0") == SearchStart::Started);
    assert(Await(session) == SearchEnd::Done);
    assert(session.Hits() == zeros.size());
    assert(AllEntries(session) == zeros);

    // Memory the game gave up: its addresses go.
    {
        const u64 va = zeros.rbegin()->first;
        unmapped_granule = static_cast<size_t>(HostOfPage(all_spans, va >> 12) - g_base) / PF::GRANULE_SIZE;
        std::set<std::pair<u64, ValueKind>> mapped;
        for (const auto& hit : zeros) {
            if (static_cast<size_t>(HostOfPage(all_spans, hit.first >> 12) - g_base) / PF::GRANULE_SIZE !=
                unmapped_granule) {
                mapped.insert(hit);
            }
        }
        assert(mapped.size() < zeros.size());
        assert(session.Refine("0") == SearchStart::Started);
        assert(Await(session) == SearchEnd::Done);
        assert(AllEntries(session) == mapped);
        unmapped_granule = SIZE_MAX;
    }

    // Starting over forgets the list and its file.
    session.Clear();
    assert(!session.HasList());
    assert(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
    assert(session.Refine("0") == SearchStart::NoList);

    // A first search that finds nothing leaves nothing, the file included.
    assert(session.Search("987654", ValueKind::I32) == SearchStart::Started);
    assert(Await(session) == SearchEnd::NoMatches);
    assert(!session.HasList());
    assert(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    // A list of many chunks: 32 MiB of a byte, which is too many to browse. Each refined list goes
    // somewhere else, the old one still being read: a single overlap breaks a chunk or loses hits.
    constexpr u8 FILLER = 123;
    constexpr size_t FILLER_BYTES = 32 * MiB_;
    const size_t filler_start = BACKING / 2;
    std::memset(g_base + filler_start, FILLER, FILLER_BYTES);
    assert(session.Search("123", ValueKind::I8) == SearchStart::Started);
    assert(Await(session) == SearchEnd::Done);
    const u64 many = session.Hits();
    assert(many >= FILLER_BYTES && session.MustRefine());
    assert(session.ShownHits() == SHOWN_WHEN_TOO_MANY);
    assert(session.ReadEntries(0, SHOWN_WHEN_TOO_MANY, entries) && entries.size() == SHOWN_WHEN_TOO_MANY);
    for (int round = 0; round < 6; ++round) {
        assert(session.Refine("123") == SearchStart::Started);
        assert(Await(session) == SearchEnd::Done);
        assert(session.Hits() == many);
        assert(session.ReadEntries(many - 1, 2, entries) && entries.size() == 1);
    }

    // Stopped at once. A refined list stopped in memory is no list, so the last one stays: either it
    // did stop in time, or it ended first and holds the same.
    assert(session.Refine("123") == SearchStart::Started);
    session.Cancel();
    {
        const SearchEnd end = Await(session);
        assert(end == SearchEnd::Cancelled || end == SearchEnd::Partial || end == SearchEnd::Done);
        if (end == SearchEnd::Cancelled) assert(session.Hits() == many);
        assert(session.HasList());
    }
    // Waiting for it, and in the destructor: nothing is left running.
    assert(session.Refine("123") == SearchStart::Started);
    session.Stop();
    assert(!session.Running() && session.HasList());
    std::memset(g_base + filler_start, 0, FILLER_BYTES);
    session.Clear();

    // No room for a refined list: the failure is told and the list stays. Here the file holds about
    // the first list and little more.
    {
        Session tight{{path, g_decommit, walk, 3 * CHUNK_SIZE, 3 * CHUNK_SIZE}};
        assert(tight.Search(FIRST_NUMBER, ValueKind::I32) == SearchStart::Started);
        const SearchEnd end = Await(tight);
        assert(end == SearchEnd::Done || end == SearchEnd::TooManyResults);
        if (end == SearchEnd::Done) {
            const auto before = AllEntries(tight);
            assert(tight.Refine(FIRST_NUMBER) == SearchStart::Started);
            assert(Await(tight) == SearchEnd::TooManyResults);
            assert(AllEntries(tight) == before);
        } else {
            assert(!tight.HasList());
        }
    }
    // Closed with a search running.
    {
        Session closing{{path, g_decommit, walk}};
        assert(closing.Search(FIRST_NUMBER, ValueKind::I32) == SearchStart::Started);
    }
    assert(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
    std::printf("session: ok\n");
}

void TestScanner() {
    g_base = static_cast<u8*>(VirtualAlloc(nullptr, BACKING, MEM_RESERVE, PAGE_READWRITE));
    assert(g_base != nullptr);
    void* const handler = AddVectoredExceptionHandler(1, Handler);
    PF::Config config;
    config.path = TempFile(L"jme_gate_swap.bin");
    config.probation_ms = 20;
    config.min_idle_ms = 20;
    config.scout_ms = 10;
    config.batch_bytes = 0;
    assert(PF::Init(g_base, BACKING, PLATFORM, config));

    // A value of each kind on a page out of every 16, leaving the rest untouched or holding noise.
    constexpr u32 INT_VALUE = 0x1234ABCD;
    constexpr float FLOAT_VALUE = 7777.5f;
    constexpr double DOUBLE_VALUE = 7777.5;
    std::set<std::pair<u64, ValueKind>> planted;
    constexpr size_t GRANULES = BACKING / PF::GRANULE_SIZE;
    for (size_t granule = 0; granule < GRANULES; ++granule) {
        // A granule of every 4 is never touched: reserved pages must be skipped, not committed.
        if (granule % 4 == 3) continue;
        for (size_t in_granule = 0; in_granule < PF::GRANULE_PAGES; in_granule += 16) {
            const size_t page = granule * PF::GRANULE_PAGES + in_granule;
            u8* const host = g_base + page * PF::PAGE_SIZE;
            std::memcpy(host + 8, &INT_VALUE, sizeof(INT_VALUE));
            std::memcpy(host + 64, &DOUBLE_VALUE, sizeof(DOUBLE_VALUE));
            std::memcpy(host + 4092, &FLOAT_VALUE, sizeof(FLOAT_VALUE));
            planted.insert({VaOf(page) + 8, ValueKind::I32});
            planted.insert({VaOf(page) + 64, ValueKind::F64});
            planted.insert({VaOf(page) + 4092, ValueKind::F32});
        }
    }

    const auto spans = MakeSpans();
    QuerySet queries;
    queries.Add(MakeQuery(*ParseNumber("305441741"), ValueKind::I32)); // 0x1234ABCD
    queries.Add(MakeQuery(*ParseNumber("7777.5"), ValueKind::F32));
    queries.Add(MakeQuery(*ParseNumber("7777.5"), ValueKind::F64));

    const auto search = [&](u32 workers) {
        const PF::Stats before = PF::GetStats();
        ResultFile file;
        assert(file.Open(TempFile(L"jme_gate_results.bin"), 64 * MiB_));
        ResultList list;
        const ScanStats stats = RunScan({spans, queries, workers, g_decommit, file}, list);
        assert(stats.failure == ScanFailure::None && !stats.cancelled);
        AssertQuiet(before, PF::GetStats());
        const Found found = ReadList(file, list);
        // Each page once, and all of them.
        assert(found.size() == planted.size());
        assert((std::set<std::pair<u64, ValueKind>>(found.begin(), found.end()) == planted));
        return stats;
    };

    // All in memory: the disk stays out of it.
    const u64 committed_before = PF::GetStats().probe_arms;
    for (const u32 workers : {1u, 2u, 4u}) {
        const ScanStats stats = search(workers);
        assert(stats.disk_bytes == 0 && stats.sweep_bytes == 0);
        assert(stats.memory_bytes > 0);
    }
    assert(committed_before == PF::GetStats().probe_arms);

    // Probation and write-out until a good part is on disk, some of it read back.
    const auto evict = [&] {
        for (int i = 0; i < 400 && PF::GetStats().swapped_bytes < BACKING / 4; ++i) {
            PF::Step(BACKING, PF::Urgency::Emergency, g_decommit);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        PF::Settle();
    };
    evict();
    assert(PF::GetStats().swapped_bytes > 0);
    PF::Settle();
    {
        const ScanStats stats = search(4);
        assert(stats.disk_bytes > 0);
    }
    // Some pages back in memory, others still out: a granule split between both.
    u64 sum = 0;
    for (size_t granule = 0; granule < GRANULES; granule += 2) {
        if (granule % 4 == 3) continue;
        sum += *reinterpret_cast<volatile u32*>(g_base + granule * PF::GRANULE_SIZE + 8);
    }
    assert(sum != 0);
    PF::Settle();
    {
        const ScanStats stats = search(2);
        assert(stats.memory_bytes > 0 && stats.disk_bytes > 0);
    }
    // Stopped at once: no crash, whatever it reports, and no results promised.
    {
        ResultFile file;
        assert(file.Open(TempFile(L"jme_gate_results.bin"), 64 * MiB_));
        ResultList list;
        const std::atomic<bool> cancel{true};
        const ScanStats stats = RunScan({spans, queries, 4, g_decommit, file, &cancel}, list);
        assert(stats.cancelled && stats.memory_bytes == 0);
    }
    // No room for the list: it says so, it does not go to RAM.
    {
        ResultFile file;
        assert(file.Open(TempFile(L"jme_gate_results.bin"), CHUNK_SIZE));
        ResultList list;
        const ScanStats stats = RunScan({spans, queries, 2, g_decommit, file}, list);
        assert(stats.failure == ScanFailure::TooManyResults);
    }
    // A refined search stopped at once has seen nothing, and says memory was not done.
    {
        ResultFile file;
        assert(file.Open(TempFile(L"jme_gate_results.bin"), 64 * MiB_));
        ResultList list;
        assert(RunScan({spans, queries, 2, g_decommit, file}, list).failure == ScanFailure::None);
        const SlotRange used = file.ListRange();
        file.BeginList(NextListRange(used, file.MaxSlots()));
        ResultList refined;
        const std::atomic<bool> cancel{true};
        const ScanStats stats = RunRefine({spans, queries, 4, g_decommit, file, &cancel}, list, refined);
        assert(stats.cancelled && !stats.memory_complete && stats.TotalHits() == 0);
    }
    TestSession(spans, planted, evict);
    PF::Shutdown();
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(g_base, 0, MEM_RELEASE);
    std::printf("scanner: ok, %zu hits found in memory and on disk\n", planted.size());
}

void TestEncoding() {
    const auto encode = [](std::string_view text, ValueKind kind) {
        const auto number = ParseNumber(text);
        assert(number);
        return EncodeValue(*number, kind);
    };
    assert(encode("100", ValueKind::I32) == u64{100});
    assert(encode("-1", ValueKind::I8) == u64{0xFF});
    assert(encode("-1", ValueKind::I64) == ~u64{0});
    assert(!encode("300", ValueKind::I8));
    assert(!encode("-129", ValueKind::I8));
    assert(!encode("1.5", ValueKind::I32));
    // A decimal is written as typed, not as the range a search would take.
    assert(encode("1.5", ValueKind::F32) == u64{std::bit_cast<u32>(1.5f)});
    assert(encode("1.5", ValueKind::F64) == std::bit_cast<u64>(1.5));
    assert(encode("100", ValueKind::F32) == u64{std::bit_cast<u32>(100.0f)});
    // Not a constant: the compiler would refuse the overflow it sees coming.
    volatile double more_than_float_holds = 1e60;
    Number too_big_for_float;
    too_big_for_float.value = more_than_float_holds;
    assert(!EncodeValue(too_big_for_float, ValueKind::F32));

    assert(FormatValue(ValueKind::I32, 100, false) == "100");
    assert(FormatValue(ValueKind::I8, 0xFF, false) == "255");
    assert(FormatValue(ValueKind::I8, 0xFF, true) == "-1");
    assert(FormatValue(ValueKind::I16, 0xFFFE, true) == "-2");
    assert(FormatValue(ValueKind::I32, 0xFFFFFFFF, true) == "-1");
    assert(FormatValue(ValueKind::I64, ~u64{0}, true) == "-1");
    assert(FormatValue(ValueKind::I64, ~u64{0}, false) == "18446744073709551615");
    // What is above the kind's width is not the value.
    assert(FormatValue(ValueKind::I8, 0x1234, false) == "52");
    assert(FormatValue(ValueKind::F32, std::bit_cast<u32>(12.5f), false) == "12.5");
    assert(FormatValue(ValueKind::F64, std::bit_cast<u64>(-0.25), false) == "-0.25");
}

void TestPage() {
    using Jadyx::MenuAction;
    using enum MenuAction;
    const auto press = [](Page& page, MenuAction action, std::size_t rows = 0, bool has_list = false) {
        return HandlePageInput(page, action, rows, has_list);
    };

    // Every key is at most three presses from where the pad starts.
    for (std::size_t target = 0; target < PAD_KEYS.size(); ++target) {
        const std::size_t rows = (target / PAD_COLUMNS + PAD_ROWS - PAD_START / PAD_COLUMNS) % PAD_ROWS;
        const std::size_t columns = (target % PAD_COLUMNS + PAD_COLUMNS - PAD_START % PAD_COLUMNS) % PAD_COLUMNS;
        assert(std::min(rows, PAD_ROWS - rows) + std::min(columns, PAD_COLUMNS - columns) <= 3);
    }

    Page pad;
    assert(PAD_KEYS[pad.key] == '5');
    assert(press(pad, Up) == PageEffect::Redraw && PAD_KEYS[pad.key] == '8');
    assert(press(pad, Up) == PageEffect::Redraw && PAD_KEYS[pad.key] == '0');
    assert(press(pad, Down) == PageEffect::Redraw && PAD_KEYS[pad.key] == '8');
    pad.key = PAD_START;
    press(pad, Left);
    assert(PAD_KEYS[pad.key] == '4');
    press(pad, Left);
    assert(PAD_KEYS[pad.key] == '6');
    press(pad, Right);
    assert(PAD_KEYS[pad.key] == '4');

    // Typing, with one point at most and the sign always in front.
    assert(press(pad, Refresh) == PageEffect::None);
    assert(press(pad, Alternate) == PageEffect::None);
    PressKey(pad, '1');
    PressKey(pad, '.');
    PressKey(pad, '.');
    PressKey(pad, '5');
    assert(pad.text == "1.5");
    PressKey(pad, '-');
    assert(pad.text == "-1.5");
    PressKey(pad, '-');
    assert(pad.text == "1.5");
    assert(press(pad, Alternate) == PageEffect::Redraw && pad.text == "1.");
    pad.key = PAD_START;
    assert(press(pad, Confirm) == PageEffect::Redraw && pad.text == "1.5");
    assert(press(pad, Refresh) == PageEffect::Start);
    pad.text.assign(MAX_TYPED, '1');
    PressKey(pad, '2');
    PressKey(pad, '-');
    assert(pad.text.size() == MAX_TYPED && pad.text.back() == '1' && pad.text.front() == '1');

    // The kind goes round for a new search only.
    Page kinds;
    assert(!ChosenKind(kinds));
    press(kinds, NextTab);
    assert(ChosenKind(kinds) == ValueKind::I8);
    press(kinds, PreviousTab);
    assert(!ChosenKind(kinds));
    press(kinds, PreviousTab);
    assert(ChosenKind(kinds) == ValueKind::F64);
    press(kinds, NextTab);
    assert(!ChosenKind(kinds));
    EnterNumber(kinds, EntryPurpose::Refine);
    assert(press(kinds, NextTab) == PageEffect::None && !ChosenKind(kinds));
    EnterNumber(kinds, EntryPurpose::Edit);
    assert(press(kinds, PreviousTab) == PageEffect::None && !ChosenKind(kinds));

    // B leaves the pad for the list if there is one, or for the menu.
    Page back;
    assert(press(back, Back, 0, false) == PageEffect::Leave);
    assert(press(back, Back, 3, true) == PageEffect::Redraw && back.mode == PageMode::List);

    // The list.
    Page list;
    list.mode = PageMode::List;
    constexpr std::size_t ROWS = 12;
    assert(press(list, Up, ROWS, true) == PageEffect::Redraw && list.selected == ROWS - 1);
    assert(list.first + LIST_ROWS == ROWS);
    assert(press(list, Down, ROWS, true) == PageEffect::Redraw && list.selected == 0 && list.first == 0);
    assert(press(list, NextTab, ROWS, true) == PageEffect::Redraw && list.selected == LIST_ROWS);
    assert(list.selected >= list.first && list.selected < list.first + LIST_ROWS);
    assert(press(list, NextTab, ROWS, true) == PageEffect::Redraw && list.selected == 2 * LIST_ROWS);
    assert(press(list, NextTab, ROWS, true) == PageEffect::Redraw && list.selected == ROWS - 1);
    assert(press(list, NextTab, ROWS, true) == PageEffect::None);
    assert(press(list, PreviousTab, ROWS, true) == PageEffect::Redraw && list.selected == ROWS - 1 - LIST_ROWS);
    assert(press(list, PreviousTab, ROWS, true) == PageEffect::Redraw &&
           press(list, PreviousTab, ROWS, true) == PageEffect::Redraw);
    assert(list.selected == 0 && list.first == 0);
    assert(press(list, PreviousTab, ROWS, true) == PageEffect::None);
    assert(press(list, Confirm, ROWS, true) == PageEffect::Pick);
    assert(press(list, Confirm, 0, false) == PageEffect::None);
    // Left and right mark the selected result; the screen does the marking.
    assert(press(list, Right, ROWS, true) == PageEffect::Mark && list.selected == 0);
    assert(press(list, Left, ROWS, true) == PageEffect::Mark);
    assert(press(list, Left, 0, false) == PageEffect::None);
    {
        Page marks;
        const Marked a{0x1000, ValueKind::I32};
        const Marked b{0x2000, ValueKind::I32};
        const Marked a_float{0x1000, ValueKind::F32};
        assert(!IsMarked(marks, a));
        ToggleMark(marks, a);
        ToggleMark(marks, b);
        assert(IsMarked(marks, a) && IsMarked(marks, b) && !IsMarked(marks, a_float) && marks.marked.size() == 2);
        ToggleMark(marks, a);
        assert(!IsMarked(marks, a) && IsMarked(marks, b) && marks.marked.size() == 1);
    }
    Page one;
    one.mode = PageMode::List;
    assert(press(one, Up, 1, true) == PageEffect::None && press(one, Down, 1, true) == PageEffect::None);
    assert(press(list, Refresh, ROWS, true) == PageEffect::Redraw);
    assert(list.mode == PageMode::Entry && list.purpose == EntryPurpose::Refine && list.text.empty());
    // Refining starts on the number the list came from.
    list.mode = PageMode::List;
    list.searched = "5";
    list.text = "123";
    assert(press(list, Refresh, ROWS, true) == PageEffect::Redraw);
    assert(list.mode == PageMode::Entry && list.purpose == EntryPurpose::Refine && list.text == "5");
    list.mode = PageMode::List;
    assert(press(list, Alternate, ROWS, true) == PageEffect::Redraw);
    assert(list.mode == PageMode::Entry && list.purpose == EntryPurpose::Search);
    list.mode = PageMode::List;
    assert(press(list, Back, ROWS, true) == PageEffect::Leave);

    // The warning and a running search only know their own buttons.
    Page warning;
    warning.mode = PageMode::Warning;
    assert(press(warning, Up) == PageEffect::None && press(warning, Refresh) == PageEffect::None);
    assert(press(warning, Confirm) == PageEffect::Accept);
    assert(press(warning, Back) == PageEffect::Leave);
    Page running;
    running.mode = PageMode::Searching;
    assert(press(running, Confirm) == PageEffect::None && press(running, Refresh) == PageEffect::None);
    assert(press(running, Back) == PageEffect::Stop);

    // Menu puts the page away in any mode, still paused; only Menu and B bring it back.
    for (const PageMode mode : {PageMode::Warning, PageMode::Entry, PageMode::Searching, PageMode::List}) {
        Page away;
        away.mode = mode;
        assert(press(away, Menu, ROWS, true) == PageEffect::Redraw && away.hidden);
        for (const MenuAction other : {Up, Down, Left, Right, Confirm, Alternate, Refresh, PreviousTab, NextTab})
            assert(press(away, other, ROWS, true) == PageEffect::None);
        assert(away.mode == mode && away.text.empty() && away.selected == 0);
        assert(press(away, Menu, ROWS, true) == PageEffect::Redraw && !away.hidden);
        assert(press(away, Menu, ROWS, true) == PageEffect::Redraw && away.hidden);
        assert(press(away, Back, ROWS, true) == PageEffect::Redraw && !away.hidden && away.mode == mode);
    }
}

} // namespace

int main() {
    TestValues();
    TestEncoding();
    TestPage();
    TestMatcher();
    TestCodec();
    TestFile();
    TestScanner();
    std::printf("memory edit gates passed\n");
    return 0;
}
