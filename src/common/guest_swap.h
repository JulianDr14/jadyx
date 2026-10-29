// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include "common/common_types.h"

// Cold guest memory on disk, for the demand-committed private DRAM backing (Windows only).
//
// The backing is cut into 2 MiB granules. Short of memory, a granule with committed pages goes on
// probation: its committed pages turn no-access, and the first access restores them, so a granule
// still in use costs one fault. A granule nobody touched for the probation time is written out:
// its pages turn read-only (writers wait), the pages holding data are written to a slot of the swap
// file, and every committed page is decommitted. All-zero pages are only decommitted. The first
// access to a swapped granule commits its pages and reads them back.
//
// Kernel calls do not fault: a system call reading or writing a no-access or decommitted page
// fails with ERROR_NOACCESS instead of entering the handler. Guest memory reaches the system only
// through copies, as it did when untouched pages were merely reserved.
namespace Common::GuestSwap {

constexpr size_t GRANULE_SIZE = 2 * 1024 * 1024;
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t GRANULE_PAGES = GRANULE_SIZE / PAGE_SIZE;

/// The memory calls of the platform: the console needs the *FromApp entry points.
struct Platform {
    /// Commits [address, address + size) read-write.
    bool (*commit)(void* address, size_t size);
    /// Changes the protection of committed pages.
    bool (*protect)(void* address, size_t size, u32 protection);
    /// Commits a block of its own, for the reserve.
    void* (*allocate)(size_t size);
};

struct Config {
    /// The swap file. It is created empty and deleted when the last handle closes.
    std::wstring path;
    /// Commit held back and given up when a demand commit fails, so the commit is retried.
    u64 reserve_bytes{};
    /// How long a granule on probation must stay untouched before it is written out.
    u32 probation_ms{1000};
};

/// Takes over [base, base + size), which must be 2 MiB aligned in size. Fails without a swap file.
bool Init(u8* base, size_t size, const Platform& platform, const Config& config);
/// Closes and deletes the swap file with what it holds: called when the backing goes away.
void Shutdown();
[[nodiscard]] bool Active();

/// Exception handler entry for an access violation at `address` inside the backing. `commit` is
/// the ordinary demand commit of a reserved page. Returns true when the access can be retried.
/// Allocates nothing.
bool OnFault(u8* address, bool (*commit)(u8* address));

/// The pages [begin, end) are about to be decommitted (cleared or freed): what the swap file holds
/// for them is dropped. Called with the backing's decommit mutex held.
void ForgetPages(u8* begin, u8* end);

/// Whether the granule of `address` is resident: neither on probation, swapped nor moving. Read
/// with the decommit mutex held, it stays so until the mutex is released.
[[nodiscard]] bool IsResident(const u8* address);

/// The end of the reserved run [begin, end) the guest may read as zero: the first granule of it
/// whose data is on disk ends it. `end` when nothing in it is swapped.
[[nodiscard]] const u8* ClipUntouched(const u8* begin, const u8* end);

struct StepResult {
    /// Committed bytes given back.
    u64 released{};
    /// Granules written out and granules only decommitted.
    u32 swapped{};
    u32 zero{};
    /// Granules put on probation.
    u32 probation{};
};

/// One pass of the swapper: tries to give back `want` bytes, writing out the granules whose
/// probation ran out and putting more on probation when those are not enough. `urgent` shortens
/// the probation. Takes `decommit_mutex` around each granule.
StepResult Step(u64 want, bool urgent, std::mutex& decommit_mutex);

/// Gives the reserve back to the system. True when it was held.
bool ReleaseReserve();
/// Commits the reserve again after it was given up. True when it is held.
bool RefillReserve();
[[nodiscard]] bool ReserveReleased();

struct Stats {
    u64 swapped_bytes{};      // data on disk now
    u64 probation_bytes{};    // committed bytes on probation now
    u64 written_bytes{};      // written to the file in total
    u64 zero_bytes{};         // zero pages decommitted instead of written
    u64 read_bytes{};         // read back in total
    u64 swap_ins{};
    u64 swap_in_us{};         // total time of the reads back
    u64 swap_in_max_us{};
    u64 probation_starts{};
    u64 probation_touches{};  // granules touched while on probation
    u64 write_failures{};
    u64 reserve_releases{};
    u64 file_slots{};         // slots the file has grown to
};
[[nodiscard]] Stats GetStats();
[[nodiscard]] std::string Describe();

} // namespace Common::GuestSwap
