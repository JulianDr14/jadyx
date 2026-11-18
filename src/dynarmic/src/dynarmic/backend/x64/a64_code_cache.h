// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

#include <boost/icl/interval_set.hpp>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/in_flight_compiles.h"
#include "dynarmic/backend/x64/invalidation_journal.h"
#include "dynarmic/backend/x64/quiescence_gate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/A64/shared_code_cache.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::X64 {

class FastDispatchTable;
struct A64JitState;

/// Whether code emitted for one configuration is correct on a core with the other. Values the
/// code reads from A64JitState::bindings at run time (the callbacks object, processor id, TPIDR
/// pointers) may differ, as may the cache size; the rest must match.
[[nodiscard]] bool A64ConfigsShareCode(const A64::UserConfig& a, const A64::UserConfig& b);

/// Code cache size for a shared cache: `cores` caches of `per_core` bytes, kept within the reach of
/// the rel32 branches between blocks.
[[nodiscard]] std::size_t A64SharedCodeCacheSize(std::size_t per_core, std::size_t cores);

/// The compiled A64 code of one or more cores: the code memory, the emitter that fills it, and the
/// bookkeeping to keep each core's own shortcuts into it (fast-dispatch table, return stack
/// buffer) valid as blocks go.
///
/// Private (one core): no synchronisation; behaves as a cache owned by its Jit.
///
/// Shared (every core of a process):
///  - Lookups take `mutex` shared; emitting, invalidating and fastmem patching take it exclusive.
///    Translation runs outside it; a block whose guest code may have changed meanwhile is
///    translated again (`epoch`).
///  - Code memory is never reused while a core may run it: invalidated blocks are unlinked with
///    atomic stores and stay in place. Only a full clear reuses memory, and it runs through
///    `gate` when no core is running cache code.
///  - Other cores learn about dropped blocks through their journal, drained on their own thread
///    (Sync), and a CacheInvalidation halt so that happens soon.
///  - A core waiting on the gate holds no lock: the full clear takes `mutex`.
class A64CodeCache {
public:
    enum class Mode { Private, Shared };

    using LookupBlockFn = CodePtr (*)(void* lookup_block_arg);

    /// A core attached to the cache. Its thread owns its fast-dispatch table and state.
    class Core {
    public:
        ~Core();
        Core(const Core&) = delete;
        Core& operator=(const Core&) = delete;

    private:
        friend class A64CodeCache;
        Core(A64CodeCache& cache, A64JitState& state, FastDispatchTable* fast_dispatch);

        A64CodeCache& cache;
        A64JitState& state;
        FastDispatchTable* const fast_dispatch;
        InvalidationJournal journal;
        bool active = false;
    };

    /// The core counts as running cache code while this lives (shared: inside the gate).
    class [[nodiscard]] Execution {
    public:
        ~Execution();
        Execution(const Execution&) = delete;
        Execution& operator=(const Execution&) = delete;

    private:
        friend class A64CodeCache;
        Execution(A64CodeCache& cache, Core& core);

        A64CodeCache& cache;
        Core& core;
    };

    /// Where a compile started: an Emit after any invalidation since then translates again.
    struct Ticket {
        std::uint64_t epoch;
    };

    struct EmitResult {
        CodePtr entrypoint = nullptr;  ///< Null: translate again and retry.
        bool compiled = false;         ///< False when another core had compiled the block.
    };

    /// Room left before a compile must clear the cache.
    static constexpr std::size_t minimum_free_space = 1 * 1024 * 1024;
    /// Shared: below this, a clear is requested while there is still room to compile, so cores
    /// stop at their next block boundary instead of one stalling on a full cache. At most a
    /// quarter of the room a clear leaves, so a small or capped cache does not clear constantly.
    static constexpr std::size_t early_clear_space = 8 * 1024 * 1024;

    /// The cache for a core with `conf`: the process's cache when conf.shared_code_cache is set and
    /// the core can share it, else one of its own.
    static std::shared_ptr<A64CodeCache> Acquire(const A64::UserConfig& conf, LookupBlockFn lookup_block);

    A64CodeCache(Mode mode, const A64::UserConfig& conf, LookupBlockFn lookup_block, std::size_t code_size,
                 std::shared_ptr<A64::SharedCodeCache::Stats> stats = nullptr);
    ~A64CodeCache();

    A64CodeCache(const A64CodeCache&) = delete;
    A64CodeCache& operator=(const A64CodeCache&) = delete;

