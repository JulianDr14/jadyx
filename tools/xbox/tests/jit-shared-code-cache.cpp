// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared JIT code cache gate (jit_shared_cache): the A64 cores of a process run one code cache.
// Links the production UWP Dynarmic library. Checks, in order:
//  - the pieces on their own: link-site layout, quiescence gate, invalidation journal,
//    fast-dispatch table, config compatibility and cache sizing;
//  - four real JITs on one cache: a block compiles once and runs on every core, each core keeps
//    its own state (callbacks, TPIDR, exclusive monitor slot), and an invalidation or clear made
//    by one core reaches the others' links, fast-dispatch tables and return stacks;
//  - the same under threads, with the cache too small for the program, so it clears while cores
//    run, and with invalidations and clears requested from another thread meanwhile.
// Run with run-jit-shared-code-cache.ps1.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "dynarmic/backend/x64/a64_code_cache.h"
#include "dynarmic/backend/x64/fast_dispatch_table.h"
#include "dynarmic/backend/x64/in_flight_compiles.h"
#include "dynarmic/backend/x64/invalidation_journal.h"
#include "dynarmic/backend/x64/link_site.h"
#include "dynarmic/backend/x64/quiescence_gate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/shared_code_cache.h"
#include "dynarmic/interface/code_memory.h"
#include "dynarmic/interface/exclusive_monitor.h"

// Standalone harness: fail assertions hard, saying where; no frontend logging service.
[[noreturn]] static void Die(const char* what, const char* where) {
    std::fprintf(stderr, "FAIL %s: %s\n", what, where ? where : "");
    std::fflush(stderr);
    std::_Exit(3);  // abort()'s exit code reads as success to "if errorlevel 1".
}
void AssertFatalImpl() { Die("assert", "fatal"); }
void AssertFailSoftImpl() { Die("assert", "soft"); }
void AssertFailedAt(const char* what) { Die("assert", what); }
void UnreachableAt(const char* where) { Die("unreachable", where); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*,
                       fmt::string_view, const fmt::format_args&) {}
}

