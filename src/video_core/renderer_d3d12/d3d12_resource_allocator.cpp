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
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "common/thread.h"

namespace D3D12 {

using namespace Common::Literals;

namespace {
std::atomic_bool small_texture_alignment{true};
std::atomic_bool spare_heaps{true};
std::atomic_bool cpu_texture_writes{false};
std::atomic_bool cpu_texture_refused{false};

enum class HeapClass : u8 {
    Texture,
    RenderTarget,
    SmallRenderTarget,
    TransientRenderTarget,
    CpuTexture, ///< plain textures the CPU writes in place (SetCpuTextureWrites)
};
constexpr size_t heap_classes = 5;

constexpr D3D12_RESOURCE_FLAGS RT_DS = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

HeapClass ClassOf(const D3D12_RESOURCE_DESC& desc, u64 size, bool transient, bool cpu_writable) {
    if ((desc.Flags & RT_DS) == 0) return cpu_writable ? HeapClass::CpuTexture : HeapClass::Texture;
    if (transient) return HeapClass::TransientRenderTarget;
    return size <= HeapGrowth::small_render_target ? HeapClass::SmallRenderTarget
                                                   : HeapClass::RenderTarget;
}

D3D12_HEAP_FLAGS FlagsOf(HeapClass heap_class) {
    return heap_class == HeapClass::Texture || heap_class == HeapClass::CpuTexture
               ? D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES
               : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
}

D3D12_HEAP_PROPERTIES PropertiesOf(HeapClass heap_class) {
    if (heap_class == HeapClass::CpuTexture) {
        return {.Type = D3D12_HEAP_TYPE_CUSTOM,
                .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
                .MemoryPoolPreference = D3D12_MEMORY_POOL_L0};
    }
    return {.Type = D3D12_HEAP_TYPE_DEFAULT};
}

/// CreateHeap with the pages left as the process freed them (NotZeroedFlag): every placed RT/DS
/// is discarded on first use and every other texture is uploaded or copied before it is sampled.
HRESULT CreatePoolHeap(ID3D12Device* device, D3D12_HEAP_DESC heap_desc, ComPtr<ID3D12Heap>& heap) {
    heap_desc.Flags |= NotZeroedFlag(device);
    HRESULT hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (RefuseNotZeroed(heap_desc.Flags, hr)) {
        heap_desc.Flags &= ~D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    }
    return hr;
}

/// The driver refused a CPU-writable heap or a texture in one: plain textures go back to
/// DEFAULT heaps and staging.
void RefuseCpuTextures(HRESULT hr) {
    cpu_texture_writes.store(false, std::memory_order_relaxed);
    if (!cpu_texture_refused.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render,
                    "D3D12: a texture was refused in a CPU-writable heap ({:#010x}); plain "
                    "textures go through staging from now on",
                    static_cast<u32>(hr));
    }
}

const char* NameOf(HeapClass heap_class) {
    switch (heap_class) {
    case HeapClass::Texture:
        return "texture";
    case HeapClass::RenderTarget:
        return "RT/DS";
    case HeapClass::SmallRenderTarget:
        return "small RT/DS";
    case HeapClass::TransientRenderTarget:
        return "transient RT/DS";
    case HeapClass::CpuTexture:
        return "CPU-writable texture";
    }
    return "?";
}

// Render targets are recreated at new sizes every few frames and pin whole heaps: one survivor
// kept a 64 MiB heap from emptying, leaving 600 MiB free across RT heaps that could not be
// released. Smaller RT heaps drain sooner and are trimmed.
u64 PoolOf(HeapClass heap_class) {
    switch (heap_class) {
    case HeapClass::Texture:
        return 64_MiB;
    case HeapClass::RenderTarget:
        return 32_MiB;
    case HeapClass::SmallRenderTarget:
        return 8_MiB;
    case HeapClass::TransientRenderTarget:
        return 16_MiB;
    case HeapClass::CpuTexture:
        return 32_MiB;
    }
    return 64_MiB;
}

// Padding of a heap grown under pressure. A large render target gets an exact heap: at world
// entry 16 MiB padded RT heaps each kept one survivor, leaving ~300 MiB free inside them that
// no trim could release. Small render targets arrive in bursts of hundreds and keep their full
// pool: 2 MiB padding still made hundreds of heaps. Transient ones are dropped within frames,
// so a padded heap of them empties instead of holding survivors.
u64 PressurePadOf(HeapClass heap_class) {
    switch (heap_class) {
    case HeapClass::Texture:
    case HeapClass::CpuTexture:
        return 16_MiB;
    case HeapClass::RenderTarget:
        return 0;
    case HeapClass::SmallRenderTarget:
    case HeapClass::TransientRenderTarget:
        return 8_MiB;
    }
    return 16_MiB;
}
} // Anonymous namespace

