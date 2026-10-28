// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#define NOMINMAX
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include "video_core/renderer_d3d12/d3d12_heap_growth_policy.h"

using Microsoft::WRL::ComPtr;
using namespace D3D12::HeapGrowth;
constexpr std::uint64_t M = 1ULL << 20;
void Check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D12 failure: " + std::to_string(hr));
}

void Policy() {
    const Budget healthy{3000*M, 5120*M}, low{4400*M, 5120*M};
    assert(!UnderPressure(64*M, healthy, {}));
    assert(UnderPressure(64*M, low, {}));
    assert(UnderPressure(64*M, {4288*M, 5120*M}, {}));
    assert(!UnderPressure(64*M, {4288*M-1, 5120*M}, {}));
    assert(UnderPressure(64*M, healthy, {3400*M, 4096*M}));
    assert(UnderPressure(64*M, {5200*M, 5120*M}, {}));
    assert(UnderPressure(UINT64_MAX, healthy, {}));
    assert(!UnderPressure(UINT64_MAX, {}, {}));
    assert(Capacity(12*M, 65536, true) == 12*M);
    assert(Capacity(12*M, 65536, false) == 64*M);
    assert(Capacity(1, 4096, true) == 65536);
    assert(Capacity(65*M, 4*M, true) == 68*M);
    assert(!Capacity(UINT64_MAX, 65536, true));
    const Budget pressured{4000*M, 5120*M}, exhausted{4900*M, 5120*M};
    assert(PressureCapacity(128*1024, 65536, pressured) == 16*M);
    assert(PressureCapacity(7*M, 65536, pressured, 0) == 7*M);
    assert(PressureCapacity(128*1024, 65536, pressured, 2*M) == 2*M);
    assert(PressureCapacity(128*1024, 65536, exhausted) == 128*1024);
    assert(small_render_target < 16*M);
    std::uint64_t cases{};
    for (std::uint64_t size = 1; size < 96*M; size += 7919) {
        for (auto alignment : {4096ULL, 65536ULL, 4*M}) {
            const auto compact_size = Capacity(size, alignment, true);
            const auto normal = Capacity(size, alignment, false);
            assert(compact_size >= size && compact_size <= normal);
            assert(compact_size % (std::max)(alignment, 65536ULL) == 0);
            assert(normal % (std::max)(alignment, 65536ULL) == 0);
            ++cases;
        }
    }
    std::cout << "PASS policy: " << cases << " sizes/alignments, budgets/overflow\n";
}

int main() try {
    Policy();
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;
    Check(device.As(&messages));
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC qdesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Check(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEvent");
    std::uint64_t signal{}, compared{}, normal_bytes{}, compact_bytes{};
    const auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        const D3D12_HEAP_PROPERTIES hp{type};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1,
            DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        ComPtr<ID3D12Resource> result;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state,
                                              nullptr, IID_PPV_ARGS(&result)));
        return result;
    };
    for (int variant = 0; variant < 6; ++variant) {
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 37, 19, 2, 3,
            DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN};
        if (variant == 1) {
            desc.Width = 40; desc.Height = 20; desc.Format = DXGI_FORMAT_BC1_UNORM;
        }
        if (variant == 2) {
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            desc.DepthOrArraySize = 5;
        }
        if (variant == 3) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (variant == 4) {
            desc.Format = DXGI_FORMAT_D32_FLOAT;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }
        if (variant == 5) {
            desc.Width = 64; desc.Height = 64; desc.MipLevels = 1;
            desc.SampleDesc.Count = 2;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
        assert(info.SizeInBytes != UINT64_MAX);
        const auto capacity = Capacity(info.SizeInBytes, info.Alignment, true);
        const D3D12_HEAP_DESC hd{capacity, {D3D12_HEAP_TYPE_DEFAULT},
            (std::max<UINT64>)(info.Alignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT),
            desc.Flags ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                       : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
        ComPtr<ID3D12Heap> heap;
        Check(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
        ComPtr<ID3D12Resource> texture;
        Check(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, IID_PPV_ARGS(&texture)));
        normal_bytes += Capacity(info.SizeInBytes, info.Alignment, false);
        compact_bytes += capacity;
        if (variant >= 4) continue; // Depth/MSAA validate legal allocation; no data conversion.
        const UINT count = desc.MipLevels *
            (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize);
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> row_bytes(count);
        UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, count, 0, layouts.data(), rows.data(),
                                      row_bytes.data(), &bytes);
        auto upload = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<unsigned char> expected(bytes);
        for (UINT64 i = 0; i < bytes; ++i) expected[i] = static_cast<unsigned char>((i*37+variant)%251);
        void* mapped{};
        const D3D12_RANGE none{0, 0};
        Check(upload->Map(0, &none, &mapped));
        std::memcpy(mapped, expected.data(), bytes);
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&list)));
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint = layouts[i];
            D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = i;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = layouts[i];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Check(list->Close());
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++signal));
        Check(fence->SetEventOnCompletion(signal, event));
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU fence timeout");
        const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)};
        Check(readback->Map(0, &read, &mapped));
        for (UINT i = 0; i < count; ++i) {
            const auto& f = layouts[i];
            for (UINT z = 0; z < f.Footprint.Depth; ++z)
                for (UINT row = 0; row < rows[i]; ++row) {
                    const auto offset = f.Offset + (z*rows[i]+row)*UINT64(f.Footprint.RowPitch);
                    assert(!std::memcmp(static_cast<unsigned char*>(mapped)+offset,
                                        expected.data()+offset, row_bytes[i]));
                    compared += row_bytes[i];
                }
        }
        readback->Unmap(0, &none);
    }
    CloseHandle(event);
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T bytes{};
        Check(messages->GetMessage(i, nullptr, &bytes));
        std::vector<unsigned char> storage(bytes);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(messages->GetMessage(i, msg, &bytes));
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(msg->pDescription);
    }
    std::cout << "PASS GPU/debug: 6 compact allocations (RGBA/BC1/3D/RT/depth/MSAA), "
              << compared << " bytes exact; capacity " << normal_bytes << " -> " << compact_bytes
              << " (synthetic, not app commit)\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
