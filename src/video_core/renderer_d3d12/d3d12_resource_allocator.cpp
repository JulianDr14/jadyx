// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/alignment.h"
#include "common/assert.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/memory_ledger.h"
#include "video_core/renderer_d3d12/d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/d3d12_heap_growth_policy.h"
#include "video_core/renderer_d3d12/d3d12_heap_packing.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace D3D12 {

using namespace Common::Literals;

namespace {
enum class HeapClass : u8 { Texture, RenderTarget };

HeapClass ClassOf(const D3D12_RESOURCE_DESC& desc) {
    constexpr D3D12_RESOURCE_FLAGS RT_DS = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return (desc.Flags & RT_DS) != 0 ? HeapClass::RenderTarget : HeapClass::Texture;
}

D3D12_HEAP_FLAGS FlagsOf(HeapClass heap_class) {
    return heap_class == HeapClass::RenderTarget ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                                                 : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
}

// Render targets are recreated at new sizes every few frames and pin whole heaps: one survivor
// kept a 64 MiB heap from emptying, leaving 600 MiB free across RT heaps that could not be
// released. Smaller RT heaps drain sooner and are trimmed.
u64 PoolOf(HeapClass heap_class) {
    return heap_class == HeapClass::RenderTarget ? 32_MiB : 64_MiB;
}
} // Anonymous namespace

struct TextureResourceAllocator::State {
    struct Block {
        ComPtr<ID3D12Heap> heap;
        HeapClass heap_class{};
        u64 size{};
        u64 alignment{};
        std::map<u64, u64> free_ranges; // offset -> bytes
        Common::MemoryCharge charge;
        u64 empty_tick{}; // first trim pass that saw it empty; 0 while in use
    };

    mutable std::mutex mutex;
    std::vector<Block> blocks;
    u64 heap_bytes{};
    u64 live_bytes{};
    u64 peak_bytes{};
    u64 placed_resources{};
    u64 committed_fallbacks{};
    u64 trimmed_heaps{};
    u64 trimmed_bytes{};
    u64 pending_bytes{};
    u64 growth_recoveries{};
    u64 compact_heaps{};
    u64 avoided_capacity{};
    u64 pressure_waits{};
    // Accessed only by the recording thread, never by retirement callbacks.
    u64 pressure_wait_tick{};
    // Diagnostic census of placed resources by shape (format, width, height, depth, mips, flags).
    struct CensusEntry {
        u64 created{}, live{}, live_bytes{};
    };
    using CensusKey = std::tuple<u32, u64, u32, u16, u16, u32>;
    std::map<CensusKey, CensusEntry> census;

    void Free(size_t block_index, u64 offset, u64 size, bool pending = false,
              CensusEntry* entry = nullptr) {
        std::scoped_lock lock{mutex};
        if (entry) {
            --entry->live;
            entry->live_bytes -= size;
        }
        if (block_index >= blocks.size()) {
            return;
        }
        const u64 released_size = size;
        auto& ranges = blocks[block_index].free_ranges;
        auto next = ranges.lower_bound(offset);
        if (next != ranges.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) {
                offset = prev->first;
                size += prev->second;
                ranges.erase(prev);
            }
        }
        next = ranges.lower_bound(offset);
        if (next != ranges.end() && offset + size == next->first) {
            size += next->second;
            ranges.erase(next);
        }
        ranges.emplace(offset, size);
        ASSERT(live_bytes >= released_size);
        live_bytes -= released_size;
        if (pending) pending_bytes -= released_size;
    }
};

struct TextureResourceAllocator::Allocation {
    std::shared_ptr<State> state;
    size_t block_index{};
    u64 offset{};
    u64 size{};
    bool pending{}; // guarded by State::mutex
    State::CensusEntry* census{};

    ~Allocation() {
        if (state) {
            state->Free(block_index, offset, size, pending, census);
        }
    }
};

TextureResourceAllocator::TextureResourceAllocator(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_}, state{std::make_shared<State>()} {}

TextureResourceAllocator::~TextureResourceAllocator() = default;

