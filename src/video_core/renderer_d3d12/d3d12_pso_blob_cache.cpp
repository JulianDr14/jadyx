// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_pso_blob_cache.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include <fmt/format.h>
#include <wrl/client.h>

#include "video_core/renderer_d3d12/d3d12_pso_key.h"

namespace D3D12 {

namespace {
std::atomic<bool> enabled{false};

// File: magic, format version, the stable hash again (a renamed file is not taken), blob size,
// then the blob.
constexpr u32 BLOB_MAGIC = 0x424F5350; // "PSOB"
constexpr u32 BLOB_VERSION = 1;
struct BlobHeader {
    u32 magic;
    u32 version;
    u64 hash;
    u64 size;
};
} // namespace

void PsoBlobCache::SetEnabled(bool value) {
    enabled.store(value, std::memory_order_relaxed);
}

bool PsoBlobCache::Enabled() noexcept {
    return enabled.load(std::memory_order_relaxed);
}

u64 PsoBlobCache::StableHash(const GraphicsPsoKey& key) noexcept {
    u64 hash = 14695981039346656037ULL;
    const auto add = [&](u64 value) { hash = (hash ^ value) * 1099511628211ULL; };
    for (const u32 word : key.state) {
        add(word);
    }
    for (const auto shader : key.shaders) {
        add(shader.size());
        for (const u8 byte : shader) {
            add(byte);
        }
    }
    return hash;
}

void PsoBlobCache::Open(std::filesystem::path path) {
    std::scoped_lock lock{mutex};
    directory = std::move(path);
    if (!directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            directory.clear();
        }
    }
}

std::filesystem::path PsoBlobCache::PathOf(u64 hash) const {
    return directory / fmt::format("{:016x}.pso", hash);
}

std::vector<u8> PsoBlobCache::Load(u64 hash) {
    std::filesystem::path path;
    {
        std::scoped_lock lock{mutex};
        if (directory.empty()) {
            return {};
        }
        path = PathOf(hash);
    }
    std::ifstream file{path, std::ios::binary};
    BlobHeader header{};
    std::vector<u8> blob;
    if (file && file.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
        header.magic == BLOB_MAGIC && header.version == BLOB_VERSION && header.hash == hash &&
        header.size != 0 && header.size <= (64ULL << 20)) {
        blob.resize(static_cast<size_t>(header.size));
        if (!file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()))) {
            blob.clear();
        }
    }
    std::scoped_lock lock{mutex};
    ++(blob.empty() ? stats.misses : stats.hits);
    return blob;
}

void PsoBlobCache::Store(u64 hash, ID3D12PipelineState* pso) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (!pso || FAILED(pso->GetCachedBlob(&blob)) || !blob || blob->GetBufferSize() == 0) {
        return;
    }
    std::filesystem::path path;
    {
        std::scoped_lock lock{mutex};
        if (directory.empty()) {
            return;
        }
        path = PathOf(hash);
    }
    // Written aside and renamed, so a reader never sees half a blob.
    std::filesystem::path temporary = path;
    temporary += fmt::format(".{:x}.tmp", reinterpret_cast<std::uintptr_t>(pso));
    const BlobHeader header{BLOB_MAGIC, BLOB_VERSION, hash, blob->GetBufferSize()};
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(static_cast<const char*>(blob->GetBufferPointer()),
                   static_cast<std::streamsize>(blob->GetBufferSize()));
        if (!file) {
            file.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return;
    }
    std::scoped_lock lock{mutex};
    ++stats.stored;
    stats.bytes += blob->GetBufferSize();
}

void PsoBlobCache::Reject(u64 hash) {
    std::filesystem::path path;
    {
        std::scoped_lock lock{mutex};
        if (directory.empty()) {
            return;
        }
        path = PathOf(hash);
        ++stats.rejected;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

PsoBlobCache::Stats PsoBlobCache::GetStats() const {
    std::scoped_lock lock{mutex};
    return stats;
}

PsoBlobCache& GetPsoBlobCache() {
    static PsoBlobCache cache;
    return cache;
}

void SetPsoBlobs(bool value) {
    PsoBlobCache::SetEnabled(value);
}

} // namespace D3D12
