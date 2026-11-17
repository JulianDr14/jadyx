// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/backend/x64/a64_code_cache.h"

#include <algorithm>
#include <bit>
#include <utility>

#include "common/assert.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/callback.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/fast_dispatch_table.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/interface/jit_profile.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::Backend::X64 {

namespace {

/// Everything the dispatcher calls takes its argument from the core's bindings, so one prelude
/// serves every core.
RunCodeCallbacks GenRunCodeCallbacks(const A64::UserConfig& conf, A64CodeCache::LookupBlockFn lookup_block) {
    return RunCodeCallbacks{
        ArgCallback(lookup_block, 0).WithArgFromJitState(A64BindingOffset::lookup_block_arg),
        DevirtualizeFromJitState<&A64::UserCallbacks::AddTicks>(conf.callbacks, A64BindingOffset::callbacks_arg),
        DevirtualizeFromJitState<&A64::UserCallbacks::GetTicksRemaining>(conf.callbacks, A64BindingOffset::callbacks_arg),
        conf.enable_cycle_counting,
    };
}

std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

/// The cache keeps its own copy of the configuration without the process handle: the handle owns
/// the shared cache, and a cache owning the handle would never be freed.
A64::UserConfig WithoutSharedHandle(A64::UserConfig conf) {
    conf.shared_code_cache = nullptr;
    return conf;
}

/// Largest shared cache: every block must reach the prelude and the other blocks with rel32.
constexpr std::size_t max_shared_code_size = std::size_t{1536} * 1024 * 1024;

}  // anonymous namespace

bool A64ConfigsShareCode(const A64::UserConfig& a, const A64::UserConfig& b) {
    // The code calls the callbacks' member functions resolved through the first core's vtable
    // (devirtualize.h), so the objects must share it. Compared directly: RTTI is off (/GR-).
    const auto vtable = [](const A64::UserCallbacks* callbacks) {
        return callbacks ? *reinterpret_cast<const void* const*>(callbacks) : nullptr;
    };
    const bool same_callbacks = vtable(a.callbacks) == vtable(b.callbacks);
    // TPIDR accesses are emitted as loads through the bindings only when the pointer exists.
    const bool same_tpidr = (a.tpidr_el0 == nullptr) == (b.tpidr_el0 == nullptr) &&
                            (a.tpidrro_el0 == nullptr) == (b.tpidrro_el0 == nullptr);
    // Every other field changes the emitted code or what it points at. Keep in field order.
    return same_callbacks && same_tpidr &&
           a.fastmem_pointer == b.fastmem_pointer &&
           a.global_monitor == b.global_monitor &&
           a.page_table == b.page_table &&
           a.optimizations == b.optimizations &&
           a.page_table_address_space_bits == b.page_table_address_space_bits &&
           a.page_table_pointer_mask == b.page_table_pointer_mask &&
           a.page_table_log2_stride == b.page_table_log2_stride &&
           a.page_table_read_offset == b.page_table_read_offset &&
           a.page_table_thunks == b.page_table_thunks &&
           a.page_table_marked_bit == b.page_table_marked_bit &&
           a.page_table_sign_extension == b.page_table_sign_extension &&
           a.cntfrq_el0 == b.cntfrq_el0 &&
           a.ctr_el0 == b.ctr_el0 &&
           a.dczid_el0 == b.dczid_el0 &&
           a.fastmem_address_space_bits == b.fastmem_address_space_bits &&
           a.detect_misaligned_access_via_page_table == b.detect_misaligned_access_via_page_table &&
           a.unsafe_optimizations == b.unsafe_optimizations &&
           a.hook_data_cache_operations == b.hook_data_cache_operations &&
           a.hook_isb == b.hook_isb &&
           a.hook_hint_instructions == b.hook_hint_instructions &&
           a.silently_mirror_page_table == b.silently_mirror_page_table &&
           a.absolute_offset_page_table == b.absolute_offset_page_table &&
           a.only_detect_misalignment_via_page_table_on_page_boundary ==
               b.only_detect_misalignment_via_page_table_on_page_boundary &&
           a.recompile_on_fastmem_failure == b.recompile_on_fastmem_failure &&
           a.silently_mirror_fastmem == b.silently_mirror_fastmem &&
           a.fastmem_exclusive_access == b.fastmem_exclusive_access &&
           a.page_table_exclusive_access == b.page_table_exclusive_access &&
           a.recompile_on_exclusive_fastmem_failure == b.recompile_on_exclusive_fastmem_failure &&
           a.define_unpredictable_behaviour == b.define_unpredictable_behaviour &&
           a.wall_clock_cntpct == b.wall_clock_cntpct &&
           a.check_halt_on_memory_access == b.check_halt_on_memory_access &&
           a.enable_cycle_counting == b.enable_cycle_counting &&
           a.very_verbose_debugging_output == b.very_verbose_debugging_output;
}

