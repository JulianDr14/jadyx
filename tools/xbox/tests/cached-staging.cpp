// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Cached staging gate: with cached staging on, an upload buffer lives in write-back L0 memory and
// the GPU copies out of it what the CPU wrote; off, it is the UPLOAD heap again. Runs on the
// adapter and on WARP (a cache-coherent UMA device, like the console).
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include <dxgi1_4.h>
#include <iostream>
#include <stdexcept>
#include <string>
#pragma comment(lib, "dxgi.lib")
using namespace D3D12;
namespace {
constexpr u64 SIZE = 1 << 20;

/// Copies source into a DEFAULT buffer, then into readback, and returns the readback.
void GpuRoundTrip(ID3D12Device* device, ID3D12Resource* source, std::span<u8>& readback_bytes,
                  ComPtr<ID3D12Resource>& readback) {
    const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    ThrowIfFailed(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                            nullptr, IID_PPV_ARGS(&list)), "list");
    auto middle = CreateCommittedBuffer(device, SIZE, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                        "middle");
    readback = CreateMappedBuffer(device, SIZE, D3D12_HEAP_TYPE_READBACK, readback_bytes);
    list->CopyBufferRegion(middle.Get(), 0, source, 0, SIZE);
    TransitionResource(list.Get(), middle.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, middle.Get(), 0, SIZE);
    ThrowIfFailed(list->Close(), "close");
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    ThrowIfFailed(queue->Signal(fence.Get(), 1), "signal");
    ThrowIfFailed(fence->SetEventOnCompletion(1, nullptr), "wait");
}

std::string Check(ID3D12Device* device, const char* name) {
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch));
    const auto before = GetCachedStagingStats();

    SetCachedStaging(true);
    if (!CachedStaging()) throw std::runtime_error("switch not kept");
    std::span<u8> bytes;
    auto cached = CreateMappedBuffer(device, SIZE, D3D12_HEAP_TYPE_UPLOAD, bytes);
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    ThrowIfFailed(cached->GetHeapProperties(&heap, &flags), "GetHeapProperties");
    if (heap.Type != D3D12_HEAP_TYPE_CUSTOM ||
        heap.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
        heap.MemoryPoolPreference != D3D12_MEMORY_POOL_L0)
        throw std::runtime_error(std::string{name} + ": staging not in write-back L0 (heap type " +
                                 std::to_string(heap.Type) + ")");
    if (HeapType(cached.Get()) != D3D12_HEAP_TYPE_CUSTOM)
        throw std::runtime_error("HeapType does not report CUSTOM");
    const auto after = GetCachedStagingStats();
    if (after.buffers != before.buffers + 1 || after.bytes != before.bytes + SIZE || after.refused)
        throw std::runtime_error("stats miscounted");

    // Written, then read back on the CPU (what a repack does), then copied by the GPU.
    for (u64 i = 0; i < SIZE; ++i) bytes[i] = static_cast<u8>(i * 7 + i / 4096);
    u64 sum = 0;
    for (u64 i = 0; i < SIZE; i += 64) sum += bytes[i];
    if (sum == 0) throw std::runtime_error("CPU read back nothing");
    std::span<u8> out;
    ComPtr<ID3D12Resource> readback;
    GpuRoundTrip(device, cached.Get(), out, readback);
    for (u64 i = 0; i < SIZE; ++i) {
        if (out[i] != static_cast<u8>(i * 7 + i / 4096))
            throw std::runtime_error(std::string{name} + ": GPU copy differs at byte " +
                                     std::to_string(i));
    }

    SetCachedStaging(false);
    std::span<u8> plain_bytes;
    auto plain = CreateMappedBuffer(device, SIZE, D3D12_HEAP_TYPE_UPLOAD, plain_bytes);
    if (HeapType(plain.Get()) != D3D12_HEAP_TYPE_UPLOAD)
        throw std::runtime_error("off does not use the UPLOAD heap");
    return std::string{name} + (arch.CacheCoherentUMA ? " (cache-coherent UMA)" :
                                arch.UMA ? " (UMA)" : " (discrete)");
}
} // namespace
int main() try {
    Common::Log::Initialize();
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> hardware;
    ThrowIfFailed(factory->EnumAdapters1(0, &hardware), "adapter 0");
    ComPtr<ID3D12Device> hardware_device;
    ThrowIfFailed(D3D12CreateDevice(hardware.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&hardware_device)), "hardware device");
    const std::string first = Check(hardware_device.Get(), "adapter");
    ComPtr<IDXGIAdapter> warp;
    ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    ComPtr<ID3D12Device> warp_device;
    ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&warp_device)), "WARP device");
    const std::string second = Check(warp_device.Get(), "WARP");
    std::cout << "PASS cached staging: write-back L0 upload buffers written, read on the CPU and "
                 "copied by the GPU on "
              << first << " and " << second << "; off goes back to the UPLOAD heap\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
