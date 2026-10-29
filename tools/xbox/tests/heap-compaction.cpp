// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Heap compaction gate: on WARP with the debug layer, a live texture left alone in a mostly
// free heap moves into free space of another heap in use, never into a new or empty heap; the
// emptied heap is released by trim; and moved BC, render target and depth images keep their
// texels (read back by the GPU) with the debug layer clean.
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
#include <memory>
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

u8 Pattern(size_t i, u32 seed) {
    return u8(i * 29 + i / 16 * 7 + seed);
}

// Fills `count` images of `info`, then keeps only those of the first heap at `keep` (indices),
// so that heap is mostly free while the next one holds the rest.
struct Spread {
    std::vector<std::unique_ptr<Image>> images;
    size_t first_heap{SIZE_MAX};
    size_t next_heap{SIZE_MAX};
};

Spread Fill(TextureCacheRuntime& runtime, const ImageInfo& info, size_t max_images,
            bool native_blocks) {
    Spread spread;
    for (size_t i = 0; i < max_images; ++i) {
        auto image = std::make_unique<Image>(runtime, info, 0, 0, native_blocks);
        const size_t heap = image->HeapIndex();
        Require(heap != SIZE_MAX, "image not placed in a texture heap");
        if (spread.first_heap == SIZE_MAX) spread.first_heap = heap;
        spread.images.push_back(std::move(image));
        if (heap != spread.first_heap) {
            spread.next_heap = heap;
            return spread;
        }
    }
    throw std::runtime_error("the images never spilled into a second heap");
}

std::vector<bool> Avoid(size_t heap) {
    std::vector<bool> avoid(heap + 1);
    avoid[heap] = true;
    return avoid;
}

// Uploads a pattern, moves the image out of `from`, reads it back and compares.
void MoveAndCheck(TextureCacheRuntime& runtime, Scheduler& scheduler, Image& image,
                  const ImageInfo& info, size_t from, size_t to, u32 seed, const char* what,
                  bool compare) {
    size_t bytes{};
    const auto copies = Layout(info, bytes);
    if (compare) {
        auto upload = runtime.UploadStagingBuffer(bytes);
        for (size_t i = 0; i < bytes; ++i) upload.mapped_span[i] = Pattern(i, seed);
        image.UploadMemory(upload, copies);
    }
    const auto before = GetHeapCompactionStats();
    Require(image.Relocate(Avoid(from)), std::string{what} + " did not move");
    Require(image.HeapIndex() == to, std::string{what} + " moved into heap " +
                                         std::to_string(image.HeapIndex()) + ", not " +
                                         std::to_string(to));
    Require(GetHeapCompactionStats().images == before.images + 1,
            std::string{what} + " move not counted");
    if (!compare) return;
    auto readback = runtime.DownloadStagingBuffer(bytes);
    image.DownloadMemory(readback, copies);
    scheduler.Finish();
    for (size_t i = 0; i < bytes; ++i) {
        Require(readback.mapped_span[i] == Pattern(i, seed),
                std::string{what} + " differs after the move at byte " + std::to_string(i));
    }
}
} // namespace