namespace {

using namespace Dynarmic;
using Backend::X64::FastDispatchTable;
using Backend::X64::InFlightCompiles;
using Backend::X64::InvalidationJournal;
using Backend::X64::QuiescenceGate;
namespace LinkSite = Backend::X64::LinkSite;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- Harness -------------------------------------------------------------------------------------

[[noreturn]] void Fail(const char* test, const char* what, int line) {
    std::fprintf(stderr, "FAIL %s: %s (line %d)\n", test, what, line);
    std::fflush(stderr);
    std::_Exit(1);  // abort()'s exit code reads as success to "if errorlevel 1".
}
#define REQUIRE(cond) \
    do { \
        if (!(cond)) Fail(current_test, #cond, __LINE__); \
    } while (0)

const char* current_test = "";

/// A deadlock shows up as a gate that never returns: fail it instead.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit)
        : thread{[this, limit] {
            std::unique_lock lock{mutex};
            if (!done_cv.wait_for(lock, limit, [this] { return done; })) {
                std::fprintf(stderr, "FAIL %s: no progress in %lld s (deadlock?)\n", current_test,
                             static_cast<long long>(limit.count()));
                std::fflush(stderr);
                std::_Exit(2);
            }
        }} {}
    ~Watchdog() {
        {
            std::scoped_lock lock{mutex};
            done = true;
        }
        done_cv.notify_all();
        thread.join();
    }

private:
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
    std::thread thread;
};

void Run(const char* name, void (*test)()) {
    current_test = name;
    std::printf("RUN  %s\n", name);
    std::fflush(stdout);
    const auto start = std::chrono::steady_clock::now();
    {
        const Watchdog watchdog{std::chrono::seconds{180}};
        test();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("PASS %s (%lld ms)\n", name, static_cast<long long>(ms.count()));
    std::fflush(stdout);
}

// --- A64 encodings ---------------------------------------------------------------------------------

namespace Asm {
constexpr u32 Movz(u32 rd, u32 imm16) { return 0xD2800000u | (imm16 << 5) | rd; }
constexpr u32 AddImm(u32 rd, u32 rn, u32 imm12) { return 0x91000000u | (imm12 << 10) | (rn << 5) | rd; }
constexpr u32 AddReg(u32 rd, u32 rn, u32 rm) { return 0x8B000000u | (rm << 16) | (rn << 5) | rd; }
constexpr u32 EorRor(u32 rd, u32 rn, u32 rm, u32 shift) {
    return 0xCAC00000u | (rm << 16) | (shift << 10) | (rn << 5) | rd;
}
constexpr u32 B(u64 from, u64 to) { return 0x14000000u | (static_cast<u32>((to - from) >> 2) & 0x3FFFFFFu); }
constexpr u32 Bl(u64 from, u64 to) { return 0x94000000u | (static_cast<u32>((to - from) >> 2) & 0x3FFFFFFu); }
constexpr u32 Adr(u32 rd, u64 from, u64 to) {
    const u64 offset = to - from;
    return 0x10000000u | ((static_cast<u32>(offset) & 3u) << 29) | ((static_cast<u32>(offset >> 2) & 0x7FFFFu) << 5) | rd;
}
constexpr u32 Br(u32 rn) { return 0xD61F0000u | (rn << 5); }
constexpr u32 Ret = 0xD65F03C0u;
constexpr u32 Svc(u32 imm16) { return 0xD4000001u | (imm16 << 5); }
constexpr u32 MrsTpidr(u32 rt) { return 0xD53BD040u | rt; }
constexpr u32 MrsTpidrro(u32 rt) { return 0xD53BD060u | rt; }
constexpr u32 MsrTpidr(u32 rt) { return 0xD51BD040u | rt; }
constexpr u32 Ldxr(u32 rt, u32 rn) { return 0xC85F7C00u | (rn << 5) | rt; }
constexpr u32 Stxr(u32 rs, u32 rt, u32 rn) { return 0xC8007C00u | (rs << 16) | (rn << 5) | rt; }
}  // namespace Asm

// --- Guest machine ---------------------------------------------------------------------------------

constexpr u32 AddressBits = 24;
constexpr std::size_t PageEntries = std::size_t{1} << (AddressBits - 12);
constexpr u64 DataBase = 0x10000;
constexpr u64 KernelBase = 0x20000;
constexpr u64 ChainBase = 0x100000;
constexpr std::size_t Cores = 4;

/// Guest code, read by every core through MemoryReadCode. Patched only while no core runs.
struct GuestCode {
    std::vector<std::pair<u64, std::vector<u32>>> segments;

    void Add(u64 base, std::vector<u32> words) { segments.emplace_back(base, std::move(words)); }
    u32& At(u64 vaddr) {
        for (auto& [base, words] : segments) {
            if (vaddr >= base && vaddr - base < words.size() * 4) return words[(vaddr - base) / 4];
        }
        Die("harness", "patching a word outside the guest code");
    }
    std::optional<u32> Read(u64 vaddr) const {
        for (const auto& [base, words] : segments) {
            if (vaddr >= base && vaddr - base < words.size() * 4 && !(vaddr & 3)) return words[(vaddr - base) / 4];
        }
        return std::nullopt;
    }
};

/// One data page behind the page table (absolute offset entries).
struct DataPage {
    alignas(4096) std::array<std::uint8_t, 4096> bytes{};
    u64 Load(u64 vaddr) const {
        u64 value;
        std::memcpy(&value, bytes.data() + (vaddr - DataBase), 8);
        return value;
    }
    void Store(u64 vaddr, u64 value) { std::memcpy(bytes.data() + (vaddr - DataBase), &value, 8); }
};

struct Machine {
    GuestCode code;
    DataPage data;
    std::vector<void*> page_table = std::vector<void*>(PageEntries, nullptr);
    ExclusiveMonitor monitor{Cores};

    Machine() { page_table[DataBase >> 12] = data.bytes.data() - DataBase; }
};

struct Callbacks final : A64::UserCallbacks {
    Machine* machine{};
    A64::Jit* jit{};
    u64 tpidr = 0;
    u64 tpidrro = 0;
    u64 svcs = 0;
    u64 tick_calls = 0;

    std::optional<u32> MemoryReadCode(u64 vaddr) override { return machine->code.Read(vaddr); }
    // The page table maps the data page: these run only on a miss, which the tests never make.
    u64 MemoryRead(u64, std::size_t) override { Die("callback", "MemoryRead"); }
    A64::Vector MemoryRead128(u64) override { Die("callback", "MemoryRead128"); }
    void MemoryWrite(u64, u64, std::size_t) override { Die("callback", "MemoryWrite"); }
    void MemoryWrite128(u64, A64::Vector) override { Die("callback", "MemoryWrite128"); }
    void CallSVC(u32 swi) override {
        if (swi != 0) Die("callback", "SVC #1: a trap the program must never reach");
        ++svcs;
        jit->HaltExecution(HaltReason::UserDefined1);
    }
    void ExceptionRaised(u64 pc, A64::Exception exception) override {
        std::fprintf(stderr, "exception %d at 0x%llx\n", static_cast<int>(exception), static_cast<unsigned long long>(pc));
        Die("callback", "ExceptionRaised");
    }
    void AddTicks(u64) override { ++tick_calls; }
    u64 GetTicksRemaining() override { return 1'000'000'000; }
    u64 GetCNTPCT() override { return 0; }
};

/// The production configuration (arm_dynarmic_64.cpp) without fastmem.
A64::UserConfig MakeConfig(Machine& machine, Callbacks& cb, std::size_t core,
                           std::shared_ptr<A64::SharedCodeCache> shared, u32 cache_bytes) {
    A64::UserConfig config{};
    config.callbacks = &cb;
    config.page_table = machine.page_table.data();
    config.page_table_address_space_bits = AddressBits;
    config.page_table_pointer_mask = 0;
    config.page_table_marked_bit = std::nullopt;
    config.page_table_thunks = true;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    config.page_table_exclusive_access = true;
    config.processor_id = static_cast<std::uint8_t>(core);
    config.global_monitor = &machine.monitor;
    config.tpidr_el0 = &cb.tpidr;
    config.tpidrro_el0 = &cb.tpidrro;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.define_unpredictable_behaviour = true;
    config.code_cache_size = cache_bytes;
    config.shared_code_cache = std::move(shared);
    return config;
}

/// Runs until the program's SVC #0, through any other halt (invalidations, clears).
void RunToSvc(A64::Jit& jit, Callbacks& cb) {
    const u64 svcs = cb.svcs;
    for (u64 runs = 0; cb.svcs == svcs; ++runs) {
        if (runs > 10'000'000) Fail(current_test, "the program never reached its SVC", __LINE__);
        jit.Run();
    }
    jit.ClearHalt(HaltReason::UserDefined1);
}

const A64::SharedCodeCache::Stats& StatsOf(const std::shared_ptr<A64::SharedCodeCache>& handle) {
    return handle->GetStats();
}
u64 Compiled(const std::shared_ptr<A64::SharedCodeCache>& handle) {
    return StatsOf(handle).blocks_compiled.load();
}

// --- Kernel: one small program touching every per-core binding -----------------------------------

namespace Kernel {
constexpr u64 At(u64 index) { return KernelBase + index * 4; }
// Patchable words, each in its own block and reached a different way.
constexpr u64 EntryWord = At(0);   // MOVZ X10: the block a Run starts at (cache lookup)
constexpr u64 LinkedWord = At(9);  // MOVZ X13: reached by a direct, linked branch
constexpr u64 IndirectWord = At(12);  // MOVZ X12: reached by BR (fast dispatch)

std::vector<u32> Program(u32 entry, u32 linked, u32 indirect) {
    return {
        Asm::Movz(10, entry),             // 0
        Asm::MrsTpidr(5),                 // 1  callbacks' TPIDR_EL0
        Asm::MrsTpidrro(6),               // 2  callbacks' TPIDRRO_EL0
        Asm::MsrTpidr(7),                 // 3
        Asm::Ldxr(3, 0),                  // 4  this core's monitor slot
        Asm::Stxr(4, 1, 0),               // 5
        Asm::Bl(At(6), At(14)),           // 6  return comes back through the RSB
        Asm::B(At(7), At(9)),             // 7  linked branch
        Asm::Svc(1),                      // 8  trap
        Asm::Movz(13, linked),            // 9
        Asm::Adr(9, At(10), At(12)),      // 10
        Asm::Br(9),                       // 11 fast dispatch
        Asm::Movz(12, indirect),          // 12
        Asm::Svc(0),                      // 13
        Asm::AddReg(11, 10, 2),           // 14
        Asm::Ret,                         // 15
    };
}

struct Values {
    u32 entry = 0x1111, linked = 0x2222, indirect = 0x3333;
};

/// Runs the kernel on `core` and checks everything it computed, against `values` in the code.
void RunAndCheck(Machine& machine, A64::Jit& jit, Callbacks& cb, std::size_t core, const Values& values) {
    const u64 slot = DataBase + core * 0x40;
    const u64 old_value = 0x5000 + core * 0x10 + cb.svcs;
    const u64 new_value = 0xA000 + core * 0x10 + cb.svcs;
    const u64 tpidr_before = cb.tpidr;
    const u64 ticks_before = cb.tick_calls;
    machine.data.Store(slot, old_value);
    jit.SetPC(KernelBase);
    jit.SetRegister(0, slot);
    jit.SetRegister(1, new_value);
    jit.SetRegister(2, 0x100 * (core + 1));
    jit.SetRegister(7, 0x7000 + core * 0x10 + cb.svcs);
    const u64 tpidr_after = jit.GetRegister(7);
    RunToSvc(jit, cb);

    REQUIRE(jit.GetRegister(3) == old_value);     // LDXR through this core's binding
    REQUIRE(jit.GetRegister(4) == 0);             // STXR found this core's reservation
    REQUIRE(machine.data.Load(slot) == new_value);
    REQUIRE(jit.GetRegister(5) == tpidr_before);  // MRS read this core's callbacks
    REQUIRE(jit.GetRegister(6) == cb.tpidrro);
    REQUIRE(cb.tpidr == tpidr_after);             // MSR wrote this core's callbacks
    REQUIRE(jit.GetRegister(11) == values.entry + 0x100 * (core + 1));
    REQUIRE(jit.GetRegister(13) == values.linked);
    REQUIRE(jit.GetRegister(12) == values.indirect);
    REQUIRE(cb.tick_calls > ticks_before);        // AddTicks called on this core's callbacks
}
}  // namespace Kernel

/// Four cores on one machine, sharing `handle` (or not, when null).
struct Cluster {
    Machine machine;
    std::array<Callbacks, Cores> callbacks;
    std::vector<std::unique_ptr<A64::Jit>> jits;
    std::shared_ptr<A64::SharedCodeCache> handle;
    Kernel::Values values;

    explicit Cluster(u32 cache_bytes = 16 * 1024 * 1024, bool shared = true) {
        if (shared) handle = std::make_shared<A64::SharedCodeCache>(Cores);
        machine.code.Add(KernelBase, Kernel::Program(values.entry, values.linked, values.indirect));
        for (std::size_t core = 0; core < Cores; ++core) {
            Callbacks& cb = callbacks[core];
            cb.machine = &machine;
            cb.tpidr = 0x1000 + core;
            cb.tpidrro = 0x2000 + core;
            jits.push_back(std::make_unique<A64::Jit>(MakeConfig(machine, cb, core, handle, cache_bytes)));
            cb.jit = jits.back().get();
        }
    }

    void RunKernel(std::size_t core) {
        Kernel::RunAndCheck(machine, *jits[core], callbacks[core], core, values);
    }
    void RunKernelEverywhere(std::size_t first = 0) {
        for (std::size_t i = 0; i < Cores; ++i) RunKernel((first + i) % Cores);
    }
};

// --- Unit tests ------------------------------------------------------------------------------------

void TestLinkSites() {
    for (const LinkSite::Encoding enc : {LinkSite::jmp_rel32, LinkSite::jcc_rel32, LinkSite::mov_rcx_imm64}) {
        for (std::uintptr_t site = 0x10000; site < 0x10040; ++site) {
            const std::uintptr_t field = LinkSite::FieldAddress(site, enc);
            REQUIRE(field % enc.field_size == 0);                           // one aligned store
            REQUIRE(field / 64 == (field + enc.field_size - 1) / 64);       // one cache line
            REQUIRE(LinkSite::PaddingFor(site, enc) < enc.field_size);
            REQUIRE(LinkSite::InstructionEnd(site, enc) == field + enc.field_size);
        }
    }
    REQUIRE(LinkSite::Rel32(0x1000, 0x1000ull + 0x7FFFFFFF) == 0x7FFFFFFF);
    REQUIRE(!LinkSite::Rel32(0x1000, 0x1000 + 0x80000000ull));
    REQUIRE(!LinkSite::Rel32(0x80001000ull + 1, 0x1000));
}

void TestGateSingleThread() {
    int clears = 0;
    QuiescenceGate gate{[&] { ++clears; }};
    // Nobody inside: the clear runs on the caller.
    REQUIRE(gate.RequestClear());
    REQUIRE(clears == 1 && gate.Generation() == 1 && !gate.ClearPending());
    // Somebody inside: the clear waits for the last one out.
    REQUIRE(gate.Enter() == 1);
    gate.Enter();
    REQUIRE(gate.RequestClear());
    REQUIRE(!gate.RequestClear());  // Already pending: one clear serves both.
    REQUIRE(clears == 1 && gate.ClearPending());
    gate.Exit();
    REQUIRE(clears == 1);
    gate.Exit();
    REQUIRE(clears == 2 && gate.Generation() == 2 && gate.Active() == 0 && !gate.ClearPending());
}

void TestGateThreads() {
    // Workers flag themselves while "running code"; the clear must never see one.
    std::atomic<int> inside{0};
    std::atomic<u64> clears{0};
    std::atomic<bool> violated{false};
    QuiescenceGate gate{[&] {
        if (inside.load() != 0) violated = true;
        clears.fetch_add(1);
    }};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng{static_cast<unsigned>(t)};
            for (int i = 0; i < 20000; ++i) {
                const u64 generation = gate.Enter();
                inside.fetch_add(1);
                if (gate.Generation() != generation) violated = true;  // cleared while inside
                if (rng() % 64 == 0) gate.RequestClear();  // like a core out of room
                inside.fetch_sub(1);
                gate.Exit();
            }
        });
    }
    std::thread requester{[&] {
        while (!stop) {
            gate.RequestClear();
            std::this_thread::yield();
        }
    }};
    for (auto& worker : workers) worker.join();
    stop = true;
    requester.join();
    REQUIRE(!violated);
    REQUIRE(clears.load() > 0);
    REQUIRE(gate.Active() == 0);
}

