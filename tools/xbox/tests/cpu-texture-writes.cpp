// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// CPU texture writes gate: on WARP (a cache-coherent UMA device, like the console) a plain BC
// texture lands in a CPU-writable heap and its first upload is written in place, mips smaller
// than a block included; the GPU reads back exactly those bytes, a second upload goes through
// staging, a render target stays out of the CPU heap, and the debug layer stays clean.
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace D3D12;
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;

namespace {
std::vector<BufferImageCopy> Layout(const ImageInfo& info, size_t& bytes) {
    std::vector<BufferImageCopy> copies;
    bytes = 0;
    const u32 block = VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 bpp = VideoCore::Surface::BytesPerBlock(info.format);
    for (s32 level = 0; level < info.resources.levels; ++level) {
        const u32 w = std::max(1U, info.size.width >> level);
        const u32 h = std::max(1U, info.size.height >> level);
        const u32 bw = (w + block - 1) / block, bh = (h + block - 1) / block;
        const size_t size = size_t{bw} * bh * bpp * info.resources.layers;
        copies.push_back({.buffer_offset = bytes,
                          .buffer_size = size,
                          .buffer_row_length = bw * block,
                          .buffer_image_height = bh * block,
                          .image_subresource = {level, 0, info.resources.layers},
                          .image_offset = {},
                          .image_extent = {w, h, 1}});
        bytes += size;
    }
    return copies;
}

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

bool DebugErrors(ID3D12InfoQueue* messages) {
    bool found = false;
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<u8> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << message->pDescription << '\n';
            found = true;
        }
    }
    return found;
}
} // namespace

int main() try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    _putenv("JADYX_D3D12_WARP=1");
    Device device;
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    ThrowIfFailed(device.Get()->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch,
                                                    sizeof(arch)), "ARCHITECTURE1");
    Require(arch.CacheCoherentUMA, "WARP is not cache-coherent UMA");
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device};
    StagingBufferPool staging{device, scheduler};
    TransferBufferPool transfers{device, scheduler};
    CpuDescriptorAllocator views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    CpuDescriptorAllocator samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    CpuDescriptorAllocator dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
    TextureCacheRuntime runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs};

    // Off: a plain texture stays in a DEFAULT heap.
    ImageInfo info;
    info.format = PixelFormat::BC1_RGBA_UNORM;
    info.type = ImageType::e2D;
    info.size = {64, 32, 1};
    info.resources = {7, 2}; // 64x32 down to 1x1: the last levels are smaller than a block
    SetCpuTextureWrites(false);
    {
        Image off{runtime, info, 0, 0, true};
        Require(!off.CpuWritable(), "a texture went to a CPU heap with the switch off");
    }

    SetCpuTextureWrites(true);
    Require(CpuTextureWrites(), "switch not kept");
    size_t bytes{};
    const auto copies = Layout(info, bytes);
    const auto pattern = [](size_t i, u32 seed) { return u8(i * 31 + i / 8 * 11 + seed); };
    const auto before = GetInPlaceTextureStats();
    Image image{runtime, info, 0, 0, true};
    Require(image.CpuWritable(), "plain BC1 texture not placed in a CPU-writable heap");
    auto upload = runtime.UploadStagingBuffer(bytes);
    for (size_t i = 0; i < bytes; ++i) upload.mapped_span[i] = pattern(i, 5);
    image.UploadMemory(upload, copies);
    const auto after = GetInPlaceTextureStats();
    Require(after.refused == before.refused, "the driver refused a write in place");
    Require(after.images == before.images + 1, "first upload not written in place");
    Require(after.bytes - before.bytes == bytes,
            "in-place bytes " + std::to_string(after.bytes - before.bytes) + " != " +
                std::to_string(bytes));
    auto readback = runtime.DownloadStagingBuffer(bytes);
    image.DownloadMemory(readback, copies);
    scheduler.Finish();
    for (size_t i = 0; i < bytes; ++i) {
        Require(readback.mapped_span[i] == pattern(i, 5),
                "first upload differs on the GPU at byte " + std::to_string(i));
    }

    // The GPU has used it now: the next upload goes through staging.
    auto second = runtime.UploadStagingBuffer(bytes);
    for (size_t i = 0; i < bytes; ++i) second.mapped_span[i] = pattern(i, 77);
    image.UploadMemory(second, copies);
    Require(GetInPlaceTextureStats().images == after.images, "second upload written in place");
    auto second_readback = runtime.DownloadStagingBuffer(bytes);
    image.DownloadMemory(second_readback, copies);
    scheduler.Finish();
    for (size_t i = 0; i < bytes; ++i) {
        Require(second_readback.mapped_span[i] == pattern(i, 77),
                "second upload differs on the GPU at byte " + std::to_string(i));
    }

    // A render-target-capable texture never goes to the CPU heap.
    ImageInfo color = info;
    color.format = PixelFormat::A8B8G8R8_UNORM;
    color.resources = {1, 1};
    Image target{runtime, color, 0, 0};
    Require(!target.CpuWritable(), "a render target went to a CPU-writable heap");

    scheduler.Finish();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "device removed");
    Require(!DebugErrors(messages.Get()), "D3D12 debug layer errors");
    SetCpuTextureWrites(false);
    std::cout << "PASS CPU texture writes: BC1 64x32, 7 levels x 2 layers (" << bytes
              << " bytes) written in place on WARP and read back by the GPU, second upload "
                 "through staging, render target and switch-off kept out of the CPU heap; "
                 "debug layer clean\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "CPU texture writes gate: " << e.what() << '\n';
    return 1;
}