std::size_t A64SharedCodeCacheSize(std::size_t per_core, std::size_t cores) {
    const std::size_t count = std::max<std::size_t>(cores, 1);
    const std::size_t total = per_core > max_shared_code_size / count ? max_shared_code_size : per_core * count;
    return std::clamp(total, std::min(per_core, max_shared_code_size), max_shared_code_size);
}

// ---------------------------------------------------------------------------------------------
// Core and Execution

A64CodeCache::Core::Core(A64CodeCache& cache, A64JitState& state, FastDispatchTable* fast_dispatch)
        : cache{cache}, state{state}, fast_dispatch{fast_dispatch} {}

A64CodeCache::Core::~Core() {
    ASSERT(!active);
    cache.Detach(*this);
}

A64CodeCache::Execution::Execution(A64CodeCache& cache, Core& core)
        : cache{cache}, core{core} {
    cache.Enter(core);
}

A64CodeCache::Execution::~Execution() {
    cache.Leave(core);
}

// ---------------------------------------------------------------------------------------------
// Construction

std::shared_ptr<A64CodeCache> A64CodeCache::Acquire(const A64::UserConfig& conf, LookupBlockFn lookup_block) {
    const auto make_private = [&] {
        return std::make_shared<A64CodeCache>(Mode::Private, conf, lookup_block, conf.code_cache_size);
    };
    if (!conf.shared_code_cache) {
        return make_private();
    }
    A64::SharedCodeCache& handle = *conf.shared_code_cache;
    const auto fall_back = [&] {
        handle.stats->private_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return make_private();
    };

    std::scoped_lock lock{handle.backend_mutex};
    if (handle.backend_unavailable) {
        return fall_back();
    }
    if (handle.backend) {
        auto cache = std::static_pointer_cast<A64CodeCache>(handle.backend);
        return A64ConfigsShareCode(cache->conf, conf) ? cache : fall_back();
    }
    const std::size_t cores = handle.ExpectedCores();
    auto cache = std::make_shared<A64CodeCache>(Mode::Shared, conf, lookup_block,
                                                A64SharedCodeCacheSize(conf.code_cache_size, cores), handle.stats);
    // Another core may run a page while it is patched: only a write view of the code keeps it
    // executable meanwhile.
    if (!cache->code.WritesWithoutProtectionChanges()) {
        handle.backend_unavailable = true;
        return fall_back();
    }
    // The global code cache cap is per core; this one cache serves them all.
    cache->code.SetCapScale(cores);
    handle.backend = cache;
    return cache;
}

A64CodeCache::A64CodeCache(Mode mode, const A64::UserConfig& user_conf, LookupBlockFn lookup_block, std::size_t code_size,
                           std::shared_ptr<A64::SharedCodeCache::Stats> stats)
        : mode{mode}
        , conf{WithoutSharedHandle(user_conf)}
        , stats{std::move(stats)}
        , code{GenRunCodeCallbacks(conf, lookup_block), JitStateInfo{A64JitState{}}, code_size, GenRCP(conf)}
        , emitter{code, conf, MakeHooks()}
        , polyfill_options{GenPolyfillOptions(code)}
        , gate{[this] {
            const ExclusiveLock lock{mutex};
            ClearNow();
        }} {
    ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
    UpdateEarlyClearThreshold();
}