void TestInFlightCompiles() {
    InFlightCompiles in_flight;
    REQUIRE(InFlightCompiles::Claim::Exclusive().Owned());
    {
        const auto first = in_flight.Acquire(0x1000);
        REQUIRE(first.Owned());
        REQUIRE(in_flight.Size() == 1);
        const auto other_key = in_flight.Acquire(0x2000);  // other blocks never wait
        REQUIRE(other_key.Owned());
    }
    REQUIRE(in_flight.Size() == 0);

    // A second core missing the same block waits for the compile, then gets an unowned claim.
    std::atomic<bool> released{false};
    std::atomic<bool> waiter_owned{true};
    std::atomic<bool> waited_for_release{false};
    std::optional<InFlightCompiles::Claim> holder{in_flight.Acquire(0x3000)};
    std::thread waiter{[&] {
        const auto claim = in_flight.Acquire(0x3000);
        waited_for_release = released.load();
        waiter_owned = claim.Owned();
    }};
    while (in_flight.Waits() == 0) std::this_thread::yield();
    released = true;
    holder.reset();
    waiter.join();
    REQUIRE(waited_for_release);
    REQUIRE(!waiter_owned);
    REQUIRE(in_flight.Waits() == 1);
    REQUIRE(in_flight.Size() == 0);
    REQUIRE(in_flight.Acquire(0x3000).Owned());  // free again once the compile is over

    // Moved claims release once.
    {
        auto a = in_flight.Acquire(0x4000);
        auto b = std::move(a);
        REQUIRE(b.Owned());
        REQUIRE(in_flight.Size() == 1);
    }
    REQUIRE(in_flight.Size() == 0);
}

