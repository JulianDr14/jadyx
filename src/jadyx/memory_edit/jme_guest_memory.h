// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit: the guest memory a game keeps its values in, as spans for the scanner, and one
// value read or written.

#include <optional>
#include <vector>

#include "common/common_types.h"
#include "jadyx/memory_edit/jme_scanner.h"
#include "jadyx/memory_edit/jme_value.h"

namespace Core {
class System;
}

namespace Jadyx::MemoryEdit {

struct GuestMemoryMap {
    std::vector<GuestSpan> spans;
    u64 pages{};
};

/// The game's heap and data, writable: no code, stack, IPC or shared memory. Each span ends at a 2 MiB
/// boundary of the host, so threads share the work evenly. With the guest paused, from a thread the
/// kernel knows, as it takes the page table's lock.
[[nodiscard]] GuestMemoryMap WalkGuestMemory(Core::System& system);

/// Whether the guest's memory is the kind the scanner can look through without moving it.
[[nodiscard]] bool CanSearchGuest(Core::System& system);

/// The value at `va`, which must be aligned to its kind. Empty when the address is not mapped.
[[nodiscard]] std::optional<u64> ReadGuestValue(Core::System& system, u64 va, ValueKind kind);

/// Changes it. False when the address is not mapped.
[[nodiscard]] bool WriteGuestValue(Core::System& system, u64 va, u64 bits, ValueKind kind);

} // namespace Jadyx::MemoryEdit
