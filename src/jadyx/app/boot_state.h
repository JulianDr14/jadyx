// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the UI thread, the heartbeat and a game's session share while the game runs.

#include <atomic>
#include <string>

#include "common/common_types.h"

namespace Jadyx {

inline std::atomic<u64> g_test_memory_limit{};
inline std::atomic<bool> g_gpu_command_failed{};
/// Guest DRAM bytes the HostTrim worker read and gave back since the last heartbeat.
inline std::atomic<u64> g_host_trim_visited{};
inline std::atomic<u64> g_host_trim_released{};
/// Guest DRAM bytes the GuestSwap worker gave back since the last heartbeat.
inline std::atomic<u64> g_guest_swap_released{};
/// The memory coordinator's plan as the Permafrost worker last published it, for the heartbeat.
struct MemoryCoordinatorState {
    std::atomic<bool> yield{};
    std::atomic<u64> relief{}, stall_us_per_s{}, texture_line{}, texture_loan{};
    std::atomic<u64> texture_floor{}; ///< The texture cap; the in-game menu can change it.
    std::string Describe() const {
        return std::string{yield.load(std::memory_order_relaxed) ? "yield" : "calm"} +
               ", guest stall " +
               std::to_string(stall_us_per_s.load(std::memory_order_relaxed) / 1000) +
               " ms/s, relief " + std::to_string(relief.load(std::memory_order_relaxed) >> 20) +
               " MiB, texture line " +
               std::to_string(texture_line.load(std::memory_order_relaxed) >> 20) +
               " MiB, texture loan " +
               std::to_string(texture_loan.load(std::memory_order_relaxed) >> 20) + " MiB";
    }
};
inline MemoryCoordinatorState g_memory_coordinator_state;
/// The PC window's mode. ApplicationView belongs to the UI thread: it publishes the state here and
/// applies the changes the in-game menu asks for (1 full screen, 0 windowed, -1 none).
inline std::atomic<bool> g_can_fullscreen{};
inline std::atomic<bool> g_fullscreen{};
inline std::atomic<int> g_fullscreen_request{-1};

} // namespace Jadyx