void TestJournal() {
    InvalidationJournal journal{4};
    std::vector<u64> dropped;
    int drop_alls = 0;
    const auto drain = [&] {
        return journal.Drain([&](u64 d) { dropped.push_back(d); }, [&] { ++drop_alls; });
    };
    REQUIRE(!drain());
    journal.Push(1);
    journal.Push(2);
    REQUIRE(journal.HasWork() && drain());
    REQUIRE((dropped == std::vector<u64>{1, 2}) && drop_alls == 0 && !journal.HasWork());
    for (u64 d = 0; d < 5; ++d) journal.Push(d);  // One past capacity: collapses.
    dropped.clear();
    REQUIRE(drain() && dropped.empty() && drop_alls == 1);
    journal.Push(7);
    journal.PushDropAll();
    journal.Push(8);  // After a drop-all nothing more needs listing.
    REQUIRE(drain() && dropped.empty() && drop_alls == 2);
    journal.Push(9);
    journal.Discard();
    REQUIRE(!journal.HasWork() && !drain());

    // Many pushers, one draining owner: every push is seen by a drop or a drop-all.
    InvalidationJournal shared{1 << 20};
    std::atomic<bool> done{false};
    std::vector<std::thread> pushers;
    for (u64 t = 0; t < 3; ++t) {
        pushers.emplace_back([&, t] {
            for (u64 i = 0; i < 100000; ++i) shared.Push((t << 32) | i);
        });
    }
    u64 seen = 0;
    std::thread owner{[&] {
        const auto drain_shared = [&] {
            shared.Drain([&](u64) { ++seen; }, [] { Die("journal", "dropped all below capacity"); });
        };
        while (!done) drain_shared();
        drain_shared();
    }};
    for (auto& pusher : pushers) pusher.join();
    done = true;
    owner.join();
    REQUIRE(seen == 300000);
}

