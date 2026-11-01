// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A played game's memory: the workers that give it back (HostTrim, Permafrost and the memory
// coordinator behind it), the JIT caches' caps, and the reports. Destroying this stops the workers.

#include <atomic>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "common/common_types.h"
#include "common/memory_ceiling.h"
#include "jadyx/app/memory_coordinator.h"
#include "jadyx/app/prewarm_budget.h"

namespace Core {
class System;
}

namespace Jadyx {

struct BootConfig;

/// The play loop's clock: it wakes every 20 ms.
constexpr u32 TICKS_PER_MINUTE = 3000;
constexpr u32 TICKS_PER_SECOND = TICKS_PER_MINUTE / 60;

class SessionMemory {
public:
    SessionMemory(Core::System& system_, const BootConfig& config_, u64 program_id_,
                  PrewarmBudget& prewarm_budget_);

    /// Before the session's failure checks: reads the limits, caps the JIT, trims.
    void Tick(u32 tick, bool menu_open);
    /// The periodic reports, at the end of a tick.
    void Report(u32 tick);

private:
    struct JitMarks {
        u64 stop, release, uncap;
    };

    Jadyx::MemoryCoordinator::Plan ReadPlan();
    JitMarks CurrentJitMarks();
    void RunHostTrim(std::stop_token stop);
    void RunPermafrost(std::stop_token stop);

    Core::System& system;
    const BootConfig& config;
    const u64 program_id;
    PrewarmBudget& prewarm_budget;

    const bool jit_after_swap;
    Jadyx::MemoryCoordinator::Config coordinator_config;
    // The Permafrost worker publishes the plan; the JIT and the trims below read it.
    struct {
        std::mutex mutex;
        Jadyx::MemoryCoordinator::Plan plan;
    } memory_plan;
    u64 low_snapshot_headroom = ~u64{};
    u32 low_snapshot_tick = 0;
    Common::MemoryCeilingTrends ceiling_trends;
    std::atomic<bool> trim_low{false};
    std::atomic<bool> trim_paused{false};
    // Last: they read everything above. Permafrost stops first.
    std::optional<std::jthread> trim_worker;
    std::optional<std::jthread> swap_worker;
};

} // namespace Jadyx