int main() try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    _putenv("JADYX_D3D12_WARP=1");
    SetSpareHeaps(false);
    SetCpuTextureWrites(false);
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device};

    // Allocator: 4 MiB textures, 16 to a 64 MiB heap.
    {
        TextureResourceAllocator allocator{device, scheduler};
        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 1024, .Height = 1024,
            .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN};
        std::vector<TextureResourceAllocator::Resource> resources;
        size_t first = SIZE_MAX, next = SIZE_MAX;
        while (next == SIZE_MAX) {
            Require(resources.size() < 64, "allocator never spilled into a second heap");
            auto created = allocator.Create(desc, D3D12_RESOURCE_STATE_COMMON);
            Require(created.placed, "texture not placed");
            const size_t heap = TextureResourceAllocator::HeapOf(*created.allocation);
            if (first == SIZE_MAX) first = heap;
            if (heap != first) next = heap;
            resources.push_back(std::move(created));
        }
        // Keep one texture in the first heap, release the others there.
        TextureResourceAllocator::Resource survivor = std::move(resources[1]);
        for (size_t i = 0; i + 1 < resources.size(); ++i) {
            if (i != 1) allocator.DeferRelease(std::move(resources[i]));
        }
        TextureResourceAllocator::Resource in_next = std::move(resources.back());
        resources.clear();
        scheduler.Finish();
        scheduler.CollectGarbage();
        const auto sparse = allocator.SparseHeaps(50);
        Require(first < sparse.size() && sparse[first], "first heap not reported sparse");

        const u64 heap_bytes = allocator.GetStats().heap_bytes;
        std::vector<bool> avoid_both(std::max(first, next) + 1);
        avoid_both[first] = avoid_both[next] = true;
        Require(!allocator.CreateInUsedHeaps(desc, D3D12_RESOURCE_STATE_COPY_DEST, false,
                                             avoid_both).resource,
                "placed with every heap in use avoided");
        Require(allocator.GetStats().heap_bytes == heap_bytes, "the pool grew for a move");

        auto moved = allocator.CreateInUsedHeaps(desc, D3D12_RESOURCE_STATE_COPY_DEST, false,
                                                 Avoid(first));
        Require(moved.resource && moved.placed, "no room found in the second heap");
        Require(TextureResourceAllocator::HeapOf(*moved.allocation) == next,
                "moved into heap " + std::to_string(TextureResourceAllocator::HeapOf(
                                         *moved.allocation)));
        allocator.DeferRelease(std::move(survivor));
        scheduler.Finish();
        scheduler.CollectGarbage();
        allocator.TrimEmptyHeaps(TextureResourceAllocator::TrimMode::All);
        Require(allocator.GetStats().heap_bytes + 64_MiB <= heap_bytes,
                "the emptied heap was not released (" +
                    std::to_string(allocator.GetStats().heap_bytes >> 20) + " of " +
                    std::to_string(heap_bytes >> 20) + " MiB)");
        allocator.DeferRelease(std::move(moved));
        allocator.DeferRelease(std::move(in_next));
        scheduler.Finish();
        scheduler.CollectGarbage();
    }

    StagingBufferPool staging{device, scheduler};
    TransferBufferPool transfers{device, scheduler};
    CpuDescriptorAllocator views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    CpuDescriptorAllocator samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    CpuDescriptorAllocator dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
    TextureCacheRuntime runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs};

    // Sampled BC1, 2 MiB each: 32 to a texture heap.
    ImageInfo bc;
    bc.format = PixelFormat::BC1_RGBA_UNORM;
    bc.type = ImageType::e2D;
    bc.size = {2048, 2048, 1};
    bc.resources = {1, 1};
    auto textures = Fill(runtime, bc, 80, true);
    Image& texture = *textures.images[2];
    for (size_t i = 0; i + 1 < textures.images.size(); ++i) {
        if (i != 2) textures.images[i].reset();
    }
    MoveAndCheck(runtime, scheduler, texture, bc, textures.first_heap, textures.next_heap, 3,
                 "BC1 texture", true);

    // Render targets and depth share the RT/DS heaps: 4 MiB each, 8 to a 32 MiB heap. The
    // depth image goes first so it lands in the first heap.
    ImageInfo depth_info;
    depth_info.format = PixelFormat::D32_FLOAT;
    depth_info.type = ImageType::e2D;
    depth_info.size = {1024, 1024, 1};
    depth_info.resources = {1, 1};
    auto depth = std::make_unique<Image>(runtime, depth_info, 0, 0);
    const size_t depth_heap = depth->HeapIndex();
    ImageInfo color;
    color.format = PixelFormat::A8B8G8R8_UNORM;
    color.type = ImageType::e2D;
    color.size = {1024, 1024, 1};
    color.resources = {1, 1};
    auto targets = Fill(runtime, color, 40, false);
    Require(targets.first_heap == depth_heap, "depth and color not in the same RT/DS heap");
    Image& target = *targets.images[0];
    for (size_t i = 1; i + 1 < targets.images.size(); ++i) targets.images[i].reset();
    MoveAndCheck(runtime, scheduler, target, color, targets.first_heap, targets.next_heap, 9,
                 "render target", true);
    // Depth goes through the depth-stencil upload; only the move and the debug layer count.
    MoveAndCheck(runtime, scheduler, *depth, depth_info, depth_heap, targets.next_heap, 0,
                 "depth image", false);

    scheduler.Finish();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "device removed");
    Require(!DebugErrors(messages.Get()), "D3D12 debug layer errors");
    const auto stats = GetHeapCompactionStats();
    std::cout << "PASS heap compaction: allocator moves only into heaps in use (none with all "
                 "avoided, pool did not grow), the emptied 64 MiB heap released by trim; "
              << stats.images << " images moved (" << (stats.bytes >> 20)
              << " MiB): BC1 and render target read back identical, depth moved; debug layer "
                 "clean\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "heap compaction gate: " << e.what() << '\n';
    return 1;
}