void TestFastDispatchTable() {
    for (const bool crc : {false, true}) {
        FastDispatchTable table{crc};
        std::mt19937_64 rng{crc ? 1u : 2u};
        for (int i = 0; i < 10000; ++i) {
            const u64 descriptor = rng();
            const u32 offset = table.SlotOffset(descriptor);
            REQUIRE((offset & ~FastDispatchTable::offset_mask) == 0 && offset % sizeof(FastDispatchTable::Entry) == 0);
            if (!crc) REQUIRE(offset == (descriptor & FastDispatchTable::offset_mask));
        }
        const u64 a = 0x1234560;
        FastDispatchTable::Entry& slot = table.Slot(a);
        REQUIRE(slot.location_descriptor == ~u64{0} && slot.code_ptr == nullptr);
        slot = {a, &table};
        // A descriptor holding another slot, or sharing this one, leaves it alone.
        u64 other = a + 0x10;
        while (table.SlotOffset(other) != table.SlotOffset(a)) other += 0x10;
        table.Invalidate(other);
        REQUIRE(slot.location_descriptor == a);
        table.Invalidate(a);
        REQUIRE(slot.location_descriptor == ~u64{0});
        slot = {a, &table};
        table.Clear();
        REQUIRE(slot.location_descriptor == ~u64{0});
    }
}

void TestConfigCompatibility() {
    Machine machine;
    Callbacks cb0, cb1;
    const A64::UserConfig base = MakeConfig(machine, cb0, 0, nullptr, 8 << 20);
    const auto with = [&](auto change) {
        A64::UserConfig other = MakeConfig(machine, cb1, 1, nullptr, 8 << 20);
        change(other);
        return Backend::X64::A64ConfigsShareCode(base, other);
    };
    using Backend::X64::A64ConfigsShareCode;
    // Per core, read through the bindings: may differ.
    REQUIRE(with([](A64::UserConfig&) {}));
    REQUIRE(with([](A64::UserConfig& c) { c.code_cache_size = 1 << 20; }));
    REQUIRE(with([](A64::UserConfig& c) { c.shared_code_cache = std::make_shared<A64::SharedCodeCache>(2); }));
    // Emitted into the code: must match.
    REQUIRE(!with([](A64::UserConfig& c) { c.optimizations = no_optimizations; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.page_table = nullptr; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.page_table_thunks = false; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.fastmem_pointer = 0x1000; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.tpidr_el0 = nullptr; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.global_monitor = nullptr; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.enable_cycle_counting = false; }));
    REQUIRE(!with([](A64::UserConfig& c) { c.wall_clock_cntpct = true; }));
    struct OtherCallbacks final : A64::UserCallbacks {
        std::optional<u32> MemoryReadCode(u64) override { return std::nullopt; }
        u64 MemoryRead(u64, std::size_t) override { return 0; }
        A64::Vector MemoryRead128(u64) override { return {}; }
        void MemoryWrite(u64, u64, std::size_t) override {}
        void MemoryWrite128(u64, A64::Vector) override {}
        void CallSVC(u32) override {}
        void ExceptionRaised(u64, A64::Exception) override {}
        void AddTicks(u64) override {}
        u64 GetTicksRemaining() override { return 0; }
        u64 GetCNTPCT() override { return 0; }
    } other_type;
    REQUIRE(!with([&](A64::UserConfig& c) { c.callbacks = &other_type; }));
}

void TestSharedSize() {
    using Backend::X64::A64SharedCodeCacheSize;
    constexpr std::size_t MiB = 1024 * 1024;
    REQUIRE(A64SharedCodeCacheSize(256 * MiB, 4) == 1024 * MiB);
    REQUIRE(A64SharedCodeCacheSize(512 * MiB, 4) == 1536 * MiB);  // rel32 reach
    REQUIRE(A64SharedCodeCacheSize(8 * MiB, 0) == 8 * MiB);
    REQUIRE(A64SharedCodeCacheSize(2048 * MiB, 1) == 1536 * MiB);
    REQUIRE(A64SharedCodeCacheSize(SIZE_MAX / 2, 4) == 1536 * MiB);  // no overflow
}