TextureResourceAllocator::Resource TextureResourceAllocator::Create(
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE* clear_value) {
    const D3D12_RESOURCE_ALLOCATION_INFO info = device.Get()->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == UINT64_MAX || info.Alignment == 0) {
        return {};
    }
    const HeapClass heap_class = ClassOf(desc);
    const u64 heap_alignment =
        std::max<u64>(info.Alignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    size_t selected = SIZE_MAX;
    u64 selected_offset{};
    u64 best_waste = UINT64_MAX;
    u64 best_block_free = UINT64_MAX;
    const auto reserve_space = [&] {
        auto& ranges = state->blocks[selected].free_ranges;
        auto containing = ranges.upper_bound(selected_offset);
        if (containing != ranges.begin()) --containing;
        const u64 range_offset = containing->first;
        const u64 range_size = containing->second;
        ranges.erase(containing);
        if (selected_offset > range_offset) {
            ranges.emplace(range_offset, selected_offset - range_offset);
        }
        const u64 end = selected_offset + info.SizeInBytes;
        if (end < range_offset + range_size) {
            ranges.emplace(end, range_offset + range_size - end);
        }
        state->blocks[selected].empty_tick = 0;
        state->live_bytes += info.SizeInBytes;
        state->peak_bytes = std::max(state->peak_bytes, state->live_bytes);
    };

    const auto find_space = [&] {
        std::scoped_lock lock{state->mutex};
        // The fullest heap that fits, then the tightest range in it: resolution-scaled render
        // targets are recreated every few frames, and choosing the tightest range across all heaps
        // kept a few live resources in every heap (912 MiB of heaps for 487 MiB live), so none
        // emptied and the texture cap could not release any. Packing the dense heaps first lets
        // the sparse ones drain.
        for (size_t index = 0; index < state->blocks.size(); ++index) {
            auto& block = state->blocks[index];
            if (!block.heap || block.heap_class != heap_class || block.alignment < heap_alignment) {
                continue;
            }
            u64 block_free = 0;
            for (const auto& range : block.free_ranges) {
                block_free += range.second;
            }
            for (const auto& [offset, bytes] : block.free_ranges) {
                const auto fit = HeapPacking::Fit(offset, bytes, info.SizeInBytes, info.Alignment);
                if (fit && (block_free < best_block_free ||
                            (block_free == best_block_free && fit->waste < best_waste))) {
                    selected = index;
                    selected_offset = fit->offset;
                    best_waste = fit->waste;
                    best_block_free = block_free;
                }
            }
        }
        // Reserve while still holding the pool lock so a concurrent trim cannot remove it.
        if (selected != SIZE_MAX) reserve_space();
    };
    find_space();

    bool compact = false;
    u64 wanted{};
    if (selected == SIZE_MAX) {
        // Collect before growing, outside the pool lock: retirement callbacks acquire it.
        // This also runs during loading screens, without waiting for another present/GC frame.
        const auto before = device.QueryCacheMemoryPressure();
        const u64 pooled_size =
            HeapGrowth::Capacity(info.SizeInBytes, info.Alignment, false, PoolOf(heap_class));
        if (!pooled_size) return {};
        const auto pressured = [pooled_size](const CacheMemorySnapshot& snapshot) {
            return HeapGrowth::UnderPressure(pooled_size,
                {snapshot.app_used, snapshot.app_limit},
                {snapshot.gpu_used, snapshot.gpu_budget});
        };
        const bool pressure = pressured(before);
        const auto old_stats = GetStats();
        scheduler.CollectGarbage();
        find_space();
        // Waiting stalled the GPU thread on most pressured misses. Only wait when even an exact
        // heap would exhaust the budget and retiring work could return enough to hold this one.
        const bool exhausted = HeapGrowth::Exhausted(
            HeapGrowth::Capacity(info.SizeInBytes, info.Alignment, true),
            {before.app_used, before.app_limit});
        if (selected == SIZE_MAX && pressure && exhausted && scheduler.IsRecordingThread() &&
            GetStats().pending_bytes >= info.SizeInBytes) {
            const u64 current = scheduler.CurrentTick();
            const u64 previous = current > 0 ? current - 1 : 0;
            if (previous > state->pressure_wait_tick) {
                state->pressure_wait_tick = previous;
                {
                    std::scoped_lock lock{state->mutex};
                    ++state->pressure_waits;
                }
                scheduler.Wait(previous);
                scheduler.CollectGarbage();
                find_space();
            }
        }
        TrimEmptyHeaps(pressure ? TrimMode::Settled : TrimMode::KeepWarm);
        // The selected range was already reserved above. Trim cannot destroy its heap.
        if (pressure) {
            const auto after = device.QueryCacheMemoryPressure();
            compact = !after.Known() || pressured(after);
            if (compact && after.Known()) {
                wanted = HeapGrowth::PressureCapacity(info.SizeInBytes, info.Alignment,
                                                      {after.app_used, after.app_limit});
            }
            const auto stats = GetStats();
            {
                std::scoped_lock lock{state->mutex};
                ++state->growth_recoveries;
            }
            LOG_INFO(Render, "D3D12: texture growth recovery: app {}/{} -> {}/{} MiB, "
                     "heaps {} -> {} MiB, pending {} -> {} MiB, reused {}",
                     before.app_used / 1_MiB, before.app_limit / 1_MiB,
                     after.app_used / 1_MiB, after.app_limit / 1_MiB,
                     old_stats.heap_bytes / 1_MiB, stats.heap_bytes / 1_MiB,
                     old_stats.pending_bytes / 1_MiB, stats.pending_bytes / 1_MiB,
                     selected != SIZE_MAX);
        }
    }

    {
        std::scoped_lock lock{state->mutex};

        if (selected == SIZE_MAX) {
            if (!wanted) wanted = HeapGrowth::Capacity(info.SizeInBytes, info.Alignment, compact,
                                                         PoolOf(heap_class));
            if (!wanted) return {};
            const D3D12_HEAP_DESC heap_desc{
                .SizeInBytes = wanted,
                .Properties = {.Type = D3D12_HEAP_TYPE_DEFAULT},
                .Alignment = heap_alignment,
                .Flags = FlagsOf(heap_class),
            };
            State::Block block{.heap_class = heap_class, .size = wanted,
                               .alignment = heap_alignment};
            const HRESULT heap_hr = device.Get()->CreateHeap(&heap_desc, IID_PPV_ARGS(&block.heap));
            if (SUCCEEDED(heap_hr)) {
                block.free_ranges.emplace(0, wanted);
                block.charge = Common::MemoryCharge(Common::MemoryAccount::TextureHeaps, wanted);
                state->heap_bytes += wanted;
                if (compact) {
                    ++state->compact_heaps;
                    state->avoided_capacity +=
                        HeapGrowth::Capacity(info.SizeInBytes, info.Alignment, false,
                                             PoolOf(heap_class)) - wanted;
                }
                // Tombstones preserve outstanding Allocation indices. Reuse only slots with
                // no heap: Trim proves there are no live or pending allocations in that slot.
                const auto vacant = std::find_if(state->blocks.begin(), state->blocks.end(),
                                                [](const auto& b) { return !b.heap; });
                if (vacant == state->blocks.end()) {
                    state->blocks.emplace_back(std::move(block));
                    selected = state->blocks.size() - 1;
                } else {
                    selected = static_cast<size_t>(vacant - state->blocks.begin());
                    *vacant = std::move(block);
                }
                selected_offset = 0;
                reserve_space();
                LOG_INFO(Render, "D3D12: texture heap pool added {} MiB {} block ({} MiB total), "
                         "required {} bytes, pressure-sized {}",
                         wanted / 1_MiB,
                         heap_class == HeapClass::RenderTarget ? "RT/DS" : "texture",
                         state->heap_bytes / 1_MiB, info.SizeInBytes, compact);
            } else {
                LOG_WARNING(Render, "D3D12: CreateHeap for texture pool failed 0x{:08x}; using committed resources",
                            static_cast<u32>(heap_hr));
            }
        }
    }

    Resource result;
    if (selected != SIZE_MAX) {
        const HRESULT placed_hr = device.Get()->CreatePlacedResource(
            state->blocks[selected].heap.Get(), selected_offset, &desc, initial_state, clear_value,
            IID_PPV_ARGS(&result.resource));
        if (SUCCEEDED(placed_hr)) {
            result.allocation = std::make_shared<Allocation>();
            result.allocation->state = state;
            result.allocation->block_index = selected;
            result.allocation->offset = selected_offset;
            result.allocation->size = info.SizeInBytes;
            result.placed = true;
            std::scoped_lock lock{state->mutex};
            ++state->placed_resources;
            auto& entry = state->census[{static_cast<u32>(desc.Format), desc.Width, desc.Height,
                                         desc.DepthOrArraySize, desc.MipLevels,
                                         static_cast<u32>(desc.Flags)}];
            ++entry.created;
            ++entry.live;
            entry.live_bytes += info.SizeInBytes;
            result.allocation->census = &entry;
            return result;
        }
        state->Free(selected, selected_offset, info.SizeInBytes);
        LOG_WARNING(Render, "D3D12: CreatePlacedResource failed 0x{:08x}; using committed resource",
                    static_cast<u32>(placed_hr));
    }

    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const auto create_committed = [&] {
        return device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      initial_state, clear_value,
                                                      IID_PPV_ARGS(&result.resource));
    };
    HRESULT hr = create_committed();
    if (hr == E_OUTOFMEMORY && scheduler.IsRecordingThread()) {
        // The commit limit refused it. Return what submitted work no longer needs (deferred
        // releases, then heaps they emptied) and try once more. Waiting only on submitted ticks
        // never flushes the list being recorded.
        LOG_WARNING(Render, "D3D12: out of memory creating a {} byte texture; reclaiming and "
                            "retrying", info.SizeInBytes);
        const u64 current = scheduler.CurrentTick();
        if (current > 1) {
            scheduler.Wait(current - 1);
        }
        scheduler.CollectGarbage();
        TrimEmptyHeaps(TrimMode::All);
        hr = create_committed();
    }
    ThrowIfFailed(hr, "Create committed texture fallback");
    {
        std::scoped_lock lock{state->mutex};
        ++state->committed_fallbacks;
    }
    return result;
}

