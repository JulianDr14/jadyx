// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;

/// Suballocates texture memory from reusable DEFAULT heaps. The allocation token returns its
/// range only after the scheduler has released the placed resource at the same fence tick.
class TextureResourceAllocator {
public:
    struct Allocation;
    struct Resource {
        ComPtr<ID3D12Resource> resource;
        std::shared_ptr<Allocation> allocation;
        bool placed{};
    };

    TextureResourceAllocator(const Device& device, Scheduler& scheduler);
    ~TextureResourceAllocator();

    TextureResourceAllocator(const TextureResourceAllocator&) = delete;
    TextureResourceAllocator& operator=(const TextureResourceAllocator&) = delete;

    [[nodiscard]] Resource Create(const D3D12_RESOURCE_DESC& desc,
                                  D3D12_RESOURCE_STATES initial_state,
                                  const D3D12_CLEAR_VALUE* clear_value = nullptr);
    void DeferRelease(Resource&& resource);
    enum class TrimMode {
        KeepWarm, ///< Keep one warm empty heap per class, release the rest.
        Settled,  ///< Release heaps that stayed empty for a while; recent ones get reused.
        All,      ///< Release every empty heap (the system refused memory).
    };
    /// Called by the recording thread. Only fully free, fence-retired heaps are eligible.
    void TrimEmptyHeaps(TrimMode mode);

    [[nodiscard]] std::string Report() const;
    /// Diagnostic: the placed-resource shapes holding the most memory and created most often.
    [[nodiscard]] std::string Census(size_t top) const;
    struct Stats {
        u64 heap_bytes{}, reserved_bytes{}, pending_bytes{}, free_bytes{}, largest_free_range{};
    };
    [[nodiscard]] Stats GetStats() const;
    /// GetStats().heap_bytes without walking every free range.
    [[nodiscard]] u64 HeapBytes() const;
    /// By heap index: heaps in use with at most this share of their bytes live. Evicting what
    /// they hold empties them, so trim can release them.
    [[nodiscard]] std::vector<bool> SparseHeaps(u32 max_used_percent) const;
    [[nodiscard]] static size_t HeapOf(const Allocation& allocation);

private:
    struct State;
    const Device& device;
    Scheduler& scheduler;
    std::shared_ptr<State> state;
};

} // namespace D3D12