A64CodeCache::~A64CodeCache() {
    ASSERT(cores.empty());
}

A64EmitX64Hooks A64CodeCache::MakeHooks() {
    A64EmitX64Hooks hooks;
    hooks.block_invalidated = [this](IR::LocationDescriptor descriptor) { OnBlockInvalidated(descriptor); };
    if (IsShared()) {
        // A fault arrives on the core that ran the access, holding no lock of the cache.
        hooks.fastmem_fault = [this](u64 rip) {
            const ExclusiveLock lock{mutex};
            return emitter.FastmemCallback(rip);
        };
    }
    return hooks;
}

std::unique_ptr<A64CodeCache::Core> A64CodeCache::Attach(A64JitState& state, FastDispatchTable* fast_dispatch) {
    std::unique_ptr<Core> core{new Core{*this, state, fast_dispatch}};
    std::scoped_lock lock{cores_mutex};
    ASSERT_MSG(IsShared() || cores.empty(), "a private code cache serves one core");
    cores.push_back(core.get());
    if (stats) {
        stats->cores.fetch_add(1, std::memory_order_relaxed);
    }
    return core;
}

void A64CodeCache::Detach(Core& core) {
    std::scoped_lock lock{cores_mutex};
    std::erase(cores, &core);
    if (stats) {
        stats->cores.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------
// Execution

void A64CodeCache::Enter(Core& core) {
    ASSERT(!core.active);
    if (IsShared()) {
        gate.Enter();
    }
    core.active = true;
    Sync(core);
}

void A64CodeCache::Leave(Core& core) {
    ASSERT(core.active);
    core.active = false;
    if (IsShared()) {
        gate.Exit();
    }
}

void A64CodeCache::Sync(Core& core) {
    if (!IsShared()) {
        return;  // A private core drops blocks from its own tables as they go.
    }
    FastDispatchTable* const table = core.fast_dispatch;
    const bool dropped = core.journal.Drain(
        [table](std::uint64_t descriptor) {
            if (table) {
                table->Invalidate(descriptor);
            }
        },
        [table] {
            if (table) {
                table->Clear();
            }
        });
    if (dropped) {
        core.state.ResetRSB();
    }
}

void A64CodeCache::HaltCores(const Core* except) {
    std::scoped_lock lock{cores_mutex};
    for (Core* const core : cores) {
        if (core != except) {
            Atomic::Or(&core->state.halt_reason, static_cast<u32>(HaltReason::CacheInvalidation));
        }
    }
}

void A64CodeCache::RequestSharedClear(const Core* except) {
    if (gate.RequestClear()) {
        HaltCores(except);
    }
}

// ---------------------------------------------------------------------------------------------
// Lookup and compile

CodePtr A64CodeCache::Find(IR::LocationDescriptor descriptor) const {
    std::shared_lock<std::shared_mutex> lock;
    if (IsShared()) {
        lock = std::shared_lock{mutex};
    }
    const auto block = emitter.GetBasicBlock(descriptor);
    return block ? block->entrypoint : nullptr;
}

std::optional<A64CodeCache::Ticket> A64CodeCache::BeginCompile(Core& core, bool may_clear) {
    if (!IsShared()) {
        if (code.SpaceRemaining() < minimum_free_space) {
            if (!may_clear) {
                return std::nullopt;
            }
            ClearNow();
        }
        return Ticket{epoch};
    }

    ASSERT_MSG(!may_clear || core.active, "only an executing core may wait for a shared clear");
    for (;;) {
        std::size_t space;
        Ticket ticket;
        {
            const ExclusiveLock lock{mutex};
            space = code.SpaceRemaining();
            ticket = Ticket{epoch};
        }
        if (space >= minimum_free_space) {
            if (space < early_clear_threshold && may_clear) {
                // Every core, this one included, stops at its next block boundary for it.
                RequestSharedClear(nullptr);
            }
            return ticket;
        }
        if (!may_clear) {
            return std::nullopt;
        }
        // Out of room: stop running cache code until the clear is done. The caller's frame is in
        // the dispatcher (prelude code), which the clear keeps.
        RequestSharedClear(&core);
        Leave(core);
        Enter(core);
    }
}

InFlightCompiles::Claim A64CodeCache::ClaimCompile(IR::LocationDescriptor descriptor) {
    if (!IsShared()) {
        return InFlightCompiles::Claim::Exclusive();
    }
    auto claim = in_flight.Acquire(descriptor.Value());
    if (!claim.Owned() && stats) {
        stats->compiles_awaited.fetch_add(1, std::memory_order_relaxed);
    }
    return claim;
}

A64CodeCache::EmitResult A64CodeCache::Emit(IR::Block& block, Ticket ticket) {
    const ExclusiveLock lock = LockExclusive();
    if (epoch != ticket.epoch) {
        return {};  // The guest code may have changed since it was read.
    }
    if (const auto existing = emitter.GetBasicBlock(block.Location())) {
        if (stats) {
            stats->duplicate_translations.fetch_add(1, std::memory_order_relaxed);
        }
        return {existing->entrypoint, false};
    }
    if (code.SpaceRemaining() < minimum_free_space) {
        return {};  // Other cores used the room: BeginCompile clears.
    }
    code.EnsureMemoryCommitted(minimum_free_space);
    const JitProfile::Timer emit_timer{JitProfile::Phase::Emit};
    const CodePtr entrypoint = emitter.Emit(block).entrypoint;
    if (stats) {
        stats->blocks_compiled.fetch_add(1, std::memory_order_relaxed);
    }
    return {entrypoint, true};
}

std::size_t A64CodeCache::SpaceRemaining() {
    const ExclusiveLock lock = LockExclusive();  // SpaceRemaining refreshes the cap it caches.
    return code.SpaceRemaining();
}

// ---------------------------------------------------------------------------------------------
// Invalidation

void A64CodeCache::OnBlockInvalidated(IR::LocationDescriptor descriptor) {
    std::scoped_lock lock{cores_mutex};
    for (Core* const core : cores) {
        if (!IsShared()) {
            // Same thread as the code, which is stopped here (invalidation or fastmem fault).
            if (core->fast_dispatch) {
                core->fast_dispatch->Invalidate(descriptor.Value());
            }
            continue;
        }
        core->journal.Push(descriptor.Value());
        if (core != invalidator) {
            Atomic::Or(&core->state.halt_reason, static_cast<u32>(HaltReason::CacheInvalidation));
        }
    }
}

void A64CodeCache::InvalidateRanges(Core& core, const boost::icl::interval_set<u64>& ranges) {
    {
        const ExclusiveLock lock = LockExclusive();
        ++epoch;
        invalidator = &core;
        emitter.InvalidateCacheRanges(ranges);
        invalidator = nullptr;
    }
    Sync(core);
}

bool A64CodeCache::Clear(Core& core, std::uint64_t seen_generation) {
    if (Generation() != seen_generation) {
        return false;  // Cleared since the request: nothing it asked to drop is left.
    }
    if (!IsShared()) {
        ClearNow();
        return true;
    }
    ASSERT(!core.active);
    // A clear already pending also runs before any core enters again.
    RequestSharedClear(&core);
    return true;
}

void A64CodeCache::ClearNow() {
    JitProfile::full_cache_clears.fetch_add(1, std::memory_order_relaxed);
    code.ClearCache();
    emitter.ClearCache();
    UpdateEarlyClearThreshold();  // The cap may have moved since the last clear.
    ++epoch;
    generation.fetch_add(1, std::memory_order_release);
    if (stats) {
        stats->full_clears.fetch_add(1, std::memory_order_relaxed);
    }

    std::scoped_lock lock{cores_mutex};
    for (Core* const core : cores) {
        if (IsShared()) {
            core->journal.PushDropAll();  // Applied when the core enters again.
        } else {
            if (core->fast_dispatch) {
                core->fast_dispatch->Clear();
            }
            core->state.ResetRSB();
        }
    }
}

void A64CodeCache::UpdateEarlyClearThreshold() {
    early_clear_threshold = std::min(early_clear_space, code.SpaceRemaining() / 4);
}

}  // namespace Dynarmic::Backend::X64