void TextureResourceAllocator::DeferRelease(Resource&& resource) {
    if (!resource.resource) {
        return;
    }
    auto allocation = std::move(resource.allocation);
    if (allocation) {
        std::scoped_lock lock{allocation->state->mutex};
        if (!allocation->pending) {
            allocation->pending = true;
            allocation->state->pending_bytes += allocation->size;
        }
    }
    scheduler.DeferRelease(std::move(resource.resource),
                           [allocation = std::move(allocation)]() mutable { allocation.reset(); });
}

void TextureResourceAllocator::TrimEmptyHeaps(TrimMode mode) {
    // Releasing every empty heap on each pressured miss churned heaps that the next textures
    // would have reused. Settled keeps a heap until it has stayed empty for this many submits.
    constexpr u64 settle_ticks = 64;
    const u64 tick = mode == TrimMode::Settled ? scheduler.CurrentTick() : 0;
    std::scoped_lock lock{state->mutex};
    std::array<bool, 2> warm{};
    for (auto& block : state->blocks) {
        if (!block.heap || block.free_ranges.size() != 1) continue;
        const auto& [offset, size] = *block.free_ranges.begin();
        if (offset != 0 || size != block.size) continue;
        const size_t type = static_cast<size_t>(block.heap_class);
        if (mode == TrimMode::KeepWarm && !warm[type]) {
            warm[type] = true;
            continue;
        }
        if (mode == TrimMode::Settled) {
            if (!block.empty_tick) block.empty_tick = tick;
            if (tick < block.empty_tick + settle_ticks) continue;
        }
        state->heap_bytes -= block.size;
        state->trimmed_bytes += block.size;
        ++state->trimmed_heaps;
        block.heap.Reset();
        block.free_ranges.clear();
        block.charge.Reset();
        block.size = 0;
        block.empty_tick = 0;
    }
}

