// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"

namespace D3D12 {

namespace {
std::atomic<bool> not_zeroed_heaps{true};
std::atomic<bool> not_zeroed_refused{false};
} // Anonymous namespace

void SetNotZeroedHeaps(bool enabled) {
    not_zeroed_heaps.store(enabled && !not_zeroed_refused.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
}

D3D12_HEAP_FLAGS NotZeroedFlag(ID3D12Device* device) {
    // The OS zeroed new memory on the creating thread: at area loads 20-50 ms per heap, and most
    // of the page faults of the frame.
    static const bool supported = [device] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
        const bool result = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                                  &options, sizeof(options)));
        LOG_INFO(Render, "D3D12: memory created without zeroing: {}",
                 result ? "supported" : "not supported");
        return result;
    }();
    return supported && not_zeroed_heaps.load(std::memory_order_relaxed)
               ? D3D12_HEAP_FLAG_CREATE_NOT_ZEROED
               : D3D12_HEAP_FLAG_NONE;
}

bool RefuseNotZeroed(D3D12_HEAP_FLAGS used, HRESULT hr) {
    if ((used & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) == 0 || SUCCEEDED(hr) || hr == E_OUTOFMEMORY) {
        return false;
    }
    not_zeroed_heaps.store(false, std::memory_order_relaxed);
    if (!not_zeroed_refused.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render, "D3D12: memory created without zeroing was refused ({:#010x}); "
                    "the OS zeroes it from now on", static_cast<u32>(hr));
    }
    return true;
}

ComPtr<ID3D12Resource> CreateCommittedBuffer(ID3D12Device* device, u64 size,
                                             D3D12_HEAP_TYPE heap_type,
                                             D3D12_RESOURCE_STATES initial_state,
                                             D3D12_RESOURCE_FLAGS flags, const char* what,
                                             HRESULT* hr, D3D12_HEAP_FLAGS heap_flags) {
    const D3D12_HEAP_PROPERTIES heap{.Type = heap_type};
    const D3D12_RESOURCE_DESC desc = BufferDesc(size, flags);
    ComPtr<ID3D12Resource> buffer;
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::ResourceCreateUs,
                                       VideoCore::Perf::Counter::ResourcesCreated};
    // A sudden jump in commit otherwise has no owner in the log.
    if (size >= 128ULL * 1024 * 1024) {
        LOG_WARNING(Render, "D3D12: large buffer: {} MiB, {}", size / (1024 * 1024), what);
    }
    HRESULT result = device->CreateCommittedResource(&heap, heap_flags, &desc, initial_state,
                                                     nullptr, IID_PPV_ARGS(&buffer));
    if (RefuseNotZeroed(heap_flags, result)) {
        result = device->CreateCommittedResource(
            &heap, heap_flags & ~D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc, initial_state,
            nullptr, IID_PPV_ARGS(&buffer));
    }
    if (hr) {
        *hr = result;
        if (result == E_OUTOFMEMORY) {
            return {};
        }
    }
    ThrowIfFailed(result, what);
    CheckRemovedAfter(device, [&] {
        return fmt::format("{} ({} bytes, heap {})", what, size, static_cast<u32>(heap_type));
    });
    return buffer;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                const D3D12_ROOT_SIGNATURE_DESC& desc,
                                                const char* what) {
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    const HRESULT hr =
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) {
        std::string message = "(no details)";
        if (error && error->GetBufferSize() > 0) {
            message.assign(static_cast<const char*>(error->GetBufferPointer()),
                           error->GetBufferSize());
        }
        throw std::runtime_error(fmt::format("D3D12: serializing the {} failed (0x{:08X}): {}",
                                             what, static_cast<u32>(hr), message));
    }
    ComPtr<ID3D12RootSignature> root_signature;
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&root_signature)),
                  what);
    return root_signature;
}

D3D12_HEAP_TYPE HeapType(ID3D12Resource* resource) {
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(resource->GetHeapProperties(&properties, &flags))) {
        return D3D12_HEAP_TYPE_DEFAULT;
    }
    return properties.Type;
}

} // namespace D3D12