    [[nodiscard]] bool IsShared() const noexcept {
        return mode == Mode::Shared;
    }

    /// `state.bindings` must already be filled in for this core.
    [[nodiscard]] std::unique_ptr<Core> Attach(A64JitState& state, FastDispatchTable* fast_dispatch);

    /// Call before running the code; waits out a pending full clear and applies what other cores
    /// dropped. Not while holding a lock the clear could need.
    [[nodiscard]] Execution Execute(Core& core) {
        return Execution{*this, core};
    }

    /// Entry point of the block compiled for `descriptor`, or null.
    [[nodiscard]] CodePtr Find(IR::LocationDescriptor descriptor) const;

    /// Makes room for one more block. When the cache is full it is cleared, which in shared mode
    /// waits for every core to stop (the caller must be executing); with `may_clear` false it
    /// fails instead.
    [[nodiscard]] std::optional<Ticket> BeginCompile(Core& core, bool may_clear);

    /// The right to translate and emit `descriptor`. Shared: when another core is compiling it,
    /// waits for that core and returns an unowned claim (look the block up again). Take it after
    /// BeginCompile and hold it only to translate and emit: a holder must never wait for a clear.
    [[nodiscard]] InFlightCompiles::Claim ClaimCompile(IR::LocationDescriptor descriptor);

    /// Emits `block`, translated after `ticket`. See EmitResult.
    [[nodiscard]] EmitResult Emit(IR::Block& block, Ticket ticket);

    /// Drops the blocks compiled from guest code in `ranges`.
    void InvalidateRanges(Core& core, const boost::icl::interval_set<u64>& ranges);

    /// Counts full clears; pass a value read before a ClearCache request to Clear.
    [[nodiscard]] std::uint64_t Generation() const noexcept {
        return generation.load(std::memory_order_acquire);
    }

    /// Clears the whole cache unless it was cleared since `seen_generation`. Shared: the clear runs
    /// once no core is executing (before any core runs code again), and executing cores are halted
    /// to get there; the caller must not be executing.
    /// Returns false when skipped: blocks compiled after that earlier clear are still there.
    bool Clear(Core& core, std::uint64_t seen_generation);

    /// Applies, on the core's own thread, what other cores dropped. Cheap when there is nothing.
    void Sync(Core& core);

    [[nodiscard]] std::size_t SpaceRemaining();

    [[nodiscard]] const Optimization::PolyfillOptions& GetPolyfillOptions() const noexcept {
        return polyfill_options;
    }

    [[nodiscard]] const BlockOfCode& Code() const noexcept {
        return code;
    }

private:
    using ExclusiveLock = std::unique_lock<std::shared_mutex>;

    /// The exclusive lock in shared mode, nothing in private mode.
    [[nodiscard]] ExclusiveLock LockExclusive() {
        return IsShared() ? ExclusiveLock{mutex} : ExclusiveLock{};
    }

    A64EmitX64Hooks MakeHooks();
    void OnBlockInvalidated(IR::LocationDescriptor descriptor);
    void Enter(Core& core);
    void Leave(Core& core);
    /// Requires the exclusive lock (shared) and, in shared mode, no executing core.
    void ClearNow();
    /// Sets early_clear_threshold from the room an empty cache has. Under the exclusive lock.
    void UpdateEarlyClearThreshold();
    /// Asks the gate for a clear and halts every core but `except` so it can run.
    void RequestSharedClear(const Core* except);
    void HaltCores(const Core* except);
    void Detach(Core& core);

    const Mode mode;
    const A64::UserConfig conf;
    const std::shared_ptr<A64::SharedCodeCache::Stats> stats;

    mutable std::shared_mutex mutex;
    BlockOfCode code;
    A64EmitX64 emitter;
    const Optimization::PolyfillOptions polyfill_options;
    /// Bumped by every invalidation and clear, under the exclusive lock.
    std::uint64_t epoch = 0;
    /// See early_clear_space. Under the exclusive lock.
    std::size_t early_clear_threshold = 0;
    std::atomic<std::uint64_t> generation{0};
    /// The core invalidating ranges, under the exclusive lock: it drops them itself, unhalted.
    const Core* invalidator = nullptr;

    std::mutex cores_mutex;  ///< Taken after `mutex` when both are held.
    std::vector<Core*> cores;

    QuiescenceGate gate;
    InFlightCompiles in_flight;
};

}  // namespace Dynarmic::Backend::X64