// --- JITs sharing one cache ------------------------------------------------------------------------

void TestCompileOnce() {
    Cluster cluster;
    const auto& stats = StatsOf(cluster.handle);
    REQUIRE(stats.cores == Cores && stats.private_fallbacks == 0);

    cluster.RunKernel(0);
    const u64 blocks = Compiled(cluster.handle);
    REQUIRE(blocks >= 5);  // entry, sub, after-return, linked, indirect
    cluster.RunKernelEverywhere(1);
    REQUIRE(Compiled(cluster.handle) == blocks);  // the other cores ran core 0's code
    REQUIRE(stats.duplicate_translations == 0);
    cluster.RunKernelEverywhere(2);  // warm: links, fast dispatch, RSB
    REQUIRE(Compiled(cluster.handle) == blocks);
    REQUIRE(stats.full_clears == 0);
}

void TestInvalidationReachesOtherCores() {
    Cluster cluster;
    cluster.RunKernelEverywhere();
    cluster.RunKernelEverywhere();  // every core has the blocks in its links, table and RSB
    struct Case {
        u64 word;
        u32 Kernel::Values::*value;
        u32 new_value;
        std::size_t invalidator;
    };
    // One core invalidates: the others must stop running the old block however they reach it.
    for (const Case c : {Case{Kernel::IndirectWord, &Kernel::Values::indirect, 0x4444, 0},
                         Case{Kernel::LinkedWord, &Kernel::Values::linked, 0x5555, 1},
                         Case{Kernel::EntryWord, &Kernel::Values::entry, 0x6666, 2}}) {
        const u64 before = Compiled(cluster.handle);
        u32& word = cluster.machine.code.At(c.word);
        word = (word & ~(0xFFFFu << 5)) | (c.new_value << 5);
        cluster.values.*c.value = c.new_value;
        cluster.jits[c.invalidator]->InvalidateCacheRange(c.word, 4);
        cluster.RunKernelEverywhere(c.invalidator);
        REQUIRE(Compiled(cluster.handle) == before + 1);  // recompiled once, for everyone
    }
    REQUIRE(StatsOf(cluster.handle).full_clears == 0);
}

void TestClearFromOneCore() {
    Cluster cluster;
    cluster.RunKernelEverywhere();
    const u64 blocks = Compiled(cluster.handle);
    cluster.jits[3]->ClearCache();
    cluster.RunKernelEverywhere(3);
    REQUIRE(StatsOf(cluster.handle).full_clears == 1);
    REQUIRE(Compiled(cluster.handle) == 2 * blocks);  // everything once more, once
    // Requests that a clear already covered do not clear again.
    cluster.jits[0]->ClearCache();
    cluster.jits[1]->ClearCache();
    cluster.RunKernelEverywhere();
    REQUIRE(StatsOf(cluster.handle).full_clears == 2);
}

void TestPrewarmOnSharedCache() {
    Cluster cluster;
    std::vector<BlockProfile> profiles;
    cluster.jits[0]->SetBlockProfileCallback([&](const BlockProfile& p) { profiles.push_back(p); });
    cluster.RunKernel(0);
    const u64 blocks = Compiled(cluster.handle);
    REQUIRE(!profiles.empty());
    // Already compiled by another core: warm at no cost.
    for (const BlockProfile& p : profiles) REQUIRE(cluster.jits[1]->PrecompileBlock(p));
    REQUIRE(Compiled(cluster.handle) == blocks);
    // After a clear, one core's prewarm serves them all.
    cluster.jits[2]->ClearCache();
    cluster.RunKernel(2);  // applies the clear, then compiles again
    cluster.jits[2]->ClearCache();
    for (const BlockProfile& p : profiles) REQUIRE(cluster.jits[2]->PrecompileBlock(p));
    const u64 warmed = Compiled(cluster.handle);
    cluster.RunKernelEverywhere(1);
    REQUIRE(Compiled(cluster.handle) == warmed);
}

void TestIncompatibleCoreKeepsItsOwn() {
    Machine machine;
    Callbacks cb0, cb1;
    cb0.machine = cb1.machine = &machine;
    machine.code.Add(KernelBase, Kernel::Program(0x1111, 0x2222, 0x3333));
    auto handle = std::make_shared<A64::SharedCodeCache>(2);
    A64::Jit jit0{MakeConfig(machine, cb0, 0, handle, 8 << 20)};
    A64::UserConfig odd = MakeConfig(machine, cb1, 1, handle, 8 << 20);
    odd.optimizations = no_optimizations;
    A64::Jit jit1{odd};
    cb0.jit = &jit0;
    cb1.jit = &jit1;
    REQUIRE(StatsOf(handle).cores == 1 && StatsOf(handle).private_fallbacks == 1);
    Kernel::RunAndCheck(machine, jit0, cb0, 0, {});
    Kernel::RunAndCheck(machine, jit1, cb1, 1, {});
    REQUIRE(Compiled(handle) >= 5);
}

// --- Threads on a cache too small for the program --------------------------------------------------

