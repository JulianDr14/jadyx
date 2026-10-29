// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstring>

#include <fmt/format.h>
#include <windows.h>
#include <wrl/client.h>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_memory_import_probe.h"

namespace D3D12 {
namespace {
using Microsoft::WRL::ComPtr;

constexpr UINT64 PROBE_SIZE = 64 * 1024;

std::atomic<bool> probe_enabled{};

u32 Pattern(size_t index) {
    return static_cast<u32>(index * 2654435761u) ^ 0x5a17c0deu;
}

D3D12_RESOURCE_DESC BufferDesc(D3D12_RESOURCE_FLAGS flags) {
    return D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = PROBE_SIZE,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {1, 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = flags,
    };
}

// Copies `source` to a readback buffer on a queue of its own and compares it with the pattern.
HRESULT CopyOut(ID3D12Device* device, ID3D12Resource* source, bool& matches) {
    D3D12_HEAP_PROPERTIES readback{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC desc = BufferDesc(D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> target;
    HRESULT hr = device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&target));
    const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator));
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&list));
    }
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        return hr;
    }
    list->CopyBufferRegion(target.Get(), 0, source, 0, PROBE_SIZE);
    if (hr = list->Close(); FAILED(hr)) {
        return hr;
    }
    ID3D12CommandList* const lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (hr = queue->Signal(fence.Get(), 1); FAILED(hr)) {
        return hr;
    }
    const HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (event == nullptr) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    hr = fence->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr)) WaitForSingleObjectEx(event, INFINITE, FALSE);
    CloseHandle(event);
    if (FAILED(hr)) {
        return hr;
    }
    if (hr = device->GetDeviceRemovedReason(); FAILED(hr)) {
        return hr;
    }
    void* mapped{};
    const D3D12_RANGE range{0, PROBE_SIZE};
    if (hr = target->Map(0, &range, &mapped); FAILED(hr)) {
        return hr;
    }
    const auto* const words = static_cast<const u32*>(mapped);
    matches = true;
    for (size_t i = 0; i < PROBE_SIZE / sizeof(u32); ++i) {
        matches = matches && words[i] == Pattern(i);
    }
    const D3D12_RANGE written{0, 0};
    target->Unmap(0, &written);
    return S_OK;
}

std::string Step(HRESULT hr) {
    return SUCCEEDED(hr) ? "yes" : hr == E_NOTIMPL ? "not tried" : fmt::format("no (0x{:08x})",
                                                                               static_cast<u32>(hr));
}

} // Anonymous namespace

MemoryImportProbe ProbeMemoryImport(ID3D12Device* device) {
    MemoryImportProbe probe;
    ComPtr<ID3D12Device3> device3;
    if (probe.open = device->QueryInterface(IID_PPV_ARGS(&device3)); FAILED(probe.open)) {
        return probe;
    }
    void* const block = VirtualAlloc(nullptr, PROBE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == nullptr) {
        probe.open = HRESULT_FROM_WIN32(GetLastError());
        return probe;
    }
    auto* const words = static_cast<u32*>(block);
    for (size_t i = 0; i < PROBE_SIZE / sizeof(u32); ++i) {
        words[i] = Pattern(i);
    }
    {
        ComPtr<ID3D12Heap> heap;
        probe.open = device3->OpenExistingHeapFromAddress(block, IID_PPV_ARGS(&heap));
        ComPtr<ID3D12Resource> buffer;
        if (SUCCEEDED(probe.open)) {
            const D3D12_RESOURCE_DESC desc = BufferDesc(D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
            probe.place = device->CreatePlacedResource(heap.Get(), 0, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&buffer));
        }
        if (SUCCEEDED(probe.place)) {
            probe.copy = CopyOut(device, buffer.Get(), probe.matches);
        }
    }
    VirtualFree(block, 0, MEM_RELEASE);
    return probe;
}

std::string DescribeMemoryImportProbe(const MemoryImportProbe& probe) {
    return "heap from an address " + Step(probe.open) + ", placed buffer " + Step(probe.place) +
           ", GPU copy " + Step(probe.copy) + ", data matches " +
           (SUCCEEDED(probe.copy) ? (probe.matches ? "yes" : "no") : "not tried");
}

void SetMemoryImportProbe(bool enabled) {
    probe_enabled.store(enabled, std::memory_order_relaxed);
}

bool GetMemoryImportProbe() {
    return probe_enabled.load(std::memory_order_relaxed);
}

} // namespace D3D12
