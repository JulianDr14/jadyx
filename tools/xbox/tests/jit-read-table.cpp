// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// JIT load table gate: with page_table_read_offset set, guest loads look up the second half of
// the page table and stores keep the first half. Runs the production UWP Dynarmic library with
// the production page table settings, so A64 plain loads go through the page table thunks and
// ordered and exclusive loads through the inline lookup. Build like jit-prewarm.cpp.

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include "common/logging.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"

// Standalone harness links the production UWP Dynarmic library. Fail assertions
// hard; it does not need the frontend logging service or a kernel instance.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*,
                       fmt::string_view, const fmt::format_args&) {}
}

namespace {

constexpr std::uint64_t CodeBase = 0x1000;
constexpr std::uint64_t DataBase = 0x10000;
constexpr std::uint64_t DataPage = DataBase >> 12;

// One guest data page, backed by host memory both by the table and by the callbacks.
struct Page {
    alignas(4096) std::array<std::uint8_t, 4096> bytes{};
    std::uint64_t Load(std::uint64_t vaddr, std::size_t size) const {
        assert(vaddr >= DataBase && vaddr - DataBase + size <= bytes.size());
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data() + (vaddr - DataBase), size);
        return value;
    }
    void Store(std::uint64_t vaddr, std::uint64_t value, std::size_t size) {
        assert(vaddr >= DataBase && vaddr - DataBase + size <= bytes.size());
        std::memcpy(bytes.data() + (vaddr - DataBase), &value, size);
    }
    // Absolute offset entry: the JIT adds the full guest address.
    void* Entry() { return bytes.data() - DataBase; }
};

// Which halves of the page table map the data page.
struct Tables {
    bool store{}, load{};
};

struct Counts {
    unsigned reads{}, writes{};
};

// --- A64 -----------------------------------------------------------------------------------------

constexpr std::uint32_t A64Bits = 24;
constexpr std::size_t A64Entries = std::size_t{1} << (A64Bits - 12);

