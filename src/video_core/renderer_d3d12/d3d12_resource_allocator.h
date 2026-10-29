// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_heap_packing.h"

namespace D3D12 {

class Scheduler;

/// Small sampled 2D textures placed at 4 KiB instead of 64 KiB ("small_textures=0" turns it off).
/// Applies to textures created from now on.
void SetSmallTextureAlignment(bool enabled);

/// A class that grew gets one spare heap made on another thread while memory is plentiful
/// ("spare_heaps=0" turns it off).
void SetSpareHeaps(bool enabled);

/// Plain sampled textures (no render target, depth or UAV use) placed in CPU-writable heaps
/// (write-back L0) on a cache-coherent UMA device, so the CPU writes their first upload in place
/// instead of through staging and a GPU copy ("cpu_texture_writes=1"). Off for good once the
/// driver refuses such a heap or texture. Applies to textures created from now on.
void SetCpuTextureWrites(bool enabled);
[[nodiscard]] bool CpuTextureWrites() noexcept;

/// Suballocates texture memory from reusable DEFAULT heaps. The allocation token returns its
/// range only after the scheduler has released the placed resource at the same fence tick.
class TextureResourceAllocator {
public:
    struct Allocation;
    struct Resource {
        ComPtr<ID3D12Resource> resource;
        std::shared_ptr<Allocation> allocation;
        bool placed{};
        bool cpu_writable{}; ///< placed in a CPU-writable heap (WriteToSubresource)
    };

    TextureResourceAllocator(const Device& device, Scheduler& scheduler);
    ~TextureResourceAllocator();

    TextureResourceAllocator(const TextureResourceAllocator&) = delete;
    TextureResourceAllocator& operator=(const TextureResourceAllocator&) = delete;

    /// A known texel block lets a small sampled 2D texture sit at 4 KiB instead of 64 KiB.
    /// cpu_writable asks for a CPU-writable heap; granted only on a cache-coherent UMA device
    /// while CpuTextureWrites() is on (Resource::cpu_writable says whether it was).
    [[nodiscard]] Resource Create(const D3D12_RESOURCE_DESC& desc,
                                  D3D12_RESOURCE_STATES initial_state,
                                  const D3D12_CLEAR_VALUE* clear_value = nullptr,
                                  HeapPacking::TexelBlock block = {}, bool cpu_writable = false);
    /// A placed resource of `desc` (Alignment kept) in free space of a heap in use that `avoid`
    /// does not mark (by heap index), without growing the pool or falling back to a committed
    /// resource; empty when none fits. Moves live textures out of sparse heaps.
    [[nodiscard]] Resource CreateInUsedHeaps(const D3D12_RESOURCE_DESC& desc,
                                             D3D12_RESOURCE_STATES initial_state,
                                             bool cpu_writable, const std::vector<bool>& avoid);
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
    struct SpareWorker;
    /// Create; with `in_used_heaps`, CreateInUsedHeaps.
    [[nodiscard]] Resource CreateImpl(const D3D12_RESOURCE_DESC& requested,
                                      D3D12_RESOURCE_STATES initial_state,
                                      const D3D12_CLEAR_VALUE* clear_value,
                                      HeapPacking::TexelBlock texel_block, bool cpu_writable,
                                      bool in_used_heaps, const std::vector<bool>& avoid);
    const Device& device;
    Scheduler& scheduler;
    bool cache_coherent_uma{};
    std::shared_ptr<State> state;
    std::unique_ptr<SpareWorker> spare_worker; ///< last: stopped before the members it uses
};

} // namespace D3D12
