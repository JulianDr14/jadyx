// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit: one player's search, from the first number to the address left. It outlives the
// menu, as the player goes back to the game between numbers, and holds almost nothing meanwhile: a
// line per chunk of the list, with the addresses in a file. The threads exist while a search runs,
// and the menu asks how it goes rather than waiting for it. A search stops and is waited for before
// the guest runs again.

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "jadyx/memory_edit/jme_guest_memory.h"
#include "jadyx/memory_edit/jme_results.h"
#include "jadyx/memory_edit/jme_scanner.h"
#include "jadyx/memory_edit/jme_value.h"

namespace Jadyx::MemoryEdit {

/// Above this many results the list is not for browsing: it shows the first ones and asks to refine.
inline constexpr u64 MAX_BROWSABLE_RESULTS = 100'000;
inline constexpr u32 SHOWN_WHEN_TOO_MANY = 100;

/// Why a search did not start.
enum class SearchStart : u8 {
    Started,
    Busy,      ///< One is running.
    BadNumber, ///< Not a number, or no kind can hold it.
    Zero,      ///< A first search for 0: it would match all the empty memory. Refining to 0 is fine.
    NoList,    ///< Nothing to refine.
    NoMemory,  ///< The guest has no memory to look through.
    NoFile,    ///< The result file could not be opened.
};

/// How the last search ended, once Poll says it did.
enum class SearchEnd : u8 {
    None,
    Done,
    Partial,        ///< Stopped: what it found is the list.
    Cancelled,      ///< Stopped too early to mean anything: the list is what it was.
    NoMatches,      ///< Nothing holds that number. A refined search leaves the list as it was.
    NoMemory,       ///< The list is what it was, or empty for a first search; likewise below.
    TooManyResults,
    DiskError,
};

struct SearchProgress {
    ScanPhase phase{};
    u32 permille{};
};

struct ResultEntry {
    u64 va{};
    ValueKind kind{};
};

class Session {
public:
    struct Context {
        std::wstring result_path;
        std::mutex& decommit_mutex;
        /// Called on the thread that starts a search.
        std::function<GuestMemoryMap()> walk;
        /// What the file may take in all, and a first list out of it: a refined one goes beside it.
        u64 max_file_bytes = u64{256} * 1024 * 1024;
        u64 max_first_list_bytes = u64{128} * 1024 * 1024;
    };

    explicit Session(Context context);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Forgets the list and looks through all the memory. From the thread driving the menu, with the
    /// guest paused. Without a kind, the usual ones (see AutoQueries).
    [[nodiscard]] SearchStart Search(std::string_view number, std::optional<ValueKind> kind = std::nullopt);
    /// Keeps the addresses of the list that hold this number now. Same rules.
    [[nodiscard]] SearchStart Refine(std::string_view number);

    /// Asks a running search to stop: Poll tells when it did.
    void Cancel();
    /// Stops a running search and waits for it. Before the guest runs, and before anything goes away.
    void Stop();
    /// True once, when a search has just ended and its result is in place.
    bool Poll();
    /// Forgets the list and the file.
    void Clear();

    [[nodiscard]] bool Running() const {
        return thread.joinable();
    }
    [[nodiscard]] SearchProgress Progress() const;
    [[nodiscard]] SearchEnd LastEnd() const {
        return last_end;
    }
    /// Of the last search, even if its list was not kept.
    [[nodiscard]] const ScanStats& LastStats() const {
        return stats;
    }
    [[nodiscard]] bool HasList() const {
        return hits != 0;
    }
    [[nodiscard]] u64 Hits() const {
        return hits;
    }
    /// A search that made this list was stopped: the value may be in what it did not see.
    [[nodiscard]] bool IsPartial() const {
        return partial;
    }
    /// Too many results to browse: the player sees the first ones and is asked to refine.
    [[nodiscard]] bool MustRefine() const {
        return hits > MAX_BROWSABLE_RESULTS;
    }
    /// How many results the list shows.
    [[nodiscard]] u64 ShownHits() const {
        return MustRefine() ? SHOWN_WHEN_TOO_MANY : hits;
    }

    /// Up to `count` results from the `first`-th on, in the list's order. Reads the file: not while a
    /// search runs. False when it could not.
    [[nodiscard]] bool ReadEntries(u64 first, u32 count, std::vector<ResultEntry>& entries) const;

private:
    SearchStart Begin(const QuerySet& queries, bool refine);
    void Finish();

    Context context;
    ResultFile file;
    ResultList list;
    SlotRange list_range{};
    u64 hits{};
    bool partial{};
    SearchEnd last_end{};
    ScanStats stats;

    // Those of the search running, which the thread owns until Finish.
    bool refining{};
    GuestMemoryMap map;
    QuerySet queries_running;
    ResultList next_list;
    std::atomic<bool> cancel{};
    std::atomic<bool> ended{};
    ScanProgress progress;
    std::thread thread;
};

} // namespace Jadyx::MemoryEdit
