// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The app's own logs, which start before Eden's logging and survive an early crash, and the reports
// written to them.

#include <cstdint>
#include <string>

#include "common/common_types.h"
#include "common/memory_ceiling.h"

namespace Core {
class System;
}

namespace Jadyx {

/// Milliseconds since launch, the stamp on every line of these logs.
unsigned long long ElapsedMs();
void WriteDiag(const std::string& msg);
void WritePulseLog(const std::string& msg); ///< jadyx_pulse.txt
/// For the crash handlers: no WinRT, no lock, no allocation beyond what fopen needs.
void WriteDiagRaw(const char* line, bool debugger_channel = true);
/// One line of jadyx_pulse.txt per different rejection, from the first-chance logger.
void WritePulseRejection(const char* what);

std::string CpuSetReport();
std::string MemoryReport();
std::string StorageReport();
std::string MemoryOwners();
std::string GuestMemoryReport(Core::System& system);
std::string LargestAllocations();
std::string HeapReport();

/// A game's learned JIT prewarm step (Jadyx::PrewarmBudget), 0 when none is stored.
std::int32_t LoadPrewarmStep(u64 program_id);
void SavePrewarmStep(u64 program_id, std::int32_t step);

u64 TrimFreeGuestMemory(Core::System& system, u64 budget);
void ApplyProcessMemoryLimit(u32 limit_mib);
[[noreturn]] void RestartApp();
bool QueryAppMemory(u64& used, u64& limit);
Common::MemoryCeilings QueryMemoryCeilings();

} // namespace Jadyx
