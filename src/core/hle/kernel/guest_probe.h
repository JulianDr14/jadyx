// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

#include "common/common_types.h"

namespace Core {
class ArmInterface;
}

namespace Kernel {

class KernelCore;
class KThread;

/// What the guest is doing while the game shows no frame: each emulated core records the guest
/// thread it runs (registers, call stack, the code and the memory it reads) the next time it
/// leaves guest code, and every guest thread of the application is listed with what it waits on.
/// Nothing runs until a report is asked for: a running core only reads one atomic per JIT exit.
namespace GuestProbe {

namespace Detail {
/// Bit n: core n should record its guest state.
extern std::atomic<u32> pending_cores;
void Record(std::size_t core, Core::ArmInterface& interface, KThread& thread);
} // namespace Detail

/// Called by a core right after the JIT returns, while the thread's state is still loaded.
inline void OnGuestExit(std::size_t core, Core::ArmInterface& interface, KThread& thread) {
    if ((Detail::pending_cores.load(std::memory_order_relaxed) & (1u << core)) != 0) [[unlikely]] {
        Detail::Record(core, interface, thread);
    }
}

/// Interrupts the cores `rounds` times, `gap` apart, and writes what they ran and every guest
/// thread of the application. A core that does not leave guest code within a round (stuck in a
/// system call, or idle) is reported as such. Blocks the caller for up to rounds * (gap + 250 ms)
/// plus the time to read the guest's memory.
[[nodiscard]] std::string Report(KernelCore& kernel, int rounds, std::chrono::milliseconds gap);

} // namespace GuestProbe
} // namespace Kernel
