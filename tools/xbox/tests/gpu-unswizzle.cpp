// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// GPU unswizzle gate: big block-linear color images uploaded through the compute shader must hold
// the same texels as the CPU UnswizzleImage path, for every level and layer.
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/texture_cache/util.h"
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
using namespace D3D12;
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;
int main() try {
    Common::Log::Initialize(); Settings::values.renderer_debug = true;
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device}; StagingBufferPool staging{device, scheduler};
    TransferBufferPool transfers{device, scheduler};
    CpuDescriptorAllocator views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    CpuDescriptorAllocator samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    CpuDescriptorAllocator dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
    TextureCacheRuntime runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs};
    ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("spirv_to_dxil not available");
    DescriptorRing ring{device.Get(), scheduler, 4096};
    SamplerHeap sampler_heap{device.Get(), scheduler};
    BlitImageHelper helper{device, scheduler, compiler, ring, sampler_heap, samplers};
    if (!helper.CanUnswizzle()) throw std::runtime_error("unswizzle shader not ready");
    runtime.SetBlitHelper(&helper);
    // The block-linear CPU path never touches the memory manager (only pitch-linear images read
    // guest memory through it).
    u64 no_memory_manager{};
    auto& gpu_memory = *reinterpret_cast<Tegra::MemoryManager*>(&no_memory_manager);

    struct Case { u32 width, height, levels, layers, block_height, tile_width_spacing; };
    constexpr Case cases[]{
        {1280, 720, 1, 1, 4, 0}, {1000, 600, 5, 1, 3, 0}, {512, 512, 10, 3, 4, 0},
        {300, 2000, 4, 2, 5, 0}, {4096, 64, 3, 1, 0, 0}, {257, 257, 9, 1, 2, 0},
        {4096, 1024, 2, 1, 4, 0}, {640, 480, 3, 2, 4, 1}, {1920, 1080, 11, 1, 4, 0}};
    constexpr PixelFormat formats[]{
        PixelFormat::R8_UNORM, PixelFormat::R16_UNORM, PixelFormat::A8B8G8R8_UNORM,
        PixelFormat::R16G16B16A16_FLOAT, PixelFormat::R32G32B32A32_FLOAT,
        PixelFormat::BC1_RGBA_UNORM, PixelFormat::BC7_UNORM};
    std::mt19937 random{1234};
    u32 images = 0, compared_bytes = 0, skipped = 0;
    auto make_info = [](PixelFormat format, const Case& c) {
        ImageInfo info; info.type = ImageType::e2D; info.format = format;
        info.size = {c.width, c.height, 1};
        info.resources = {static_cast<s32>(c.levels), static_cast<s32>(c.layers)};
        info.block = {0, c.block_height, 0}; info.tile_width_spacing = c.tile_width_spacing;
        info.layer_stride = CalculateLayerStride(info);
        info.maybe_unaligned_layer_stride = CalculateLayerSize(info);
        return info;
    };
    for (const PixelFormat format : formats) {
        for (const Case& c : cases) {
            const ImageInfo info = make_info(format, c);
            const std::string name = std::to_string(static_cast<u32>(format)) + " " +
                std::to_string(c.width) + "x" + std::to_string(c.height) + " levels=" +
                std::to_string(c.levels) + " layers=" + std::to_string(c.layers) + " bh=" +
                std::to_string(c.block_height) + " tws=" + std::to_string(c.tile_width_spacing);
            SetGpuUnswizzle(true);
            Image gpu_image{runtime, info, 0, 0};
            SetGpuUnswizzle(false);
            Image cpu_image{runtime, info, 0, 0};
            SetGpuUnswizzle(true);
            if (cpu_image.IsGpuUnswizzled()) throw std::runtime_error("gpu_unswizzle=0 ignored");
            if (!gpu_image.IsGpuUnswizzled()) {
                const bool compressed = VideoCore::Surface::DefaultBlockWidth(format) > 1;
                if (!compressed) throw std::runtime_error("not GPU unswizzled: " + name);
                ++skipped; // NPOT block-compressed images keep their own path
                continue;
            }
            if (!runtime.CanAccelerateImageUpload(gpu_image))
                throw std::runtime_error("CanAccelerateImageUpload false: " + name);
            const u32 guest_size = gpu_image.guest_size_bytes;
            std::vector<u8> guest(guest_size);
            for (u8& byte : guest) byte = static_cast<u8>(random());

            // CPU path: UnswizzleImage into tightly packed copies, then a plain upload.
            std::vector<u8> unswizzled(CalculateUnswizzledSizeBytes(info));
            auto copies = UnswizzleImage(gpu_memory, 0, info, guest, unswizzled);
            auto cpu_upload = runtime.UploadStagingBuffer(unswizzled.size());
            std::memcpy(cpu_upload.mapped_span.data(), unswizzled.data(), unswizzled.size());
            cpu_image.UploadMemory(cpu_upload, copies);

            // GPU path: the guest bytes as they are, unswizzled by the compute shader.
            auto gpu_upload = runtime.UploadStagingBuffer(guest_size);
            std::memcpy(gpu_upload.mapped_span.data(), guest.data(), guest_size);
            const auto swizzles = FullUploadSwizzles(info);
            runtime.AccelerateImageUpload(gpu_image, gpu_upload, swizzles, 0, 0);

            auto gpu_download = runtime.DownloadStagingBuffer(unswizzled.size());
            auto cpu_download = runtime.DownloadStagingBuffer(unswizzled.size());
            gpu_image.DownloadMemory(gpu_download, copies);
            cpu_image.DownloadMemory(cpu_download, copies);
            scheduler.Finish();
            const u8* const gpu_bytes = gpu_download.mapped_span.data();
            const u8* const cpu_bytes = cpu_download.mapped_span.data();
            if (std::memcmp(cpu_bytes, unswizzled.data(), unswizzled.size()) != 0)
                throw std::runtime_error("CPU reference round trip changed: " + name);
            if (std::memcmp(gpu_bytes, unswizzled.data(), unswizzled.size()) != 0) {
                size_t first = 0;
                while (gpu_bytes[first] == unswizzled[first]) ++first;
                s32 level = 0;
                for (const BufferImageCopy& copy : copies)
                    if (copy.buffer_offset <= first) level = copy.image_subresource.base_level;
                throw std::runtime_error("GPU unswizzle mismatch: " + name + " byte=" +
                                         std::to_string(first) + " level=" + std::to_string(level));
            }
            compared_bytes += static_cast<u32>(unswizzled.size());
            ++images;
            std::cout << "ok " << name << '\n';
        }
    }

    // Small images keep the CPU path.
    const ImageInfo small_info = make_info(PixelFormat::A8B8G8R8_UNORM, {64, 64, 1, 1, 4, 0});
    Image small_image{runtime, small_info, 0, 0};
    if (small_image.IsGpuUnswizzled()) throw std::runtime_error("small image GPU unswizzled");
    runtime.SetBlitHelper(nullptr);
    scheduler.Finish();
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; messages->GetMessage(i, nullptr, &size); std::vector<u8> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) { std::cerr << message->pDescription << '\n'; return 1; }
    }
    std::cout << "PASS GPU unswizzle: " << images << " images, " << compared_bytes / 1024
              << " KiB compared bit-exact against the CPU path, " << skipped
              << " block-compressed cases on their own path; small images stay on CPU; debug clean\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