struct A64Callbacks final : Dynarmic::A64::UserCallbacks {
    Dynarmic::A64::Jit* jit{};
    Page* page{};
    Counts counts;
    std::vector<std::uint32_t> code{
        0xf9400001, // LDR X1,[X0]       page table thunk
        0x39400005, // LDRB W5,[X0]      page table thunk, 8-bit
        0x3dc00000, // LDR Q0,[X0]       page table thunk, 128-bit
        0xc8dffc03, // LDAR X3,[X0]      inline lookup, ordered
        0xc85f7c04, // LDXR X4,[X0]      inline lookup, exclusive
        0xd5033f5f, // CLREX
        0xf9000041, // STR X1,[X2]       page table thunk, store
        0xc89ffc41, // STLR X1,[X2]      inline lookup, ordered store
        0xd4000001, // SVC #0
    };

    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t addr) override {
        if (addr >= CodeBase && addr - CodeBase < code.size() * 4 && !(addr & 3)) {
            return code[(addr - CodeBase) / 4];
        }
        return std::nullopt;
    }
    std::uint64_t MemoryRead(std::uint64_t vaddr, std::size_t size) override {
        ++counts.reads;
        return page->Load(vaddr, size);
    }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t vaddr) override {
        ++counts.reads;
        return {page->Load(vaddr, 8), page->Load(vaddr + 8, 8)};
    }
    void MemoryWrite(std::uint64_t vaddr, std::uint64_t value, std::size_t size) override {
        ++counts.writes;
        page->Store(vaddr, value, size);
    }
    void MemoryWrite128(std::uint64_t vaddr, Dynarmic::A64::Vector value) override {
        ++counts.writes;
        page->Store(vaddr, value[0], 8);
        page->Store(vaddr + 8, value[1], 8);
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { std::abort(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

// Loads: LDR, LDRB, LDR Q, LDAR, LDXR. Stores: STR, STLR.
constexpr unsigned A64Loads = 5;
constexpr unsigned A64Stores = 2;

Counts RunA64(Tables tables, bool read_offset) {
    Page page;
    for (std::size_t i = 0; i < page.bytes.size(); ++i) page.bytes[i] = std::uint8_t(i * 7 + 3);
    const std::uint64_t word = page.Load(DataBase, 8);
    const std::uint64_t high = page.Load(DataBase + 8, 8);

    std::vector<void*> table(2 * A64Entries, nullptr);
    if (tables.store) table[DataPage] = page.Entry();
    if (tables.load) table[A64Entries + DataPage] = page.Entry();

    Dynarmic::ExclusiveMonitor monitor{1};
    A64Callbacks cb;
    cb.page = &page;
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &cb;
    config.global_monitor = &monitor;
    config.code_cache_size = 8 * 1024 * 1024;
    // Production settings (arm_dynarmic_64.cpp), which enable the page table thunks.
    config.page_table = table.data();
    config.page_table_address_space_bits = A64Bits;
    config.page_table_pointer_mask = 0;
    config.page_table_marked_bit = std::nullopt;
    config.page_table_read_offset = read_offset ? std::int32_t(A64Entries * sizeof(void*)) : 0;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    config.page_table_exclusive_access = true;

    Dynarmic::A64::Jit jit{config};
    cb.jit = &jit;
    jit.SetPC(CodeBase);
    jit.SetRegister(0, DataBase);
    jit.SetRegister(2, DataBase + 0x100);
    jit.Run();

    assert(jit.GetRegister(1) == word && jit.GetRegister(3) == word && jit.GetRegister(4) == word);
    assert(jit.GetRegister(5) == (word & 0xff));
    const auto q = jit.GetVector(0);
    assert(q[0] == word && q[1] == high);
    assert(page.Load(DataBase + 0x100, 8) == word); // Both stores landed, inline or by callback.
    return cb.counts;
}

// --- A32 -----------------------------------------------------------------------------------------

constexpr std::size_t A32Entries = Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES;

struct A32Callbacks final : Dynarmic::A32::UserCallbacks {
    Dynarmic::A32::Jit* jit{};
    Page* page{};
    Counts counts;
    std::vector<std::uint32_t> code{
        0xe5901000, // LDR R1,[R0]
        0xe1904c9f, // LDA R4,[R0]       ordered
        0xe1903f9f, // LDREX R3,[R0]     exclusive
        0xf57ff01f, // CLREX
        0xe5821000, // STR R1,[R2]
        0xef000000, // SVC #0
    };

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t addr) override {
        if (addr >= CodeBase && addr - CodeBase < code.size() * 4 && !(addr & 3)) {
            return code[(addr - CodeBase) / 4];
        }
        return std::nullopt;
    }
    std::uint64_t MemoryRead(std::uint32_t vaddr, std::size_t size) override {
        ++counts.reads;
        return page->Load(vaddr, size);
    }
    void MemoryWrite(std::uint32_t vaddr, std::uint64_t value, std::size_t size) override {
        ++counts.writes;
        page->Store(vaddr, value, size);
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override { std::abort(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
};

constexpr unsigned A32Loads = 3;
constexpr unsigned A32Stores = 1;

Counts RunA32(Tables tables, bool read_offset) {
    Page page;
    for (std::size_t i = 0; i < page.bytes.size(); ++i) page.bytes[i] = std::uint8_t(i * 5 + 1);
    const std::uint32_t word = std::uint32_t(page.Load(DataBase, 4));

    using Table = std::array<std::uint8_t*, A32Entries>;
    std::vector<Table> table(2); // Store table, then load table, contiguous.
    auto* const entry = static_cast<std::uint8_t*>(page.Entry());
    if (tables.store) table[0][DataPage] = entry;
    if (tables.load) table[1][DataPage] = entry;

    Dynarmic::ExclusiveMonitor monitor{1};
    A32Callbacks cb;
    cb.page = &page;
    Dynarmic::A32::UserConfig config{};
    config.callbacks = &cb;
    config.global_monitor = &monitor;
    config.code_cache_size = 8 * 1024 * 1024;
    config.page_table = &table[0];
    config.page_table_pointer_mask = 0;
    config.page_table_marked_bit = std::nullopt;
    config.page_table_read_offset = read_offset ? std::int32_t(sizeof(Table)) : 0;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    config.page_table_exclusive_access = true;

    Dynarmic::A32::Jit jit{config};
    cb.jit = &jit;
    jit.Regs()[0] = std::uint32_t(DataBase);
    jit.Regs()[2] = std::uint32_t(DataBase + 0x100);
    jit.Regs()[15] = std::uint32_t(CodeBase);
    jit.Run();

    assert(jit.Regs()[1] == word && jit.Regs()[3] == word && jit.Regs()[4] == word);
    assert(std::uint32_t(page.Load(DataBase + 0x100, 4)) == word);
    return cb.counts;
}

template <typename Run>
void Check(const char* arch, Run run, unsigned loads, unsigned stores) {
    // No read offset: the first half serves both, as before the load table existed.
    auto c = run({.store = true}, false);
    assert(c.reads == 0 && c.writes == 0);
    c = run({.load = true}, false);
    assert(c.reads == loads && c.writes == stores);
    // Read offset: loads use only the second half, stores only the first.
    c = run({.load = true}, true);
    assert(c.reads == 0 && c.writes == stores);
    c = run({.store = true}, true);
    assert(c.reads == loads && c.writes == 0);
    c = run({.store = true, .load = true}, true);
    assert(c.reads == 0 && c.writes == 0);
    std::cout << "PASS " << arch
              << ": loads read the load table and stores the store table, inline and through "
                 "the page table thunks\n";
}

} // namespace

int main() {
    Check("A64", RunA64, A64Loads, A64Stores);
    Check("A32", RunA32, A32Loads, A32Stores);
    return 0;
}
