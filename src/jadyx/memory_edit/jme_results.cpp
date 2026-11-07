// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/memory_edit/jme_results.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <malloc.h>

#include <windows.h>

namespace Jadyx::MemoryEdit {

namespace {

struct ChunkHeader {
    u32 used; // bytes of the chunk in use, this header among them
    u32 hits;
};

constexpr u8 FORMAT_MAP = 0x80;
constexpr size_t MAX_VARINT = 10;

size_t VarintSize(u64 value) {
    size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

u8* PutVarint(u8* out, u64 value) {
    while (value >= 0x80) {
        *out++ = static_cast<u8>(value) | 0x80;
        value >>= 7;
    }
    *out++ = static_cast<u8>(value);
    return out;
}

bool GetVarint(const u8* chunk, size_t used, size_t& at, u64& value) {
    value = 0;
    for (u32 shift = 0; shift < 64 && at < used; shift += 7) {
        const u8 byte = chunk[at++];
        value |= static_cast<u64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

u64 Zigzag(s64 value) {
    return (static_cast<u64>(value) << 1) ^ static_cast<u64>(value >> 63);
}

s64 Unzigzag(u64 value) {
    return static_cast<s64>(value >> 1) ^ -static_cast<s64>(value & 1);
}

// The bytes of the index list of a hit map.
size_t ListSize(const u64* bits, size_t words, u32 hits) {
    size_t size = VarintSize(hits);
    u64 last = 0;
    for (size_t word = 0; word < words; ++word) {
        for (u64 rest = bits[word]; rest != 0; rest &= rest - 1) {
            const u64 index = word * 64 + static_cast<u64>(std::countr_zero(rest));
            size += VarintSize(index - last);
            last = index;
        }
    }
    return size;
}

} // namespace

void ChunkWriter::Reset() {
    used = sizeof(ChunkHeader);
    total_hits = 0;
    last_page = 0;
    // What the last chunk left past `used` would go to disk in the slot: it is only addresses, and
    // nothing reads it, so it stays.
    const ChunkHeader header{static_cast<u32>(used), 0};
    std::memcpy(chunk, &header, sizeof(header));
}

bool ChunkWriter::Append(u64 page, ValueKind kind, const u64* bits, u32 hits) {
    const size_t words = MatchWords(kind);
    const size_t map_size = words * sizeof(u64);
    // Each index takes a byte at least, so as many hits as map bytes need not count the list.
    const size_t list_size = hits >= map_size ? map_size : ListSize(bits, words, hits);
    const bool use_map = list_size >= map_size;
    const size_t body = use_map ? map_size : list_size;
    if (used + MAX_VARINT + 1 + body > CHUNK_SIZE) return false;

    u8* out = PutVarint(chunk + used, Zigzag(static_cast<s64>(page - last_page)));
    *out++ = static_cast<u8>(kind) | (use_map ? FORMAT_MAP : u8{0});
    if (use_map) {
        std::memcpy(out, bits, map_size);
        out += map_size;
    } else {
        out = PutVarint(out, hits);
        u64 last = 0;
        for (size_t word = 0; word < words; ++word) {
            for (u64 rest = bits[word]; rest != 0; rest &= rest - 1) {
                const u64 index = word * 64 + static_cast<u64>(std::countr_zero(rest));
                out = PutVarint(out, index - last);
                last = index;
            }
        }
    }
    used = static_cast<size_t>(out - chunk);
    last_page = page;
    total_hits += hits;
    const ChunkHeader header{static_cast<u32>(used), total_hits};
    std::memcpy(chunk, &header, sizeof(header));
    return true;
}

ChunkReader::ChunkReader(const u8* chunk_) : chunk{chunk_} {
    ChunkHeader header;
    std::memcpy(&header, chunk, sizeof(header));
    broken = header.used < sizeof(ChunkHeader) || header.used > CHUNK_SIZE;
    used = broken ? 0 : header.used;
    at = sizeof(ChunkHeader);
}

u32 ChunkReader::Hits() const {
    ChunkHeader header;
    std::memcpy(&header, chunk, sizeof(header));
    return header.hits;
}

bool ChunkReader::Next(ChunkRecord& record) {
    if (broken || at >= used) return false;
    broken = true;
    u64 delta{};
    if (!GetVarint(chunk, used, at, delta) || at >= used) return false;
    record.page = last_page + static_cast<u64>(Unzigzag(delta));
    const u8 kind_byte = chunk[at++];
    const u8 kind = kind_byte & static_cast<u8>(~FORMAT_MAP);
    if (kind >= VALUE_KINDS) return false;
    record.kind = static_cast<ValueKind>(kind);
    const size_t words = MatchWords(record.kind);
    std::fill_n(record.bits, words, u64{0});
    record.hits = 0;
    if ((kind_byte & FORMAT_MAP) != 0) {
        const size_t map_size = words * sizeof(u64);
        if (used - at < map_size) return false;
        std::memcpy(record.bits, chunk + at, map_size);
        at += map_size;
        for (size_t word = 0; word < words; ++word) {
            record.hits += static_cast<u32>(std::popcount(record.bits[word]));
        }
    } else {
        u64 hits{};
        if (!GetVarint(chunk, used, at, hits) || hits > words * 64) return false;
        u64 index = 0;
        for (u64 i = 0; i < hits; ++i) {
            u64 gap{};
            if (!GetVarint(chunk, used, at, gap)) return false;
            index += gap;
            if (index >= words * 64) return false;
            record.bits[index / 64] |= u64{1} << (index % 64);
        }
        record.hits = static_cast<u32>(hits);
    }
    last_page = record.page;
    broken = false;
    return true;
}

ResultFile::~ResultFile() {
    Close();
}

bool ResultFile::Open(const std::wstring& path, u64 max_bytes) {
    Close();
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE;
    constexpr DWORD SHARE = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE file = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE, SHARE, CREATE_ALWAYS, &params);
    if (file == INVALID_HANDLE_VALUE) return false;
    handle = file;
    max_slots = static_cast<u32>(max_bytes / CHUNK_SIZE);
    BeginList({0, max_slots});
    return true;
}

void ResultFile::Close() {
    if (handle != nullptr) CloseHandle(handle);
    handle = nullptr;
}

void ResultFile::BeginList(SlotRange range) {
    list_range = range;
    next_slot.store(range.first, std::memory_order_relaxed);
    full.store(false, std::memory_order_relaxed);
}

SlotRange ResultFile::ListRange() const {
    return {list_range.first, (std::min)(next_slot.load(std::memory_order_relaxed), list_range.end)};
}

SlotRange NextListRange(SlotRange current, u32 max_slots) {
    // What the threads leave half full may take a few more chunks than the list it comes from.
    constexpr u32 ROOM_FOR_PARTS = 16;
    const u32 after = max_slots - current.end;
    if (current.first >= current.Size() + ROOM_FOR_PARTS || current.first >= after) {
        return {0, current.first};
    }
    return {current.end, max_slots};
}

namespace {

bool Transfer(HANDLE file, bool write, u8* memory, u32 slot) {
    // An event per call: waiting on the handle would mix up calls running in parallel.
    const HANDLE event = CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    if (event == nullptr) return false;
    const u64 offset = u64{slot} * CHUNK_SIZE;
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    at.hEvent = event;
    const BOOL started = write ? WriteFile(file, memory, CHUNK_SIZE, nullptr, &at)
                               : ReadFile(file, memory, CHUNK_SIZE, nullptr, &at);
    DWORD done{};
    const bool ok = (started || GetLastError() == ERROR_IO_PENDING) &&
                    GetOverlappedResultEx(file, &at, &done, INFINITE, FALSE) && done == CHUNK_SIZE;
    CloseHandle(event);
    return ok;
}

} // namespace

std::optional<u32> ResultFile::Write(const u8* chunk) {
    const u32 slot = next_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot >= list_range.end) {
        full.store(true, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (!Transfer(handle, true, const_cast<u8*>(chunk), slot)) return std::nullopt;
    return slot;
}

bool ResultFile::Read(u32 slot, u8* chunk) const {
    return Transfer(handle, false, chunk, slot);
}

u64 ResultFile::Bytes() const {
    return u64{ListRange().end} * CHUNK_SIZE;
}

u8* NewChunkBuffer() {
    return static_cast<u8*>(_aligned_malloc(CHUNK_SIZE, 4096));
}

void FreeChunkBuffer(u8* buffer) {
    _aligned_free(buffer);
}

} // namespace Jadyx::MemoryEdit
