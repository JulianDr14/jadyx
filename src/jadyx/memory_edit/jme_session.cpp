// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/memory_edit/jme_session.h"

#include <bit>
#include <utility>

namespace Jadyx::MemoryEdit {

namespace {

// The menu thread stays free, and the guest is paused: most of the machine is there to be used. What
// the console measured: past 4 threads memory gained nothing, past 2 the disk neither.
constexpr u32 SEARCH_THREADS = 4;

// Every kind that can hold the number: a candidate keeps the kind it was found as.
QuerySet AllKinds(const Number& number) {
    QuerySet set;
    for (size_t kind = 0; kind < VALUE_KINDS; ++kind) {
        set.Add(MakeQuery(number, static_cast<ValueKind>(kind)));
    }
    return set;
}

SearchEnd EndOfFailure(ScanFailure failure) {
    switch (failure) {
    case ScanFailure::None:
        break;
    case ScanFailure::NoMemory:
        return SearchEnd::NoMemory;
    case ScanFailure::TooManyResults:
        return SearchEnd::TooManyResults;
    case ScanFailure::DiskError:
        return SearchEnd::DiskError;
    }
    return SearchEnd::Done;
}

} // namespace

Session::Session(Context context_) : context{std::move(context_)} {}

Session::~Session() {
    Stop();
}

SearchStart Session::Search(std::string_view number, std::optional<ValueKind> kind) {
    if (Running()) return SearchStart::Busy;
    const auto parsed = ParseNumber(number);
    if (!parsed) return SearchStart::BadNumber;
    if (parsed->value == 0.0) return SearchStart::Zero;
    QuerySet queries;
    if (kind) {
        queries.Add(MakeQuery(*parsed, *kind));
    } else {
        queries = AutoQueries(*parsed);
    }
    if (queries.count == 0) return SearchStart::BadNumber;
    return Begin(queries, false);
}

SearchStart Session::Refine(std::string_view number) {
    if (Running()) return SearchStart::Busy;
    if (!HasList()) return SearchStart::NoList;
    const auto parsed = ParseNumber(number);
    if (!parsed) return SearchStart::BadNumber;
    const QuerySet queries = AllKinds(*parsed);
    if (queries.count == 0) return SearchStart::BadNumber;
    return Begin(queries, true);
}

SearchStart Session::Begin(const QuerySet& queries, bool refine) {
    map = context.walk();
    if (map.spans.empty()) {
        map = {};
        return SearchStart::NoMemory;
    }
    if (refine) {
        file.BeginList(NextListRange(list_range, file.MaxSlots()));
    } else {
        Clear();
        if (!file.Open(context.result_path, context.max_file_bytes)) {
            map = {};
            return SearchStart::NoFile;
        }
        file.BeginList({0, static_cast<u32>(context.max_first_list_bytes / CHUNK_SIZE)});
    }
    refining = refine;
    queries_running = queries;
    next_list.clear();
    cancel.store(false);
    ended.store(false);
    progress.phase.store(ScanPhase::Memory, std::memory_order_relaxed);
    progress.permille.store(0, std::memory_order_relaxed);
    thread = std::thread([this] {
        const ScanParams params{map.spans,  queries_running, SEARCH_THREADS,
                                context.decommit_mutex, file, &cancel, &progress};
        stats = refining ? RunRefine(params, list, next_list) : RunScan(params, next_list);
        ended.store(true, std::memory_order_release);
    });
    return SearchStart::Started;
}

void Session::Cancel() {
    cancel.store(true);
}

void Session::Stop() {
    if (!Running()) return;
    Cancel();
    thread.join();
    Finish();
}

bool Session::Poll() {
    if (!Running() || !ended.load(std::memory_order_acquire)) return false;
    thread.join();
    Finish();
    return true;
}

void Session::Finish() {
    map = {};
    const u64 found = stats.TotalHits();
    bool keep{};
    if (stats.failure != ScanFailure::None) {
        last_end = EndOfFailure(stats.failure);
    } else if (stats.cancelled) {
        // A refined search stopped halfway through memory has not looked at most of the list. Once
        // past memory, stopping costs what is on disk, which is what the player chose to skip.
        keep = found != 0 && (!refining || stats.memory_complete);
        last_end = keep ? SearchEnd::Partial : SearchEnd::Cancelled;
    } else {
        keep = found != 0;
        last_end = keep ? SearchEnd::Done : SearchEnd::NoMatches;
    }
    if (keep) {
        list = std::move(next_list);
        list_range = file.ListRange();
        hits = found;
        partial = (refining && partial) || stats.cancelled;
    } else if (!refining) {
        Clear();
    }
    next_list = {};
}

void Session::Clear() {
    Stop();
    file.Close();
    list = {};
    list_range = {};
    hits = 0;
    partial = false;
}

SearchProgress Session::Progress() const {
    return {progress.phase.load(std::memory_order_relaxed), progress.permille.load(std::memory_order_relaxed)};
}

bool Session::ReadEntries(u64 first, u32 count, std::vector<ResultEntry>& entries) const {
    entries.clear();
    if (Running()) return false;
    if (count == 0 || first >= hits) return true;
    u8* const buffer = NewChunkBuffer();
    if (buffer == nullptr) return false;
    bool ok = true;
    u64 skipped = 0;
    ChunkRecord record;
    for (const ResultChunk& chunk : list) {
        if (entries.size() >= count) break;
        if (skipped + chunk.hits <= first) {
            skipped += chunk.hits;
            continue;
        }
        if (!file.Read(chunk.slot, buffer)) {
            ok = false;
            break;
        }
        ChunkReader reader{buffer};
        while (entries.size() < count && reader.Next(record)) {
            if (skipped + record.hits <= first) {
                skipped += record.hits;
                continue;
            }
            const u32 width = ValueWidth(record.kind);
            for (size_t word = 0; word < MatchWords(record.kind) && entries.size() < count; ++word) {
                for (u64 rest = record.bits[word]; rest != 0 && entries.size() < count; rest &= rest - 1) {
                    if (skipped++ < first) continue;
                    const u64 index = word * 64 + static_cast<u64>(std::countr_zero(rest));
                    entries.push_back({record.page * MATCH_PAGE_SIZE + index * width, record.kind});
                }
            }
        }
        if (reader.Broken()) {
            ok = false;
            break;
        }
    }
    FreeChunkBuffer(buffer);
    return ok;
}

} // namespace Jadyx::MemoryEdit