namespace Chain {
constexpr std::size_t Blocks = 28000;  // about 16 MiB of host code, 11 MiB of guest code
constexpr std::size_t Ops = 96;
constexpr u32 Registers = 8;  // X1..X8; X9 is the branch register

struct Op {
    enum Kind { Add, EorRor, AddImm } kind;
    u32 d, n, m;
};

struct Program {
    std::vector<std::vector<Op>> blocks;
    std::vector<u32> words;
    u64 leaf = 0;
};

/// Blocks of ALU ops, each ending with a different way to reach the next one: a linked branch,
/// an indirect branch (fast dispatch), or a call to a leaf whose return lands on the next one
/// (RSB). The last one ends with SVC #0.
Program Make() {
    Program program;
    std::mt19937 rng{0x5EED};
    for (std::size_t b = 0; b < Blocks; ++b) {
        std::vector<Op> ops;
        for (std::size_t i = 0; i < Ops; ++i) {
            const auto kind = static_cast<Op::Kind>(rng() % 3);
            ops.push_back({kind, 1 + rng() % Registers, 1 + rng() % Registers,
                           kind == Op::AddImm ? rng() % 4096 : 1 + rng() % Registers});
            if (kind == Op::EorRor) ops.back().m |= (1 + rng() % 63) << 8;  // shift in the high byte
        }
        program.blocks.push_back(std::move(ops));
    }
    // Lay out: block b's words, then its exit.
    std::vector<std::size_t> exits;
    for (std::size_t b = 0; b < Blocks; ++b) {
        for (const Op& op : program.blocks[b]) {
            switch (op.kind) {
            case Op::Add: program.words.push_back(Asm::AddReg(op.d, op.n, op.m)); break;
            case Op::EorRor: program.words.push_back(Asm::EorRor(op.d, op.n, op.m & 0xFF, op.m >> 8)); break;
            case Op::AddImm: program.words.push_back(Asm::AddImm(op.d, op.n, op.m)); break;
            }
        }
        exits.push_back(program.words.size());
        const std::size_t exit_words = b + 1 == Blocks ? 1 : b % 3 == 1 ? 2 : 1;
        program.words.insert(program.words.end(), exit_words, 0);
    }
    program.leaf = ChainBase + program.words.size() * 4;
    program.words.push_back(Asm::AddImm(8, 8, 1));
    program.words.push_back(Asm::Ret);
    for (std::size_t b = 0; b < Blocks; ++b) {
        const u64 at = ChainBase + exits[b] * 4;
        const std::size_t next_index = b + 1 == Blocks ? 0 : exits[b] + (b % 3 == 1 ? 2 : 1);
        const u64 next = ChainBase + next_index * 4;
        u32* exit = &program.words[exits[b]];
        if (b + 1 == Blocks) {
            exit[0] = Asm::Svc(0);
        } else if (b % 3 == 0) {
            exit[0] = Asm::B(at, next);
        } else if (b % 3 == 1) {
            exit[0] = Asm::Adr(9, at, next);
            exit[1] = Asm::Br(9);
        } else {
            exit[0] = Asm::Bl(at, program.leaf);
        }
    }
    return program;
}

/// What the program leaves in X1..X8.
std::array<u64, Registers + 1> Simulate(const Program& program, std::array<u64, Registers + 1> x) {
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        for (const Op& op : program.blocks[b]) {
            switch (op.kind) {
            case Op::Add: x[op.d] = x[op.n] + x[op.m]; break;
            case Op::EorRor: x[op.d] = x[op.n] ^ std::rotr(x[op.m & 0xFF], static_cast<int>(op.m >> 8)); break;
            case Op::AddImm: x[op.d] = x[op.n] + op.m; break;
            }
        }
        if (b + 1 != program.blocks.size() && b % 3 == 2) x[8] += 1;  // the leaf
    }
    return x;
}

std::array<u64, Registers + 1> Seed(std::size_t core, std::size_t round) {
    std::array<u64, Registers + 1> x{};
    std::mt19937_64 rng{core * 1000 + round};
    for (u32 r = 1; r <= Registers; ++r) x[r] = rng();
    return x;
}
}  // namespace Chain

/// Host code the chain needs, measured on a private cache.
std::size_t MeasureChain(const Chain::Program& program) {
    Machine machine;
    machine.code.Add(ChainBase, program.words);
    Callbacks cb;
    cb.machine = &machine;
    A64::Jit jit{MakeConfig(machine, cb, 0, nullptr, 256u << 20)};
    cb.jit = &jit;
    const std::size_t before = jit.GetCodeCacheSpaceRemaining();
    jit.SetPC(ChainBase);
    RunToSvc(jit, cb);
    return before - jit.GetCodeCacheSpaceRemaining();
}