struct TextureResourceAllocator::State {
    struct Block {
        ComPtr<ID3D12Heap> heap;
        HeapClass heap_class{};
        u64 size{};
        u64 alignment{};
        u64 used{};
        std::map<u64, u64> free_ranges; // offset -> bytes
        Common::MemoryCharge charge;
        u64 empty_tick{}; // first trim pass that saw it empty; 0 while in use
        bool spare{};     // made ahead on the spare thread and not placed into yet
    };

    mutable std::mutex mutex;
    std::vector<Block> blocks;
    u64 heap_bytes{};
    u64 live_bytes{};
    u64 peak_bytes{};
    u64 placed_resources{};
    u64 transient_placed{};
    /// Textures placed at 4 KiB, and the bytes they saved against 64 KiB slots.
    u64 small_placed{};
    u64 small_saved_bytes{};
    u64 committed_fallbacks{};
    u64 trimmed_heaps{};
    u64 trimmed_bytes{};
    u64 pending_bytes{};
    u64 growth_recoveries{};
    u64 compact_heaps{};
    u64 avoided_capacity{};
    u64 pressure_waits{};
    u64 spares_made{};
    u64 spare_hits{};
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
        blocks[block_index].used -= released_size;
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

    /// Caller holds the mutex.
    [[nodiscard]] bool HasEmptyHeap(HeapClass heap_class) const {
        return std::ranges::any_of(blocks, [heap_class](const Block& block) {
            return block.heap && block.heap_class == heap_class && block.used == 0;
        });
    }

    /// Caller holds the mutex. Tombstones preserve outstanding Allocation indices: reuse only
    /// slots with no heap, which Trim proves hold no live or pending allocations.
    size_t Insert(Block&& block) {
        const auto vacant = std::ranges::find_if(blocks, [](const Block& b) { return !b.heap; });
        if (vacant == blocks.end()) {
            blocks.emplace_back(std::move(block));
            return blocks.size() - 1;
        }
        *vacant = std::move(block);
        return static_cast<size_t>(vacant - blocks.begin());
    }
};

struct TextureResourceAllocator::SpareWorker {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::array<bool, heap_classes> wanted{};
    std::jthread thread; ///< last: stopped before the members it uses

    void Request(HeapClass heap_class) {
        {
            std::scoped_lock lock{mutex};
            wanted[static_cast<size_t>(heap_class)] = true;
        }
        wake.notify_one();
    }

