// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace D3D12 {

/// Signed DXIL of linked pipelines kept across sessions, one append-only file per title
/// (on unless boot.cfg has "dxil_cache=0"). A pipeline found in it skips spirv_to_dxil and the
/// validator.
class DxilDiskCache {
public:
    struct Key {
        u64 lo{};
        u64 hi{};
        bool operator==(const Key&) const = default;
    };

    /// Opens the title's file and indexes its records, dropping a tail cut short. A file written
    /// by another translator generation is started over, so stale records do not pile up.
    void Open(std::filesystem::path path, u64 generation);
    void Close();

    bool IsOpen() const {
        return open.load(std::memory_order_acquire);
    }

    std::optional<std::vector<u8>> Find(const Key& key);
    void Store(const Key& key, std::span<const u8> payload);

    void LogStats(std::string_view when) const;

private:
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return static_cast<size_t>(key.lo ^ (key.hi * 0x9E3779B97F4A7C15ULL));
        }
    };
    struct Location {
        u64 offset;
        u32 size;
    };

    std::filesystem::path path;
    std::fstream file;
    std::unordered_map<Key, Location, KeyHash> index;
    u64 end{};
    /// One stream for reads and appends.
    mutable std::mutex mutex;
    std::atomic<bool> open{};
    std::atomic<u64> hits{};
    std::atomic<u64> misses{};
    std::atomic<u64> stored{};
    u64 loaded{};
};

/// Global switch, set from boot.cfg before the renderer starts.
void SetDxilDiskCacheEnabled(bool enabled);
bool IsDxilDiskCacheEnabled();

/// Process-wide, like the PSO library: opened per title by PipelineCache.
DxilDiskCache& GetDxilDiskCache();

} // namespace D3D12