void StressThreads(const Chain::Program& program, u32 per_core_bytes, bool chaos) {
    Machine machine;
    machine.code.Add(ChainBase, program.words);
    auto handle = std::make_shared<A64::SharedCodeCache>(Cores);
    std::array<Callbacks, Cores> callbacks;
    std::vector<std::unique_ptr<A64::Jit>> jits;
    for (std::size_t core = 0; core < Cores; ++core) {
        callbacks[core].machine = &machine;
        jits.push_back(std::make_unique<A64::Jit>(MakeConfig(machine, callbacks[core], core, handle, per_core_bytes)));
        callbacks[core].jit = jits.back().get();
    }
    REQUIRE(StatsOf(handle).cores == Cores);

    constexpr std::size_t Rounds = 3;
    std::atomic<bool> wrong{false};
    std::atomic<std::size_t> running{Cores};
    std::vector<std::thread> threads;
    for (std::size_t core = 0; core < Cores; ++core) {
        threads.emplace_back([&, core] {
            A64::Jit& jit = *jits[core];
            for (std::size_t round = 0; round < Rounds; ++round) {
                const auto seed = Chain::Seed(core, round);
                const auto expected = Chain::Simulate(program, seed);
                for (u32 r = 1; r <= Chain::Registers; ++r) jit.SetRegister(r, seed[r]);
                jit.SetPC(ChainBase);
                RunToSvc(jit, callbacks[core]);
                for (u32 r = 1; r <= Chain::Registers; ++r) {
                    if (jit.GetRegister(r) != expected[r]) wrong = true;
                }
            }
            running.fetch_sub(1);
        });
    }
    // Meanwhile, what the kernel does to every core: invalidate code it reloads or reprotects, and
    // clear the cache. The guest code itself never changes, so the results must not either.
    std::thread chaos_thread;
    if (chaos) {
        chaos_thread = std::thread{[&] {
            std::mt19937 rng{7};
            while (running.load() != 0) {
                const u64 at = ChainBase + (rng() % program.words.size()) * 4;
                if (rng() % 8 == 0) {
                    jits[rng() % Cores]->ClearCache();
                } else {
                    for (auto& jit : jits) jit->InvalidateCacheRange(at, 64);
                }
                std::this_thread::sleep_for(std::chrono::microseconds{500});
            }
        }};
    }
    for (auto& thread : threads) thread.join();
    if (chaos_thread.joinable()) chaos_thread.join();
    REQUIRE(!wrong);
    const auto& stats = StatsOf(handle);
    REQUIRE(stats.full_clears > 0);  // the cache was too small: it cleared under the cores
    // Cores missing the same block wait for the one compiling it. Only an invalidation or a clear
    // racing a compile can still make a core translate a block another core then emits.
    REQUIRE(stats.duplicate_translations < stats.blocks_compiled / 4);
    std::printf("  %u KiB a core: %llu blocks compiled, %llu awaited, %llu translated twice, %llu full clears\n",
                per_core_bytes / 1024, static_cast<unsigned long long>(stats.blocks_compiled.load()),
                static_cast<unsigned long long>(stats.compiles_awaited.load()),
                static_cast<unsigned long long>(stats.duplicate_translations.load()),
                static_cast<unsigned long long>(stats.full_clears.load()));
}

const Chain::Program& ChainProgram() {
    static const Chain::Program program = Chain::Make();
    return program;
}

void TestThreadsOutOfRoom() {
    // 6 MiB shared, about 4 MiB of room past the 2 MiB constant pool: the early-clear threshold
    // (a quarter of the room) is the 1 MiB minimum, so cores run out of room mid-run and wait on
    // the gate for the clear.
    const std::size_t needed = MeasureChain(ChainProgram());
    std::printf("  chain needs %zu KiB of host code\n", needed / 1024);
    REQUIRE(needed > 12u << 20);  // must not fit in either stress cache
    StressThreads(ChainProgram(), 3u << 19, false);
}

void TestThreadsEarlyClear() {
    // 12 MiB shared, about 10 MiB of room: clears requested 2.5 MiB before the cache fills.
    StressThreads(ChainProgram(), 3u << 20, false);
}

void TestThreadsWithInvalidations() {
    StressThreads(ChainProgram(), 3u << 20, true);
}

}  // namespace

int main() {
    std::set_terminate([] {
        const char* what = "unknown exception";
        try {
            if (const auto exception = std::current_exception()) std::rethrow_exception(exception);
            what = "terminate without an exception";
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        std::fprintf(stderr, "FAIL %s: uncaught: %s\n", current_test, what);
        std::fflush(stderr);
        std::_Exit(4);
    });
    Run("link sites are patched by one aligned store", TestLinkSites);
    Run("quiescence gate, one thread", TestGateSingleThread);
    Run("quiescence gate never clears under a thread", TestGateThreads);
    Run("in-flight compiles: one core compiles, the others wait", TestInFlightCompiles);
    Run("invalidation journal", TestJournal);
    Run("fast-dispatch table matches the handler's slots", TestFastDispatchTable);
    Run("configs that can share code", TestConfigCompatibility);
    Run("shared cache size", TestSharedSize);

    if (DualMappedCodeCaches() == 0) {
        // Probe runs on the first cache; the JIT tests below need it to share.
        Machine machine;
        Callbacks cb;
        cb.machine = &machine;
        const A64::Jit probe{MakeConfig(machine, cb, 0, nullptr, 8u << 20)};
        if (DualMappedCodeCaches() == 0) {
            std::fprintf(stderr, "FAIL: this host cannot dual-map JIT code, so cores cannot share it\n");
            return 1;
        }
    }
    Run("a block compiles once and runs on every core", TestCompileOnce);
    Run("an invalidation by one core reaches the others", TestInvalidationReachesOtherCores);
    Run("a clear by one core serves every core", TestClearFromOneCore);
    Run("prewarm on a shared cache", TestPrewarmOnSharedCache);
    Run("an incompatible core keeps a cache of its own", TestIncompatibleCoreKeepsItsOwn);
    Run("threads: cores run out of room and clear", TestThreadsOutOfRoom);
    Run("threads: early clears", TestThreadsEarlyClear);
    Run("threads: invalidations and clears from another thread", TestThreadsWithInvalidations);
    std::printf("PASS all\n");
    return 0;
}
