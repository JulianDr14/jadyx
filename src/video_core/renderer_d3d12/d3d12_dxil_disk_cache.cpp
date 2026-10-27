// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <system_error>

#include "video_core/renderer_d3d12/d3d12_dxil_disk_cache.h"

#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"

namespace D3D12 {

namespace {

constexpr u32 FILE_MAGIC = 0x4358444A;   // "JDXC"
constexpr u32 FILE_VERSION = 2;
constexpr u32 RECORD_MAGIC = 0x5258444A; // "JDXR"
/// Larger records are a corrupt size field, not DXIL.
constexpr u32 MAX_RECORD_SIZE = 64U << 20;

struct FileHeader {
    u32 magic;
    u32 version;
    u64 generation;
};
static_assert(sizeof(FileHeader) == 16);
struct RecordHeader {
    u32 magic;
    u32 size;
    DxilDiskCache::Key key;
    u64 checksum;
};
static_assert(sizeof(RecordHeader) == 32);

std::atomic<bool> dxil_disk_cache_enabled{true};

u64 Checksum(std::span<const u8> payload) {
    return Common::CityHash64(reinterpret_cast<const char*>(payload.data()), payload.size());
}

} // Anonymous namespace

void SetDxilDiskCacheEnabled(bool enabled) {
    dxil_disk_cache_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsDxilDiskCacheEnabled() {
    return dxil_disk_cache_enabled.load(std::memory_order_relaxed);
}

DxilDiskCache& GetDxilDiskCache() {
    static DxilDiskCache cache;
    return cache;
}

void DxilDiskCache::Open(std::filesystem::path path_, u64 generation) {
    std::scoped_lock lock{mutex};
    open.store(false, std::memory_order_release);
    file.close();
    index.clear();
    path = std::move(path_);
    hits = 0;
    misses = 0;
    stored = 0;
    loaded = 0;
    end = 0;

    const u64 start = SteadyMicros();
    std::error_code ec;
    const u64 file_size = std::filesystem::file_size(path, ec);
    bool valid = false;
    if (!ec && file_size >= sizeof(FileHeader)) {
        std::ifstream in(path, std::ios::binary);
        FileHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        valid = in && header.magic == FILE_MAGIC && header.version == FILE_VERSION &&
                header.generation == generation;
        u64 offset = sizeof(header);
        // Only headers are read here; the payloads stay on disk until a pipeline asks for them.
        while (valid && offset + sizeof(RecordHeader) <= file_size) {
            RecordHeader record{};
            in.seekg(static_cast<std::streamoff>(offset));
            in.read(reinterpret_cast<char*>(&record), sizeof(record));
            if (!in || record.magic != RECORD_MAGIC || record.size > MAX_RECORD_SIZE ||
                offset + sizeof(record) + record.size > file_size) {
                break;
            }
            index.insert_or_assign(record.key, Location{offset, record.size});
            offset += sizeof(record) + record.size;
        }
        end = offset;
    }
    if (valid && end < file_size) {
        // A session closed mid-append: keep the records before it.
        LOG_WARNING(Render, "D3D12: DXIL cache tail of {} bytes dropped", file_size - end);
        std::filesystem::resize_file(path, end, ec);
        valid = !ec;
    }
    if (!valid) {
        index.clear();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const FileHeader header{FILE_MAGIC, FILE_VERSION, generation};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        end = sizeof(header);
        if (!out) {
            LOG_ERROR(Render, "D3D12: creating the DXIL cache {} failed", path.filename().string());
            return;
        }
    }
    file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        LOG_ERROR(Render, "D3D12: opening the DXIL cache {} failed", path.filename().string());
        return;
    }
    loaded = index.size();
    open.store(true, std::memory_order_release);
    LOG_INFO(Render, "D3D12: DXIL cache opened ({} pipelines, {} KiB, {:.1f} ms)", index.size(),
             end >> 10, static_cast<double>(SteadyMicros() - start) / 1000.0);
}

void DxilDiskCache::Close() {
    std::scoped_lock lock{mutex};
    open.store(false, std::memory_order_release);
    file.close();
    index.clear();
}

std::optional<std::vector<u8>> DxilDiskCache::Find(const Key& key) {
    std::scoped_lock lock{mutex};
    const auto it = index.find(key);
    if (!file.is_open() || it == index.end()) {
        misses.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    RecordHeader record{};
    std::vector<u8> payload(it->second.size);
    file.clear();
    file.seekg(static_cast<std::streamoff>(it->second.offset));
    file.read(reinterpret_cast<char*>(&record), sizeof(record));
    file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!file || record.key != key || record.checksum != Checksum(payload)) {
        file.clear();
        LOG_WARNING(Render, "D3D12: DXIL cache record at {} is damaged, compiling again",
                    it->second.offset);
        index.erase(it);
        misses.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    hits.fetch_add(1, std::memory_order_relaxed);
    return payload;
}

void DxilDiskCache::Store(const Key& key, std::span<const u8> payload) {
    std::scoped_lock lock{mutex};
    if (!file.is_open() || payload.size() > MAX_RECORD_SIZE || index.contains(key)) {
        return;
    }
    const RecordHeader record{.magic = RECORD_MAGIC,
                              .size = static_cast<u32>(payload.size()),
                              .key = key,
                              .checksum = Checksum(payload)};
    file.clear();
    file.seekp(static_cast<std::streamoff>(end));
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
    file.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    // Flushed per record: a session closed from the console never reaches a destructor.
    file.flush();
    if (!file) {
        file.clear();
        LOG_ERROR(Render, "D3D12: writing the DXIL cache failed");
        return;
    }
    index.emplace(key, Location{end, record.size});
    end += sizeof(record) + payload.size();
    stored.fetch_add(1, std::memory_order_relaxed);
}

void DxilDiskCache::LogStats(std::string_view when) const {
    if (!IsOpen()) {
        return;
    }
    LOG_INFO(Render, "D3D12 DXIL cache ({}): {} loaded, {} compiled, {} stored, {} on disk", when,
             hits.load(std::memory_order_relaxed), misses.load(std::memory_order_relaxed),
             stored.load(std::memory_order_relaxed), loaded);
}

} // namespace D3D12
