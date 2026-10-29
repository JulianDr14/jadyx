// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <mutex>
#include <vector>

#include <d3d12.h>

#include "common/common_types.h"

namespace D3D12 {

struct GraphicsPsoKey;

/// The driver's compiled PSOs (ID3D12PipelineState::GetCachedBlob) kept on disk, one file each,
/// and handed back as CachedPSO the next time the same PSO is created. Off by default
/// ("pso_blobs=1"): a driver may reject the blobs, and the console's may not make them at all.
/// A blob the driver rejects is deleted and the PSO created without it.
class PsoBlobCache {
public:
    struct Stats {
        u64 hits;     ///< PSOs created from a stored blob
        u64 misses;   ///< PSOs with no stored blob
        u64 stored;   ///< blobs written
        u64 rejected; ///< stored blobs the driver refused (deleted)
        u64 bytes;    ///< bytes of the blobs written
    };

    /// Any thread: whether blobs are used, for PSOs created from now on.
    static void SetEnabled(bool enabled);
    [[nodiscard]] static bool Enabled() noexcept;

    /// A stable identity of a PSO across sessions: its state words and shader bytes (the root
    /// signature follows from the shaders).
    [[nodiscard]] static u64 StableHash(const GraphicsPsoKey& key) noexcept;

    /// The folder of this title's blobs; an empty path closes it.
    void Open(std::filesystem::path directory);

    /// The blob stored for this PSO, empty if none (or closed).
    [[nodiscard]] std::vector<u8> Load(u64 hash);
    /// Writes the blob of a PSO created without one.
    void Store(u64 hash, ID3D12PipelineState* pso);
    /// The driver refused the stored blob: deleted so it is not tried again.
    void Reject(u64 hash);

    [[nodiscard]] Stats GetStats() const;

private:
    [[nodiscard]] std::filesystem::path PathOf(u64 hash) const;

    mutable std::mutex mutex;
    std::filesystem::path directory;
    Stats stats{};
};

[[nodiscard]] PsoBlobCache& GetPsoBlobCache();

/// PsoBlobCache::SetEnabled, for the boot config.
void SetPsoBlobs(bool enabled);

} // namespace D3D12