std::string TextureResourceAllocator::Report() const {
    std::scoped_lock lock{state->mutex};
    // Diagnostic: per class heaps, empty heaps, free bytes and largest free range.
    std::array<u64, 2> count{}, empty{}, free{}, largest{};
    for (const auto& block : state->blocks) {
        if (!block.heap) continue;
        const size_t type = static_cast<size_t>(block.heap_class);
        ++count[type];
        for (const auto& [offset, bytes] : block.free_ranges) {
            free[type] += bytes;
            largest[type] = std::max(largest[type], bytes);
            if (bytes == block.size) empty[type] += bytes;
        }
    }
    LOG_INFO(Render, "D3D12: heap frag tex n {} free {} empty {} max {} | rt n {} free {} empty {} "
             "max {} MiB, pending {} MiB",
             count[0], free[0] / 1_MiB, empty[0] / 1_MiB, largest[0] / 1_MiB, count[1],
             free[1] / 1_MiB, empty[1] / 1_MiB, largest[1] / 1_MiB, state->pending_bytes / 1_MiB);
    return fmt::format("texture heaps {} MiB, live {} MiB, peak {} MiB, placed {}, fallback {}, trimmed {} ({} MiB), "
                       "growth recoveries {}, pressure waits {}, pressure heaps {}, "
                       "avoided capacity {} MiB",
                       state->heap_bytes / 1_MiB, state->live_bytes / 1_MiB,
                       state->peak_bytes / 1_MiB, state->placed_resources,
                       state->committed_fallbacks, state->trimmed_heaps, state->trimmed_bytes / 1_MiB,
                       state->growth_recoveries, state->pressure_waits, state->compact_heaps,
                       state->avoided_capacity / 1_MiB);
}

