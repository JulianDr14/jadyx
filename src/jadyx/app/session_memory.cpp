// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <string>

#include "common/host_memory.h"
#include "common/permafrost.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "dynarmic/interface/code_memory.h"

#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/boot_renderer_decls.h"
#include "jadyx/app/boot_state.h"
#include "jadyx/app/session_memory.h"

namespace Jadyx {

namespace {

// memory_audit: the heaps and the memory map when the headroom runs low, since the
// minute reports miss the few seconds of a load that fill it. Again only after a further
// 50 MiB drop (at most every 2 s), or once the headroom has recovered.
constexpr u64 LOW_HEADROOM = 150ULL << 20;
constexpr u64 LOW_HEADROOM_STEP = 50ULL << 20;
// Short of memory, each JIT code cache keeps what it has committed, or this much.
constexpr size_t JIT_CAP_FLOOR = 64ULL << 20;
// Shorter still, a cache holding more than this is cleared and gives the rest back: a large
// game held ~160 MiB in each of three caches when a new area's textures ran it out.
constexpr size_t JIT_RELEASE_CAP = 96ULL << 20;
// With Permafrost the JIT gives way only once it falls behind: a cold guest page costs a
// read of ~0.6 ms when touched again, a cleared cache recompiles the game's hot code. At
// 768 MiB, inside Permafrost's band, a world entry froze the caches at 64 MiB with 460
// free and cleared them four times, compiling 60-80k blocks in a frame (up to 973 ms).
// So they stop growing where Permafrost turns urgent and give back only below its
// emergency mark, once it has had the first go: giving back clears a cache that may hold
// a 192 MiB prewarm. They grow again once Permafrost has brought the headroom back to
// its low mark.
// With Permafrost these marks come from the memory coordinator, which brings them down
// with Permafrost's when the guest stalls on reads back.
constexpr u64 POOL_TRIM_PER_SECOND = 64ULL << 20;
constexpr u64 POOL_TRIM_ALL_HEADROOM = 512ULL << 20;
constexpr u64 TRIM_SLICE = 32ULL << 20;
constexpr u64 TRIM_LOW_HEADROOM = 1024ULL << 20;
constexpr u64 TRIM_ACTIVE_PASS = 64ULL << 20;

} // namespace

SessionMemory::SessionMemory(Core::System& system_, const BootConfig& config_, u64 program_id_,
                             PrewarmBudget& prewarm_budget_)
    : system{system_}, config{config_}, program_id{program_id_}, prewarm_budget{prewarm_budget_},
      jit_after_swap{Common::GuestSwapActive()} {
const u64 swap_low = u64{config.guest_swap_low_mib} << 20;
const u64 swap_high = (std::max)(u64{config.guest_swap_high_mib} << 20, swap_low);
const u64 swap_watch = (std::max)(u64{config.guest_swap_watch_mib} << 20, swap_high);
coordinator_config.watch = swap_watch;
coordinator_config.low = swap_low;
coordinator_config.high = swap_high;
coordinator_config.low_floor =
    (std::min)(u64{config.memory_coordinator_floor_mib} << 20, swap_low);
if (config.texture_cap_line_mib) {
    coordinator_config.texture_line = u64{*config.texture_cap_line_mib} << 20;
}
memory_plan.plan = Jadyx::MemoryCoordinator{coordinator_config}.Initial();
    if (config.host_trim) {
        trim_worker.emplace([this](std::stop_token stop) { RunHostTrim(stop); });
    }
// Permafrost: close to short of memory the guest's use of its granules is sampled; short
// of it, the idlest go on probation and then to disk until the high mark is free again.
// The further below the low mark, the shorter the probation and the idle time asked.
if (Common::GuestSwapActive()) {
    const auto initial = ReadPlan();
    WriteDiag("permafrost on: sampling below " + std::to_string(initial.watch >> 20) +
              " MiB free, writing below " + std::to_string(initial.low >> 20) +
              " MiB up to " + std::to_string(initial.high >> 20) +
              " MiB; JIT caches stop growing below " +
              std::to_string(initial.jit_stop >> 20) + " MiB, give back below " +
              std::to_string(initial.jit_release >> 20) + " MiB; memory coordinator " +
              (config.memory_coordinator
                   ? "on, low mark down to " +
                         std::to_string(coordinator_config.low_floor >> 20) + " MiB"
                   : std::string{"off"}));
        swap_worker.emplace([this](std::stop_token stop) { RunPermafrost(stop); });
    }
}

Jadyx::MemoryCoordinator::Plan SessionMemory::ReadPlan() {
    std::scoped_lock lock{memory_plan.mutex};
    return memory_plan.plan;
}

SessionMemory::JitMarks SessionMemory::CurrentJitMarks() {
    if (!jit_after_swap) {
        // ...and the cap is lifted once 1 GiB is free again.
        return {768ULL << 20, 384ULL << 20, 1024ULL << 20};
    }
    const auto plan = ReadPlan();
    return {plan.jit_stop, plan.jit_release, plan.jit_uncap};
}

void SessionMemory::RunHostTrim(std::stop_token stop) {
    Common::SetCurrentThreadName("HostTrim");
    auto& dram = system.DeviceMemory().buffer;
    size_t cursor = 0;
    u64 pass_released = TRIM_ACTIVE_PASS;
    u64 last_pass_released = TRIM_ACTIVE_PASS;
    while (!stop.stop_requested()) {
        if (trim_paused.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const size_t before = cursor;
        const auto slice_start = std::chrono::steady_clock::now();
        const auto trim = dram.Trim(cursor, TRIM_SLICE);
        const auto slice_time = std::chrono::steady_clock::now() - slice_start;
        pass_released += trim.released;
        g_host_trim_visited.fetch_add(trim.visited, std::memory_order_relaxed);
        g_host_trim_released.fetch_add(trim.released, std::memory_order_relaxed);
        if (cursor <= before) {
            last_pass_released = pass_released;
            pass_released = 0;
        }
        if (trim.visited != 0 && trim.released >= trim.visited / 4) {
            std::this_thread::sleep_for((std::max)(
                std::chrono::steady_clock::duration{std::chrono::milliseconds(2)},
                slice_time / 2));
            continue;
        }
        // Short of memory the passes run back to back while they give memory back. A
        // pass that found little slows down: reading the whole emulated DRAM every
        // ~1.5 s took a tenth of a core and the cache the guest cores share with it.
        const bool low = trim_low.load(std::memory_order_relaxed);
        const bool active = last_pass_released >= TRIM_ACTIVE_PASS;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(low ? (active ? 20 : 100) : active ? 100 : 500));
    }
}

void SessionMemory::RunPermafrost(std::stop_token stop) {
    Common::SetCurrentThreadName("Permafrost");
    Jadyx::MemoryCoordinator coordinator{coordinator_config};
    g_memory_coordinator_state.texture_floor.store(u64{config.texture_cap_mib} << 20,
                                                   std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    Jadyx::MemoryCoordinator::Plan plan = ReadPlan();
    u64 logged_relief = 0;
    while (!stop.stop_requested()) {
        u64 used{}, limit{};
        if (!QueryAppMemory(used, limit)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        const u64 headroom = limit > used ? limit - used : 0;
        if (config.memory_coordinator) {
            u64 texture_heap{}, texture_cap{};
            D3D12::GetTextureMemoryUse(texture_heap, texture_cap);
            const bool was_yielding = plan.yield;
            plan = coordinator.Update(
                {static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count()),
                 headroom, Common::Permafrost::GetStats().fault_stall_us,
                 texture_heap, g_memory_coordinator_state.texture_floor.load(std::memory_order_relaxed),
                 texture_cap});
            D3D12::SetTextureMemoryLine(plan.texture_line);
            D3D12::SetTextureYield(plan.yield);
            {
                std::scoped_lock lock{memory_plan.mutex};
                memory_plan.plan = plan;
            }
            auto& state = g_memory_coordinator_state;
            state.yield.store(plan.yield, std::memory_order_relaxed);
            state.relief.store(plan.relief, std::memory_order_relaxed);
            state.stall_us_per_s.store(plan.stall_us_per_s, std::memory_order_relaxed);
            state.texture_line.store(plan.texture_line, std::memory_order_relaxed);
            state.texture_loan.store(plan.texture_loan, std::memory_order_relaxed);
            constexpr u64 LOG_RELIEF_STEP = 128ULL << 20;
            if (plan.yield != was_yielding ||
                plan.relief + LOG_RELIEF_STEP <= logged_relief ||
                plan.relief >= logged_relief + LOG_RELIEF_STEP ||
                (plan.relief == 0 && logged_relief != 0)) {
                logged_relief = plan.relief;
                WriteDiag(std::string{"memory coordinator: "} +
                          (plan.yield ? "yield" : "calm") + ", guest stall " +
                          std::to_string(plan.stall_us_per_s / 1000) +
                          " ms/s, relief " + std::to_string(plan.relief >> 20) +
                          " MiB (permafrost low " + std::to_string(plan.low >> 20) +
                          ", high " + std::to_string(plan.high >> 20) +
                          "), texture line " +
                          std::to_string(plan.texture_line >> 20) + " MiB, " +
                          std::to_string(headroom >> 20) + " MiB free");
            }
        }
        // The textures' loan is headroom they give back on their own: Permafrost does
        // not write the guest to disk for it. Urgent and below count only what is free.
        const u64 counted = headroom + plan.texture_loan;
        if (headroom < plan.urgent || counted < plan.low) {
            using Pressure = Common::GuestSwapPressure;
            const Pressure pressure =
                headroom < plan.emergency || Common::GuestSwapReserveReleased()
                    ? Pressure::Emergency
                : headroom < plan.urgent ? Pressure::Urgent
                                         : Pressure::Low;
            const u64 base = pressure == Pressure::Low ? counted : headroom;
            g_guest_swap_released.fetch_add(
                Common::StepGuestSwap(plan.high > base ? plan.high - base : 0,
                                      pressure),
                std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (counted < plan.watch) {
            g_guest_swap_released.fetch_add(
                Common::StepGuestSwap(0, Common::GuestSwapPressure::Watch),
                std::memory_order_relaxed);
        }
        if (headroom >= plan.high) {
            Common::RefillGuestSwapReserve();
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(counted < plan.watch ? 100 : 250));
    }
}

void SessionMemory::Tick(u32 tick, bool menu_open) {
    // The free memory under each limit every 10 s; the nearest one is logged each minute,
    // and every 10 s while it has less than 768 MiB free.
    if (tick % (10 * TICKS_PER_SECOND) == 0) {
        const Common::MemoryCeilings ceilings = QueryMemoryCeilings();
        ceiling_trends.Add(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count()),
            ceilings);
        const auto nearest = Common::NearestCeiling(ceilings);
        if (tick % TICKS_PER_MINUTE == 0 ||
            (nearest && ceilings[*nearest].Free() < (768ULL << 20))) {
            WriteDiag("memory ceiling: " +
                      Common::DescribeMemoryCeilings(ceilings, ceiling_trends));
        }
    }
    if (u64 used{}, limit{}; config.memory_audit && tick % 10 == 0 &&
                             QueryAppMemory(used, limit)) {
        const u64 headroom = limit > used ? limit - used : 0;
        if (headroom >= 2 * LOW_HEADROOM) {
            low_snapshot_headroom = ~u64{};
        } else if (headroom < LOW_HEADROOM &&
                   headroom + LOW_HEADROOM_STEP <= low_snapshot_headroom &&
                   tick - low_snapshot_tick >= 2 * TICKS_PER_SECOND) {
            low_snapshot_headroom = headroom;
            low_snapshot_tick = tick;
            WriteDiag("low headroom " + std::to_string(headroom >> 20) +
                      " MiB: memory by owner: " + MemoryOwners());
            WriteDiag("low headroom heaps: " + HeapReport());
            WriteDiag("low headroom memory map: " + LargestAllocations());
        }
    }
    if (u64 used{}, limit{}; tick % TICKS_PER_SECOND == 0 && QueryAppMemory(used, limit)) {
        const u64 headroom = limit > used ? limit - used : 0;
        if (prewarm_budget.Observe(headroom)) {
            SavePrewarmStep(program_id, static_cast<std::int32_t>(prewarm_budget.Step()));
            WriteDiag("JIT prewarm budget lowered to " +
                      std::to_string(prewarm_budget.MiBPerCore()) +
                      " MiB per core for this game's next session | " + MemoryReport());
        }
        // Short of memory, the JIT stops committing: each cache keeps the pages it has
        // and is cleared when it fills them. Shorter, the larger caches are cleared and
        // give memory back. Recompiling costs less than running out.
        const JitMarks jit = CurrentJitMarks();
        if (headroom < jit.release && !Dynarmic::CodeCacheCapReleases()) {
            Dynarmic::SetCodeCacheCap(JIT_RELEASE_CAP, true);
            WriteDiag("JIT code caches give memory back past " +
                      std::to_string(JIT_RELEASE_CAP >> 20) + " MiB | " + MemoryReport());
        } else if (headroom < jit.stop && Dynarmic::CodeCacheCap() == 0) {
            Dynarmic::SetCodeCacheCap(JIT_CAP_FLOOR);
            WriteDiag("JIT code caches stop growing | " + MemoryReport());
        } else if (headroom >= jit.uncap && Dynarmic::CodeCacheCap() != 0) {
            // A load burst is over and the caches it froze would otherwise stay frozen
            // for the whole session, clearing and recompiling the game's hot code.
            Dynarmic::SetCodeCacheCap(0);
            WriteDiag("JIT code caches grow again | " + MemoryReport());
        }
    }
    if (config.host_trim) {
        trim_paused.store(menu_open, std::memory_order_relaxed);
        if (u64 used{}, limit{}; tick % TICKS_PER_SECOND == 0 &&
                                 QueryAppMemory(used, limit)) {
            // A yield runs the passes back to back too: the guest stalls on reads back.
            trim_low.store((limit > used ? limit - used : 0) < TRIM_LOW_HEADROOM ||
                               ReadPlan().yield,
                           std::memory_order_relaxed);
        }
    }
    if (config.pool_trim && tick % TICKS_PER_SECOND == 0) {
        u64 used{}, limit{};
        const bool low = (QueryAppMemory(used, limit) &&
                          (limit > used ? limit - used : 0) < POOL_TRIM_ALL_HEADROOM) ||
                         ReadPlan().yield;
        TrimFreeGuestMemory(system, low ? ~u64{} : POOL_TRIM_PER_SECOND);
    }
}

void SessionMemory::Report(u32 tick) {
    if (config.memory_audit && tick % (10 * TICKS_PER_SECOND) == 0) {
        WriteDiag("guest memory: " + GuestMemoryReport(system));
    }
    if (config.memory_audit && tick % TICKS_PER_MINUTE == 0) {
        WriteDiag("heaps: " + HeapReport());
        if (const auto census = Common::CensusGuestZeroMemory()) {
            WriteDiag("guest memory census: " + Common::DescribeGuestZeroCensus(*census));
        }
        WriteDiag("memory map: " + LargestAllocations());
        WriteDiag("memory by owner: " + MemoryOwners());
        if (const std::string pso_cost = D3D12::DescribePsoCost(); !pso_cost.empty()) {
            WriteDiag(pso_cost);
        }
    }
    if (tick % (10 * TICKS_PER_MINUTE) == 0) {
        WriteDiag("step: playing, " + std::to_string(tick / TICKS_PER_MINUTE) +
                  " min | " + MemoryReport());
        WriteDiag("memory map: " + LargestAllocations());
    }
}

} // namespace Jadyx
