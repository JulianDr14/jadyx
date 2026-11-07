// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit: searches over the guest's memory, without moving it. Nothing faults, so nothing
// comes back from the swap, no page gets committed and no granule looks used. First what is in memory,
// with the backing's decommit mutex held a slice at a time so an emergency waits one slice at most;
// then what Permafrost has on disk, if anything.
//
// A first search looks at every page, with a bitmap with a bit per page to count each page once, as a
// page may move while the search runs. A refined search looks only at the pages of the last list and
// keeps the addresses that hold the new number now.

#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>

#include "common/common_types.h"
#include "jadyx/memory_edit/jme_results.h"
#include "jadyx/memory_edit/jme_value.h"

namespace Jadyx::MemoryEdit {

/// Guest pages contiguous in both address spaces. Their bits in the bitmap start at `first_mark`.
struct GuestSpan {
    u64 va{};
    const u8* host{};
    u32 pages{};
    u32 first_mark{};
};

enum class ScanPhase : u8 { Memory, Disk };

/// What a search is at, for whoever waits for it.
struct ScanProgress {
    std::atomic<ScanPhase> phase{ScanPhase::Memory};
    /// Of the phase, out of 1000.
    std::atomic<u32> permille{};
};

struct ScanParams {
    /// By address.
    std::span<const GuestSpan> spans;
    /// A refined search keeps a candidate only if its kind is here.
    QuerySet queries;
    /// Threads looking through memory, the caller among them. Fewer for the disk.
    u32 workers{1};
    std::mutex& decommit_mutex;
    ResultFile& file;
    /// Stops the search as soon as the threads see it: what was found stays. Optional.
    const std::atomic<bool>* cancel{};
    /// Optional.
    ScanProgress* progress{};
};

enum class ScanFailure : u8 { None, NoMemory, TooManyResults, DiskError };

struct ScanStats {
    u64 memory_bytes{}; ///< Compared in memory, the first pass.
    u64 disk_bytes{};   ///< Compared from the swap file.
    u64 sweep_bytes{};  ///< Compared in memory on the second pass: what came back meanwhile.
    u64 memory_ms{};
    u64 disk_ms{};
    u64 sweep_ms{};
    u64 hits[VALUE_KINDS]{};
    u64 disk_hits{}; ///< Those of `hits` found in the swap file.
    bool memory_complete{}; ///< The pass over memory saw all it had to.
    bool cancelled{};
    ScanFailure failure{};

    [[nodiscard]] u64 TotalHits() const {
        u64 total = 0;
        for (const u64 kind_hits : hits) total += kind_hits;
        return total;
    }
};

/// Looks at every page. Runs on the calling thread, which helps. Appends to `list`.
ScanStats RunScan(const ScanParams& params, ResultList& list);

/// Looks at the candidates in `previous`, read from the same file, which writes to a range apart from
/// them. An address no longer mapped is dropped, and a page in neither memory nor the swap reads as
/// zeros, as it does for the guest. Appends to `list`.
ScanStats RunRefine(const ScanParams& params, const ResultList& previous, ResultList& list);

} // namespace Jadyx::MemoryEdit