std::string TextureResourceAllocator::Census(size_t top) const {
    std::scoped_lock lock{state->mutex};
    std::vector<std::pair<State::CensusKey, State::CensusEntry>> entries(state->census.begin(),
                                                                         state->census.end());
    const auto list = [&](auto&& less, const char* name) {
        std::sort(entries.begin(), entries.end(),
                  [&](const auto& a, const auto& b) { return less(b.second, a.second); });
        std::string out = fmt::format(" | top {}:", name);
        for (size_t i = 0; i < std::min(top, entries.size()); ++i) {
            const auto& [key, e] = entries[i];
            const auto& [format, width, height, depth, mips, flags] = key;
            out += fmt::format(" [fmt {} {}x{}x{} m{} f{:x}: live {} ({} MiB), created {}]",
                               format, width, height, depth, mips, flags, e.live,
                               e.live_bytes / 1_MiB, e.created);
        }
        return out;
    };
    u64 live_total{};
    for (const auto& [key, e] : entries) live_total += e.live_bytes;
    std::string out = fmt::format("texture census: {} shapes, placed live {} MiB", entries.size(),
                                  live_total / 1_MiB);
    out += list([](const auto& a, const auto& b) { return a.live_bytes < b.live_bytes; }, "live");
    out += list([](const auto& a, const auto& b) { return a.created < b.created; }, "created");
    return out;
}

TextureResourceAllocator::Stats TextureResourceAllocator::GetStats() const {
    std::scoped_lock lock{state->mutex};
    Stats result{.heap_bytes = state->heap_bytes, .reserved_bytes = state->live_bytes,
                 .pending_bytes = state->pending_bytes};
    for (const auto& block : state->blocks) {
        for (const auto& [offset, bytes] : block.free_ranges) {
            result.free_bytes += bytes;
            result.largest_free_range = std::max(result.largest_free_range, bytes);
        }
    }
    return result;
}

u64 TextureResourceAllocator::HeapBytes() const {
    std::scoped_lock lock{state->mutex};
    return state->heap_bytes;
}

} // namespace D3D12