    // CreateHeap of a pool stalled the recording thread while the system zeroed the pages. The
    // spare is made here instead and joins the pool as an empty heap: the next growth finds it
    // like any other free range, KeepWarm trims keep it, and the first pressured trim releases it.
    static void Make(const Device& device, State& state, HeapClass heap_class) {
        const u64 pool = PoolOf(heap_class);
        {
            std::scoped_lock lock{state.mutex};
            if (state.HasEmptyHeap(heap_class)) return;
        }
        const auto memory = device.QueryCacheMemoryPressure();
        if (!memory.Known() ||
            !HeapGrowth::WantSpare(false, pool, {memory.app_used, memory.app_limit},
                                   {memory.gpu_used, memory.gpu_budget})) {
            return;
        }
        State::Block block{
            .heap_class = heap_class, .size = pool,
            .alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, .spare = true};
        const D3D12_HEAP_DESC heap_desc{
            .SizeInBytes = pool,
            .Properties = PropertiesOf(heap_class),
            .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
            .Flags = FlagsOf(heap_class),
        };
        if (FAILED(CreatePoolHeap(device.Get(), heap_desc, block.heap))) return;
        block.free_ranges.emplace(0, pool);
        block.charge = Common::MemoryCharge(Common::MemoryAccount::TextureHeaps, pool);
        std::scoped_lock lock{state.mutex};
        // Growth made one meanwhile, or a release emptied one: this spare is not needed.
        if (state.HasEmptyHeap(heap_class)) return;
        state.Insert(std::move(block));
        state.heap_bytes += pool;
        ++state.spares_made;
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

void SetSmallTextureAlignment(bool enabled) {
    small_texture_alignment.store(enabled, std::memory_order_relaxed);
}

void SetSpareHeaps(bool enabled) {
    spare_heaps.store(enabled, std::memory_order_relaxed);
}

void SetCpuTextureWrites(bool enabled) {
    cpu_texture_writes.store(enabled && !cpu_texture_refused.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
}

bool CpuTextureWrites() noexcept {
    return cpu_texture_writes.load(std::memory_order_relaxed);
}

TextureResourceAllocator::TextureResourceAllocator(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_}, state{std::make_shared<State>()},
      spare_worker{std::make_unique<SpareWorker>()} {
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    cache_coherent_uma = SUCCEEDED(device.Get()->CheckFeatureSupport(
                             D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch))) &&
                         arch.CacheCoherentUMA;
    spare_worker->thread = std::jthread{[this](std::stop_token stop) {
        Common::SetCurrentThreadName("D3D12SpareHeaps");
        auto& worker = *spare_worker;
        while (true) {
            std::array<bool, heap_classes> wanted{};
            {
                std::unique_lock lock{worker.mutex};
                if (!worker.wake.wait(lock, stop, [&] {
                        return std::ranges::find(worker.wanted, true) != worker.wanted.end();
                    })) {
                    return;
                }
                wanted = std::exchange(worker.wanted, {});
            }
            for (size_t type = 0; type < heap_classes; ++type) {
                if (wanted[type]) SpareWorker::Make(device, *state, static_cast<HeapClass>(type));
            }
        }
    }};
}

TextureResourceAllocator::~TextureResourceAllocator() {
    spare_worker.reset();
}

TextureResourceAllocator::Resource TextureResourceAllocator::Create(
    const D3D12_RESOURCE_DESC& requested, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE* clear_value, HeapPacking::TexelBlock texel_block, bool cpu_writable) {
    return CreateImpl(requested, initial_state, clear_value, texel_block, cpu_writable, false, {});
}

TextureResourceAllocator::Resource TextureResourceAllocator::CreateInUsedHeaps(
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial_state, bool cpu_writable,
    const std::vector<bool>& avoid) {
    return CreateImpl(desc, initial_state, nullptr, {}, cpu_writable, true, avoid);
}

TextureResourceAllocator::Resource TextureResourceAllocator::CreateImpl(
    const D3D12_RESOURCE_DESC& requested, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE* clear_value, HeapPacking::TexelBlock texel_block, bool cpu_writable,
    bool in_used_heaps, const std::vector<bool>& avoid) {
    // Small sampled textures sit at 4 KiB instead of 64 KiB: icons, glyphs and LUTs each wasted
    // most of a 64 KiB slot. The device may still refuse (a larger alignment or an invalid size),
    // and the texture goes back to the default alignment.
    D3D12_RESOURCE_DESC desc = requested;
    D3D12_RESOURCE_ALLOCATION_INFO info{UINT64_MAX, 0};
    if (desc.Alignment == 0 && small_texture_alignment.load(std::memory_order_relaxed) &&
        HeapPacking::SmallPlacementCandidate(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                             (desc.Flags & RT_DS) != 0, desc.SampleDesc.Count,
                                             desc.Width, desc.Height, texel_block)) {
        desc.Alignment = HeapPacking::small_alignment;
        info = device.Get()->GetResourceAllocationInfo(0, 1, &desc);
        if (info.SizeInBytes == UINT64_MAX || info.Alignment != HeapPacking::small_alignment) {
            desc.Alignment = 0;
            info = {UINT64_MAX, 0};
        }
    }
    if (info.SizeInBytes == UINT64_MAX) {
        info = device.Get()->GetResourceAllocationInfo(0, 1, &desc);
    }
    if (info.SizeInBytes == UINT64_MAX || info.Alignment == 0) {
        return {};
    }
    const State::CensusKey shape{static_cast<u32>(desc.Format), desc.Width, desc.Height,
                                 desc.DepthOrArraySize, desc.MipLevels,
                                 static_cast<u32>(desc.Flags)};
    bool transient = false;
    // An image old enough to be moved is not transient, whatever its shape does now.
    if ((desc.Flags & RT_DS) && !in_used_heaps) {
        std::scoped_lock lock{state->mutex};
        const auto known = state->census.find(shape);
        transient = known != state->census.end() &&
                    HeapGrowth::TransientShape(known->second.created, known->second.live);
    }
    const bool in_cpu_heap = cpu_writable && cache_coherent_uma && CpuTextureWrites() &&
                             (desc.Flags & RT_DS) == 0;
    const HeapClass heap_class = ClassOf(desc, info.SizeInBytes, transient, in_cpu_heap);
    const u64 heap_alignment =
        std::max<u64>(info.Alignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    size_t selected = SIZE_MAX;
    u64 selected_offset{};
    u64 best_waste = UINT64_MAX;
    u64 best_block_free = UINT64_MAX;
    // The spare thread may grow the block vector: read the heap only under the pool lock. The
    // reserved range keeps trim from releasing it.
    ID3D12Heap* selected_heap{};
    const auto reserve_space = [&] {
        auto& block = state->blocks[selected];
        selected_heap = block.heap.Get();
        if (block.spare) {
            block.spare = false;
            ++state->spare_hits;
        }
        auto& ranges = block.free_ranges;
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
        block.empty_tick = 0;
        block.used += info.SizeInBytes;
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
        // A small render target with no room in its own heaps takes free space in the large RT
        // heaps before growing: under pressure, splitting them off made 489 tiny heaps while
        // ~250 MiB sat free in RT heaps.
        const auto search = [&](HeapClass wanted_class) {
            for (size_t index = 0; index < state->blocks.size(); ++index) {
                auto& block = state->blocks[index];
                if (!block.heap || block.heap_class != wanted_class ||
                    block.alignment < heap_alignment || (index < avoid.size() && avoid[index]) ||
                    (in_used_heaps && block.used == 0)) {
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
        };
        search(heap_class);
        if (selected == SIZE_MAX && heap_class == HeapClass::SmallRenderTarget) {
            search(HeapClass::RenderTarget);
        }
        // Reserve while still holding the pool lock so a concurrent trim cannot remove it.
        if (selected != SIZE_MAX) reserve_space();
    };
    find_space();
    if (in_used_heaps && selected == SIZE_MAX) return {};

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
                                                      {after.app_used, after.app_limit},
                                                      PressurePadOf(heap_class));
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

    bool want_spare = false;
    HRESULT cpu_heap_refused = S_OK;
    {
        std::scoped_lock lock{state->mutex};

        if (selected == SIZE_MAX) {
            if (!wanted) wanted = HeapGrowth::Capacity(info.SizeInBytes, info.Alignment, compact,
                                                         PoolOf(heap_class));
            if (!wanted) return {};
            const D3D12_HEAP_DESC heap_desc{
                .SizeInBytes = wanted,
                .Properties = PropertiesOf(heap_class),
                .Alignment = heap_alignment,
                .Flags = FlagsOf(heap_class),
            };
            State::Block block{.heap_class = heap_class, .size = wanted,
                               .alignment = heap_alignment};
            const HRESULT heap_hr = CreatePoolHeap(device.Get(), heap_desc, block.heap);
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
                selected = state->Insert(std::move(block));
                selected_offset = 0;
                // Pooled growth: the class is filling, so the next heap is made off the frame.
                want_spare = !compact &&
                             heap_alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
                             spare_heaps.load(std::memory_order_relaxed);
                reserve_space();
                LOG_INFO(Render, "D3D12: texture heap pool added {} MiB {} block ({} MiB total), "
                         "required {} bytes, pressure-sized {}",
                         wanted / 1_MiB, NameOf(heap_class), state->heap_bytes / 1_MiB,
                         info.SizeInBytes, compact);
            } else if (heap_class == HeapClass::CpuTexture && heap_hr != E_OUTOFMEMORY) {
                cpu_heap_refused = heap_hr;
            } else {
                LOG_WARNING(Render, "D3D12: CreateHeap for texture pool failed 0x{:08x}; using committed resources",
                            static_cast<u32>(heap_hr));
            }
        }
    }
    if (FAILED(cpu_heap_refused)) {
        RefuseCpuTextures(cpu_heap_refused);
        return CreateImpl(requested, initial_state, clear_value, texel_block, false,
                          in_used_heaps, avoid);
    }

    if (want_spare) spare_worker->Request(heap_class);

    // A sudden jump in commit otherwise has no owner in the log.
    if (info.SizeInBytes >= 128ULL * 1024 * 1024) {
        LOG_WARNING(Render, "D3D12: large texture: {} MiB, {}x{}x{} format {}, {} mips",
                    info.SizeInBytes / (1024 * 1024), desc.Width, desc.Height,
                    desc.DepthOrArraySize, static_cast<u32>(desc.Format), desc.MipLevels);
    }

    Resource result;
    if (selected != SIZE_MAX) {
        const HRESULT placed_hr = device.Get()->CreatePlacedResource(
            selected_heap, selected_offset, &desc, initial_state, clear_value,
            IID_PPV_ARGS(&result.resource));
        if (SUCCEEDED(placed_hr)) {
            result.allocation = std::make_shared<Allocation>();
            result.allocation->state = state;
            result.allocation->block_index = selected;
            result.allocation->offset = selected_offset;
            result.allocation->size = info.SizeInBytes;
            result.placed = true;
            result.cpu_writable = heap_class == HeapClass::CpuTexture;
            std::scoped_lock lock{state->mutex};
            ++state->placed_resources;
            state->transient_placed += heap_class == HeapClass::TransientRenderTarget;
            if (desc.Alignment == HeapPacking::small_alignment) {
                ++state->small_placed;
                state->small_saved_bytes +=
                    Common::AlignUp(info.SizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) -
                    info.SizeInBytes;
            }
            auto& entry = state->census[shape];
            // A move is not a new texture: counting it would make the shape look transient.
            entry.created += !in_used_heaps;
            ++entry.live;
            entry.live_bytes += info.SizeInBytes;
            result.allocation->census = &entry;
            return result;
        }
        state->Free(selected, selected_offset, info.SizeInBytes);
        if (in_used_heaps) return {};
        if (heap_class == HeapClass::CpuTexture && placed_hr != E_OUTOFMEMORY) {
            RefuseCpuTextures(placed_hr);
            return CreateImpl(requested, initial_state, clear_value, texel_block, false,
                          in_used_heaps, avoid);
        }
        LOG_WARNING(Render, "D3D12: CreatePlacedResource failed 0x{:08x}; using committed resource",
                    static_cast<u32>(placed_hr));
    }

    // A committed resource takes a whole allocation of its own: the 4 KiB placement buys nothing.
    desc.Alignment = 0;
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
    std::array<bool, heap_classes> warm{};
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

std::vector<bool> TextureResourceAllocator::SparseHeaps(u32 max_used_percent) const {
    std::scoped_lock lock{state->mutex};
    std::vector<bool> sparse(state->blocks.size());
    for (size_t index = 0; index < state->blocks.size(); ++index) {
        const auto& block = state->blocks[index];
        sparse[index] = block.heap && block.used != 0 &&
                        block.used * 100 <= block.size * max_used_percent;
    }
    return sparse;
}

size_t TextureResourceAllocator::HeapOf(const Allocation& allocation) {
    return allocation.block_index;
}

std::string TextureResourceAllocator::Report() const {
    std::scoped_lock lock{state->mutex};
    // Diagnostic: per class heaps, empty heaps, free bytes and largest free range.
    std::array<u64, heap_classes> count{}, empty{}, free{}, largest{};
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
             "max {} | small rt n {} free {} empty {} max {} | transient rt n {} free {} empty {} "
             "max {} MiB, pending {} MiB",
             count[0], free[0] / 1_MiB, empty[0] / 1_MiB, largest[0] / 1_MiB, count[1],
             free[1] / 1_MiB, empty[1] / 1_MiB, largest[1] / 1_MiB, count[2], free[2] / 1_MiB,
             empty[2] / 1_MiB, largest[2] / 1_MiB, count[3], free[3] / 1_MiB, empty[3] / 1_MiB,
             largest[3] / 1_MiB, state->pending_bytes / 1_MiB);
    return fmt::format("texture heaps {} MiB, live {} MiB, peak {} MiB, placed {} (transient {}, 4 KiB {} saving "
                       "{} MiB), fallback {}, trimmed {} ({} MiB), "
                       "growth recoveries {}, pressure waits {}, pressure heaps {}, "
                       "avoided capacity {} MiB, spare heaps {} made {} used",
                       state->heap_bytes / 1_MiB, state->live_bytes / 1_MiB,
                       state->peak_bytes / 1_MiB, state->placed_resources, state->transient_placed,
                       state->small_placed, state->small_saved_bytes / 1_MiB,
                       state->committed_fallbacks, state->trimmed_heaps, state->trimmed_bytes / 1_MiB,
                       state->growth_recoveries, state->pressure_waits, state->compact_heaps,
                       state->avoided_capacity / 1_MiB, state->spares_made, state->spare_hits);
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
