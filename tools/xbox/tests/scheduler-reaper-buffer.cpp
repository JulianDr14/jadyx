// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Real-GPU component gate for D3D12Reaper background COM release:
// verifies D3D12Reaper off-thread COM release, synchronous retire callbacks,
// DeferReleaseImmediate, CollectGarbage(true) synchronous OOM wait, ~Scheduler() drain,
// TextureResourceAllocator::TrimEmptyHeaps reaper offload, and NotZeroedFlag 64 KiB UAV
// default buffer GPU copy round-trip.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"

namespace {

using namespace D3D12;

void Check(bool ok, const char* what) {
    if (!ok) {
        throw std::runtime_error(what);
    }
}

struct ProbeState {
    std::atomic<int> destroyed{0};
    std::mutex mu;
    std::thread::id destroy_tid{};
};

struct TrackedCom final : IUnknown {
    explicit TrackedCom(ProbeState& s, std::chrono::milliseconds delay = {})
        : state{s}, release_delay{delay} {}
    ~TrackedCom() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown) {
            AddRef();
            *ppv = static_cast<IUnknown*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            {
                std::lock_guard lock{state.mu};
                state.destroy_tid = std::this_thread::get_id();
            }
            if (release_delay.count() > 0) {
                std::this_thread::sleep_for(release_delay);
            }
            state.destroyed.fetch_add(1, std::memory_order_release);
            delete this;
        }
        return rem;
    }

    ProbeState& state;
    std::chrono::milliseconds release_delay{};
    std::atomic<ULONG> refs{1};
};

ComPtr<IUnknown> MakeProbe(ProbeState& s, std::chrono::milliseconds delay = {}) {
    ComPtr<IUnknown> out;
    out.Attach(new TrackedCom(s, delay));
    return out;
}

void RecordSignalWork(Scheduler& scheduler, ID3D12Resource* dst, ID3D12Resource* src) {
    scheduler.CommandList()->CopyBufferRegion(dst, 0, src, 0, 256);
}

void WaitUntil(const std::atomic<int>& counter, int target, const char* what) {
    for (int i = 0; i < 2000 && counter.load(std::memory_order_acquire) < target; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(counter.load(std::memory_order_acquire) == target, what);
}

} // namespace

