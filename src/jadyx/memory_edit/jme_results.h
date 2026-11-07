// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit: where a search keeps its addresses. Only addresses, never a copy of memory:
// the next search compares what memory holds now with the next number. They go to an unbuffered file
// that dies with its handle, 64 KiB chunks at a time, and RAM holds a line per chunk. Its own file,
// not Permafrost's: that one's slots are the guest's, and a search would take them when memory is
// shortest.
//
// A chunk is its header and records. A record is a page (zigzag varint, the difference from the
// page before it in the chunk) and a byte, the kind with bit 7 telling its format: 0 is a varint
// count and varint gaps between hit indices, 1 is the whole hit map. The shorter one is written.

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "jadyx/memory_edit/jme_matcher.h"
#include "jadyx/memory_edit/jme_value.h"

namespace Jadyx::MemoryEdit {

inline constexpr size_t CHUNK_SIZE = 64 * 1024;

/// Builds a chunk in a CHUNK_SIZE buffer of the caller's.
class ChunkWriter {
public:
    explicit ChunkWriter(u8* chunk_) : chunk{chunk_} {
        Reset();
    }

    void Reset();
    /// False when the record does not fit: the chunk is done. An empty chunk always has room.
    [[nodiscard]] bool Append(u64 page, ValueKind kind, const u64* bits, u32 hits);

    [[nodiscard]] u32 Hits() const {
        return total_hits;
    }

private:
    u8* chunk;
    size_t used{};
    u32 total_hits{};
    u64 last_page{};
};

/// One page's hits of one kind.
struct ChunkRecord {
    u64 page{};
    ValueKind kind{};
    u32 hits{};
    u64 bits[MATCH_WORDS]{};
};

class ChunkReader {
public:
    explicit ChunkReader(const u8* chunk_);

    /// False at the end of the chunk, or at a record that makes no sense (see Broken).
    [[nodiscard]] bool Next(ChunkRecord& record);
    [[nodiscard]] bool Broken() const {
        return broken;
    }
    [[nodiscard]] u32 Hits() const;

private:
    const u8* chunk;
    size_t used{};
    size_t at{};
    u64 last_page{};
    bool broken{};
};

/// Slots [first, end) of the file.
struct SlotRange {
    u32 first{};
    u32 end{};

    [[nodiscard]] constexpr u32 Size() const {
        return end - first;
    }
    constexpr bool operator==(const SlotRange&) const = default;
};

/// Where to write the list refined from the one in `current`, which stays readable meanwhile: before
/// it if a list as long fits, so the file stays small, or else on its roomier side.
[[nodiscard]] SlotRange NextListRange(SlotRange current, u32 max_slots);

/// The unbuffered file of chunks. Writes and reads may run in parallel.
class ResultFile {
public:
    ResultFile() = default;
    ~ResultFile();
    ResultFile(const ResultFile&) = delete;
    ResultFile& operator=(const ResultFile&) = delete;

    /// Replaces whatever is there. `max_bytes` caps what it may grow to; writes go to all of it until
    /// BeginList says otherwise.
    [[nodiscard]] bool Open(const std::wstring& path, u64 max_bytes);
    void Close();
    [[nodiscard]] bool IsOpen() const {
        return handle != nullptr;
    }

    /// From here on, writes take the slots of `range` in order. With no write running.
    void BeginList(SlotRange range);
    /// The range of the list being written: what it has taken of what BeginList gave.
    [[nodiscard]] SlotRange ListRange() const;

    /// Writes a page-aligned chunk to a new slot. Empty when the range is full or the write failed
    /// (see Full).
    [[nodiscard]] std::optional<u32> Write(const u8* chunk);
    /// Reads a slot into a page-aligned buffer.
    [[nodiscard]] bool Read(u32 slot, u8* chunk) const;

    [[nodiscard]] bool Full() const {
        return full.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u32 MaxSlots() const {
        return max_slots;
    }
    [[nodiscard]] u64 Bytes() const;

private:
    void* handle{};
    u32 max_slots{};
    SlotRange list_range{};
    std::atomic<u32> next_slot{};
    std::atomic<bool> full{};
};

/// A search's addresses: a line per chunk.
struct ResultChunk {
    u32 slot{};
    u32 hits{};
};
using ResultList = std::vector<ResultChunk>;

/// A chunk buffer aligned for unbuffered IO. Null when there is no memory for it.
[[nodiscard]] u8* NewChunkBuffer();
void FreeChunkBuffer(u8* buffer);

} // namespace Jadyx::MemoryEdit
