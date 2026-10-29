// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace D3D12::MemoryReserve {

/// Commits (and touches) a block of process memory held back for the driver: a pipeline object
/// that fails with E_OUTOFMEMORY releases it and is created again, instead of ending the session.
/// 0 releases it and keeps it off ("oom_reserve_mib=0").
void Configure(std::uint64_t bytes);

/// Releases the reserve. True if it held memory, so a failed allocation is worth retrying.
bool Release();

/// Commits the reserve again once it was released and the app is no longer short of memory.
/// False if it is still released (the allocation itself failed).
bool Rearm();

/// Bytes committed right now.
[[nodiscard]] std::uint64_t Held();

/// Pipeline objects that failed with E_OUTOFMEMORY this session, retried or not.
[[nodiscard]] std::uint64_t OutOfMemoryEvents();

/// Counts a pipeline object that failed with E_OUTOFMEMORY.
void NoteOutOfMemory();

} // namespace D3D12::MemoryReserve