int main() try {
    Common::Log::Initialize();
    Common::Log::Start();
    Settings::values.renderer_debug = false;

    const std::thread::id main_tid = std::this_thread::get_id();
    Device device;
    Scheduler scheduler{device};

    constexpr u64 COPY_BYTES = 65536;
    const D3D12_HEAP_FLAGS not_zeroed = NotZeroedFlag(device.Get());
    auto upload = CreateCommittedBuffer(device.Get(), COPY_BYTES, D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                                        "reaper gate upload");
    auto default_uav = CreateCommittedBuffer(
        device.Get(), COPY_BYTES, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "reaper gate uav default", nullptr, not_zeroed);
    auto readback = CreateCommittedBuffer(device.Get(), COPY_BYTES, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                          "reaper gate readback");

    // (a) DeferRelease without retire callback offloads destruction to D3D12Reaper.
    {
        ProbeState probe_state;
        scheduler.DeferRelease(MakeProbe(probe_state));
        RecordSignalWork(scheduler, default_uav.Get(), upload.Get());
        const u64 tick = scheduler.Flush();
        scheduler.Wait(tick);
        scheduler.CollectGarbage(false);
        scheduler.CollectGarbage(true);
        WaitUntil(probe_state.destroyed, 1, "DeferRelease probe was not destroyed");
        std::lock_guard lock{probe_state.mu};
        Check(probe_state.destroy_tid != std::thread::id{} && probe_state.destroy_tid != main_tid,
              "DeferRelease without retire callback must destroy on D3D12Reaper thread");
    }

    // (b) DeferRelease with retire callback releases synchronously before calling retire.
    {
        ProbeState probe_state;
        bool retire_ran = false;
        bool destroyed_before_retire = false;
        std::thread::id retire_tid{};
        scheduler.DeferRelease(MakeProbe(probe_state), [&] {
            retire_ran = true;
            retire_tid = std::this_thread::get_id();
            destroyed_before_retire = probe_state.destroyed.load(std::memory_order_acquire) == 1;
        });
        RecordSignalWork(scheduler, default_uav.Get(), upload.Get());
        const u64 tick = scheduler.Flush();
        scheduler.Wait(tick);
        scheduler.CollectGarbage(false);
        Check(retire_ran, "retire callback did not execute");
        Check(destroyed_before_retire, "retire callback must run after COM object destruction");
        Check(retire_tid == main_tid, "retire callback must run on caller thread");
        std::lock_guard lock{probe_state.mu};
        Check(probe_state.destroy_tid == main_tid,
              "DeferRelease with retire callback must release COM object synchronously on caller");
    }

    // (c) DeferReleaseImmediate offloads idle object to D3D12Reaper.
    {
        ProbeState probe_state;
        scheduler.DeferReleaseImmediate(MakeProbe(probe_state));
        scheduler.CollectGarbage(true);
        WaitUntil(probe_state.destroyed, 1, "DeferReleaseImmediate probe was not destroyed");
        std::lock_guard lock{probe_state.mu};
        Check(probe_state.destroy_tid != std::thread::id{} && probe_state.destroy_tid != main_tid,
              "DeferReleaseImmediate must destroy on D3D12Reaper thread");
    }

    // (d) ~Scheduler() drains pending_releases and waits for D3D12Reaper before returning.
    {
        ProbeState drain_state;
        {
            Scheduler local_scheduler{device};
            local_scheduler.DeferRelease(MakeProbe(drain_state));
            local_scheduler.DeferReleaseImmediate(MakeProbe(drain_state));
        }
        Check(drain_state.destroyed.load(std::memory_order_acquire) == 2,
              "~Scheduler() must drain all pending and immediate reaper COM releases");
    }

    // (e) TextureResourceAllocator::TrimEmptyHeaps KeepWarm (reaper offload) + All (sync trim).
    {
        SetSpareHeaps(false);
        TextureResourceAllocator allocator{device, scheduler};
        const D3D12_RESOURCE_DESC tex_desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = 2048,
            .Height = 2048,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_NONE,
        };
        std::vector<TextureResourceAllocator::Resource> textures;
        textures.reserve(10);
        for (int i = 0; i < 10; ++i) {
            textures.push_back(allocator.Create(tex_desc, D3D12_RESOURCE_STATE_COMMON));
            Check(textures.back().resource != nullptr, "TextureResourceAllocator texture create");
        }
        Check(allocator.HeapBytes() > 0, "TextureResourceAllocator must allocate heap bytes");
        for (auto& tex : textures) {
            allocator.DeferRelease(std::move(tex));
        }
        textures.clear();
        RecordSignalWork(scheduler, default_uav.Get(), upload.Get());
        const u64 tick = scheduler.Flush();
        scheduler.Wait(tick);
        scheduler.CollectGarbage(false);
        allocator.TrimEmptyHeaps(TextureResourceAllocator::TrimMode::KeepWarm);
        allocator.TrimEmptyHeaps(TextureResourceAllocator::TrimMode::All);
        scheduler.Finish();
        Check(allocator.HeapBytes() == 0,
              "TrimEmptyHeaps(All) must release every empty heap back to zero bytes");
    }

    // (f) NotZeroedFlag + 64 KiB UAV DEFAULT committed buffer real GPU copy round-trip.
    {
        std::vector<u8> expected(COPY_BYTES);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = static_cast<u8>((i * 73 + (i >> 8) * 19 + 7) & 0xFF);
        }
        void* mapped_upload = nullptr;
        const D3D12_RANGE no_read{0, 0};
        ThrowIfFailed(upload->Map(0, &no_read, &mapped_upload), "map upload");
        std::memcpy(mapped_upload, expected.data(), COPY_BYTES);
        upload->Unmap(0, nullptr);

        scheduler.CommandList()->CopyBufferRegion(default_uav.Get(), 0, upload.Get(), 0,
                                                  COPY_BYTES);
        TransitionResource(scheduler.CommandList(), default_uav.Get(),
                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, default_uav.Get(), 0,
                                                  COPY_BYTES);
        scheduler.Finish();

        void* mapped_readback = nullptr;
        const D3D12_RANGE full_range{0, COPY_BYTES};
        ThrowIfFailed(readback->Map(0, &full_range, &mapped_readback), "map readback");
        const bool match = std::memcmp(mapped_readback, expected.data(), COPY_BYTES) == 0;
        readback->Unmap(0, &no_read);
        Check(match, "64 KiB NotZeroed UAV default buffer GPU copy mismatch");
    }

    // (g) Synchronous OOM reclamation: CollectGarbage(true) and
    // TransferBufferPool::ReclaimAfterOutOfMemory block until D3D12Reaper completes queued
    // releases (no polling / WaitUntil needed before allocation retry).
    {
        ProbeState oom_state;
        scheduler.DeferRelease(MakeProbe(oom_state, std::chrono::milliseconds{15}));
        scheduler.DeferRelease(MakeProbe(oom_state, std::chrono::milliseconds{15}));
        RecordSignalWork(scheduler, default_uav.Get(), upload.Get());
        const u64 tick = scheduler.Flush();
        scheduler.Wait(tick);
        scheduler.CollectGarbage(false);
        scheduler.DeferReleaseImmediate(MakeProbe(oom_state, std::chrono::milliseconds{15}));
        scheduler.DeferReleaseImmediate(MakeProbe(oom_state, std::chrono::milliseconds{15}));
        scheduler.CollectGarbage(true);
        Check(oom_state.destroyed.load(std::memory_order_acquire) == 4,
              "CollectGarbage(true) must wait until D3D12Reaper finishes all COM releases before returning");
        {
            std::lock_guard lock{oom_state.mu};
            Check(oom_state.destroy_tid != std::thread::id{} && oom_state.destroy_tid != main_tid,
                  "OOM reaper probes must be destroyed on D3D12Reaper thread");
        }

        TransferBufferPool pool{device, scheduler};
        bool reclaimer_ran = false;
        int destroyed_when_reclaimer_ran = 0;
        pool.SetReclaimer([&] {
            reclaimer_ran = true;
            destroyed_when_reclaimer_ran = oom_state.destroyed.load(std::memory_order_acquire);
        });
        scheduler.DeferRelease(MakeProbe(oom_state, std::chrono::milliseconds{15}));
        RecordSignalWork(scheduler, default_uav.Get(), upload.Get());
        const u64 pool_tick = scheduler.Flush();
        scheduler.Wait(pool_tick);
        scheduler.CollectGarbage(false);
        scheduler.DeferReleaseImmediate(MakeProbe(oom_state, std::chrono::milliseconds{15}));
        Check(pool.ReclaimAfterOutOfMemory(1024, "reaper oom test"),
              "ReclaimAfterOutOfMemory must run on recording thread");
        Check(reclaimer_ran, "TransferBufferPool OOM reclaimer callback must execute");
        Check(destroyed_when_reclaimer_ran == 6 &&
                  oom_state.destroyed.load(std::memory_order_acquire) == 6,
              "ReclaimAfterOutOfMemory must finish D3D12Reaper destruction before reclaimer/retry");
    }

    std::cout << "PASS: scheduler-reaper-buffer GPU gate (D3D12Reaper off-thread COM release, "
                 "sync retire, TrimEmptyHeaps reaper, CollectGarbage(true) OOM barrier, 64 KiB NOT_ZEROED UAV round-trip)\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
