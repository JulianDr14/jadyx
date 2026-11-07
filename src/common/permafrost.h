// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include "common/common_types.h"

// Permafrost: cold guest memory frozen to disk, for the demand-committed private DRAM backing
// (Windows only).
//
// The backing is cut into 2 MiB granules. Windows has no accessed bit, so a scout samples them
// instead: once a round it makes one random committed page of each resident granule no-access,
// and the fault on it says the granule is still in use. Short of memory, the granules idle the
// longest go on probation: their committed pages turn no-access, and the first access restores
// them. A granule nobody touched for the probation time goes to a writer thread: its pages turn
// read-only (writers wait), the pages holding data are written to a slot of the swap file, and
// every committed page is decommitted. All-zero pages are only decommitted.
//
// A fault in a granule on disk reads back a window of it around the fault, wider when the faults
// walk forward; the rest stays on disk until touched. A granule read back soon after it went out
// was hot: it is shielded from the next evictions for longer each time, the refaults make the
// swapper wait for older victims, and a hot granule is read back whole by a prefetch thread.
//
// Kernel calls do not fault: a system call reading or writing a no-access or decommitted page
// fails with ERROR_NOACCESS instead of entering the handler. Guest memory reaches the system only
// through copies, as it did when untouched pages were merely reserved.
namespace Common::Permafrost {

constexpr size_t GRANULE_SIZE = 2 * 1024 * 1024;
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t GRANULE_PAGES = GRANULE_SIZE / PAGE_SIZE;

/// The memory calls of the platform: the console needs the *FromApp entry points.
struct Platform {
    /// Commits [address, address + size) read-write.
    bool (*commit)(void* address, size_t size);
    /// Changes the protection of committed pages.
    bool (*protect)(void* address, size_t size, u32 protection);
    /// Commits a block of its own, for the reserve and the IO buffers.
    void* (*allocate)(size_t size);
    /// Names the calling thread. Optional.
    void (*name_thread)(const char* name){};
};

struct Config {
    /// The swap file. It is created empty and deleted when the last handle closes.
    std::wstring path;
    /// Commit held back and given up when a demand commit fails, so the commit is retried.
    u64 reserve_bytes{};
    /// How long a granule on probation must stay untouched before it is written out.
    u32 probation_ms{1500};
    /// How long the scout must have seen a granule idle before it goes on probation. Raised while
    /// granules come back soon after they went out.
    u32 min_idle_ms{8000};
    /// The scout's round.
    u32 scout_ms{1000};
    /// A granule read back within this time of going out was evicted hot.
    u32 quick_refault_ms{30000};
    /// How long such a granule is left alone: doubled for each quick refault, up to the cap. A
    /// granule touched on probation is left alone for the base time.
    u32 shield_ms{15000};
    u32 shield_max_ms{240000};
    /// Threads writing granules out.
    u32 writers{2};
    /// What goes out in one batch: at most this much is put on probation each half second and at
    /// most this much is with the writers at once, twice that when far short. An emergency is not
    /// held back. 0 sends everything the shortfall asks for at once.
    u64 batch_bytes{64ULL << 20};
    /// Pages a read back commits and fills at a time. A committed page is visible zero to every
    /// thread until it is filled, so fewer is a shorter window.
    u32 fill_pages{4};
    /// Whether hot granules are read back whole ahead of the faults.
    bool prefetch{true};
    /// Whether the guest threads are paused while a read back commits and fills its pages, so
    /// none of them sees one still zero. fill_pages does not apply then: each run goes at once.
    bool pause_fill{true};
};

/// The calling thread runs guest code: it is paused while a read back fills pages. Unregistered
/// before it exits.
void RegisterGuestThread();
void UnregisterGuestThread();

/// Takes over [base, base + size), which must be 2 MiB aligned in size. Fails without a swap file.
bool Init(u8* base, size_t size, const Platform& platform, const Config& config);
/// Stops the threads, closes and deletes the swap file with what it holds: called when the backing
/// goes away.
void Shutdown();
[[nodiscard]] bool Active();

/// Exception handler entry for an access violation at `address` inside the backing. `commit` is
/// the ordinary demand commit of a reserved page. Returns true when the access can be retried.
/// Allocates nothing.
bool OnFault(u8* address, bool (*commit)(u8* address));

/// The pages [begin, end) are about to be decommitted (cleared or freed): what the swap file holds
/// for them is dropped. Called with the backing's decommit mutex held.
void ForgetPages(u8* begin, u8* end);

/// Whether the granule of `address` is resident: neither on probation, on disk nor moving. Read
/// with the decommit mutex held, it stays so until the mutex is released.
[[nodiscard]] bool IsResident(const u8* address);

/// The end of the reserved run [begin, end) the guest may read as zero: the first granule of it
/// with data on disk ends it. `end` when nothing in it is on disk.
[[nodiscard]] const u8* ClipUntouched(const u8* begin, const u8* end);

/// Sees `count` contiguous pages, from page `first_page` of the inspected range, at `data`. It may
/// touch nothing of the backing but `data`: a fault on a granule its caller holds never returns.
using PageVisitor = void (*)(void* context, size_t first_page, size_t count, const u8* data);

/// Shows the committed pages of [begin, begin + pages * PAGE_SIZE) without moving anything: no fault,
/// no read back, and no granule looks used. Pages on probation or holding a probe open read-only
/// meanwhile and close again. Faults in the granule being read wait for it. Called with the decommit
/// mutex held, which `visit` must not take.
void InspectCommitted(const u8* begin, size_t pages, PageVisitor visit, void* context);

/// Shows what the swap file holds for the same range, read into a buffer: nothing is committed.
/// Takes no lock. False when a read failed; the pages seen before it were shown. Each page is shown
/// once, and a page read back or evicted meanwhile belongs to InspectCommitted.
[[nodiscard]] bool InspectSwapped(const u8* begin, size_t pages, PageVisitor visit, void* context);

enum class Urgency : u8 {
    Watch,     // not short yet: the scout samples, nothing goes out
    Normal,    // short: idle granules go out
    Urgent,    // far short: shorter probation and idle time
    Emergency, // about to fail commits: the coldest granules go out, shielded or not
};

struct StepResult {
    /// Committed bytes the writers gave back since the last step.
    u64 released{};
    /// Granules handed to the writers and granules put on probation by this step.
    u32 queued{};
    u32 probation{};
};

/// One pass of the swapper: runs the scout when its round is due and, short of memory, tries to
/// give back `want` bytes: the granules whose probation ran out go to the writers, and more go on
/// probation when those are not enough. Takes `decommit_mutex` around each granule it moves.
StepResult Step(u64 want, Urgency urgency, std::mutex& decommit_mutex);

/// Waits until the writers and the prefetch thread have nothing left to do.
void Settle();

/// Gives the reserve back to the system. True when it was held.
bool ReleaseReserve();
/// Commits the reserve again after it was given up. True when it is held.
bool RefillReserve();
[[nodiscard]] bool ReserveReleased();

struct Stats {
    u64 swapped_bytes{};      // data on disk now
    u64 probation_bytes{};    // committed bytes on probation now
    u64 writing_bytes{};      // committed bytes handed to the writers and not given back yet
    u64 written_bytes{};      // written to the file in total
    u64 zero_bytes{};         // zero pages decommitted instead of written
    u64 read_bytes{};         // read back in total
    u64 swap_ins{};           // reads back, whole or windows
    u64 window_reads{};       // reads back that left part of the granule on disk
    u64 prefetch_reads{};     // reads back by the prefetch thread
    u64 swap_in_us{};         // total time of the reads back in faults
    u64 swap_in_max_us{};
    u64 zero_faults{};        // zero pages committed alone in a granule on disk
    u64 probation_starts{};
    u64 probation_touches{};  // granules touched while on probation
    u64 evictions{};          // granules written out
    u64 refaults{};           // first reads back after a write-out
    u64 quick_refaults{};     // of those, soon after it
    u64 probe_arms{};
    u64 probe_hits{};
    u64 write_failures{};
    u64 reserve_releases{};
    u64 file_slots{};         // slots the file has grown to
    u64 fault_stall_us{};     // total time guest threads waited in faults: reads back, restores
                              // from probation and granules held by another thread
    u32 min_idle_ms{};        // the idle time asked of a victim now
    u32 shielded{};           // granules left alone now
};
[[nodiscard]] Stats GetStats();
/// The stats in one line. `since_last` adds the stall per second since the last such call: one
/// caller only, at a steady interval.
[[nodiscard]] std::string Describe(bool since_last = false);
/// Diagnostic, for a crash: the granule of `address` and its latest reads back. Empty outside the
/// swapped range.
[[nodiscard]] std::string DescribePage(const u8* address);
/// Diagnostic, for a crash: the latest `count` reads back of any granule, newest first.
[[nodiscard]] std::string DescribeRecentReads(size_t count);

} // namespace Common::Permafrost
