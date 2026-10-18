// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// CopyTextureRegion copies a depth-stencil subresource only whole. Partial copies to and from
// single-plane depth images go through a buffer; this gate checks the bytes and that the debug
// layer sees no invalid copy.
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
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

void Check(bool condition, const std::string& what) {
    if (!condition) {
        throw std::runtime_error(what);
    }
}

struct Texture {
    u32 width;
    u32 height;
    std::vector<u32> texels;
};

BufferImageCopy WholeLevel(u32 width, u32 height) {
    return {.buffer_offset = 0,
            .buffer_size = size_t{width} * height * 4,
            .buffer_row_length = width,
            .buffer_image_height = height,
            .image_subresource = {0, 0, 1},
            .image_offset = {},
            .image_extent = {width, height, 1}};
}

/// Finite floats in [0, 1): valid as depth and as any 32-bit color.
Texture Pattern(u32 width, u32 height, u32 seed) {
    Texture texture{width, height, std::vector<u32>(size_t{width} * height)};
    for (size_t i = 0; i < texture.texels.size(); ++i) {
        const float value = static_cast<float>((i * 37 + seed * 101) % 997) / 997.0f;
        std::memcpy(&texture.texels[i], &value, 4);
    }
    return texture;
}

class Gate {
public:
    Gate()
        : scheduler{device}, staging{device, scheduler}, transfers{device, scheduler},
          views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
          samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER},
          rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV},
          dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV},
          runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs} {
        ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
        messages->ClearStoredMessages();
    }

    Image Make(PixelFormat format, const Texture& contents) {
        ImageInfo info;
        info.format = format;
        info.type = ImageType::e2D;
        info.size = {contents.width, contents.height, 1};
        info.resources = {1, 1};
        Image image{runtime, info, 0, 0};
        const BufferImageCopy layout = WholeLevel(contents.width, contents.height);
        auto upload = runtime.UploadStagingBuffer(layout.buffer_size);
        std::memcpy(upload.mapped_span.data(), contents.texels.data(), layout.buffer_size);
        image.UploadMemory(upload, std::span{&layout, 1});
        return image;
    }

    /// Copies, reads the destination back and compares it with the copy done on the CPU.
    void Copy(const char* name, Image& dst, Texture& dst_contents, Image& src,
              const Texture& src_contents, const ImageCopy& copy) {
        runtime.CopyImage(dst, src, std::span{&copy, 1});
        for (u32 y = 0; y < copy.extent.height; ++y) {
            for (u32 x = 0; x < copy.extent.width; ++x) {
                dst_contents.texels[size_t{copy.dst_offset.y + y} * dst_contents.width +
                                    copy.dst_offset.x + x] =
                    src_contents.texels[size_t{copy.src_offset.y + y} * src_contents.width +
                                        copy.src_offset.x + x];
            }
        }
        const BufferImageCopy layout = WholeLevel(dst_contents.width, dst_contents.height);
        auto readback = runtime.DownloadStagingBuffer(layout.buffer_size);
        dst.DownloadMemory(readback, std::span{&layout, 1});
        scheduler.Finish();
        Check(std::memcmp(readback.mapped_span.data(), dst_contents.texels.data(),
                          layout.buffer_size) == 0,
              std::string{name} + ": wrong texels");
        CheckDebugLayer(name);
        std::cerr << "  " << name << ": ok\n";
    }

    void CheckDebugLayer(const char* name) {
        bool clean = true;
        for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{};
            messages->GetMessage(i, nullptr, &size);
            std::vector<u8> storage(size);
            auto* const message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            messages->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << message->pDescription << '\n';
                clean = false;
            }
        }
        messages->ClearStoredMessages();
        Check(clean, std::string{name} + ": debug layer error");
    }

    Device device;
    Scheduler scheduler;
    StagingBufferPool staging;
    TransferBufferPool transfers;
    CpuDescriptorAllocator views;
    CpuDescriptorAllocator samplers;
    CpuDescriptorAllocator rtvs;
    CpuDescriptorAllocator dsvs;
    TextureCacheRuntime runtime;
    ComPtr<ID3D12InfoQueue> messages;
};

ImageCopy Region(Offset3D src, Offset3D dst, u32 width, u32 height) {
    return {.src_subresource = {0, 0, 1},
            .dst_subresource = {0, 0, 1},
            .src_offset = src,
            .dst_offset = dst,
            .extent = {width, height, 1}};
}

} // namespace

int main() try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    Gate gate;
    std::cerr << "Depth region copy gate\n";

    // The logged case: the 2x2 corner of a 16x2 depth image into a 2x2 one. Whole destination,
    // region at the start of the buffer: two copies and a barrier.
    {
        Texture src = Pattern(16, 2, 1), dst = Pattern(2, 2, 2);
        Image src_image = gate.Make(PixelFormat::D32_FLOAT, src);
        Image dst_image = gate.Make(PixelFormat::D32_FLOAT, dst);
        gate.Copy("depth corner into whole depth", dst_image, dst, src_image, src,
                  Region({}, {}, 2, 2));
        // Region inside the row: the rows still line up, one buffer copy moves them.
        gate.Copy("depth middle into whole depth", dst_image, dst, src_image, src,
                  Region({5, 0, 0}, {}, 2, 2));
    }
    // Into part of a depth image: the texels around the region have to survive.
    {
        Texture src = Pattern(5, 3, 3), dst = Pattern(8, 8, 4);
        Image src_image = gate.Make(PixelFormat::D32_FLOAT, src);
        Image dst_image = gate.Make(PixelFormat::D32_FLOAT, dst);
        gate.Copy("depth into part of depth", dst_image, dst, src_image, src,
                  Region({1, 1, 0}, {3, 5, 0}, 3, 2));
        gate.Copy("depth row into depth edge", dst_image, dst, src_image, src,
                  Region({0, 2, 0}, {3, 7, 0}, 5, 1));
    }
    // Color into part of depth, and part of depth into color.
    {
        Texture color = Pattern(9, 4, 5), depth = Pattern(6, 6, 6);
        Image color_image = gate.Make(PixelFormat::R32_FLOAT, color);
        Image depth_image = gate.Make(PixelFormat::D32_FLOAT, depth);
        gate.Copy("color into part of depth", depth_image, depth, color_image, color,
                  Region({2, 1, 0}, {1, 2, 0}, 4, 3));
        gate.Copy("part of depth into color", color_image, color, depth_image, depth,
                  Region({3, 1, 0}, {5, 0, 0}, 3, 4));
    }
    // Wide rows: the region sits at a 512-byte boundary of the staged buffer.
    {
        Texture src = Pattern(256, 4, 7), dst = Pattern(64, 2, 8);
        Image src_image = gate.Make(PixelFormat::D32_FLOAT, src);
        Image dst_image = gate.Make(PixelFormat::D32_FLOAT, dst);
        gate.Copy("aligned depth region into whole depth", dst_image, dst, src_image, src,
                  Region({128, 0, 0}, {}, 64, 2));
        gate.Copy("unaligned depth region into whole depth", dst_image, dst, src_image, src,
                  Region({37, 2, 0}, {}, 64, 2));
    }
    // Whole subresources keep the direct copy.
    {
        Texture src = Pattern(7, 5, 9), dst = Pattern(7, 5, 10);
        Image src_image = gate.Make(PixelFormat::D32_FLOAT, src);
        Image dst_image = gate.Make(PixelFormat::D32_FLOAT, dst);
        gate.Copy("whole depth into whole depth", dst_image, dst, src_image, src,
                  Region({}, {}, 7, 5));
    }
    // Color reinterpretation between format families at unaligned offsets.
    {
        Texture src = Pattern(7, 6, 11), dst = Pattern(10, 9, 12);
        Image src_image = gate.Make(PixelFormat::R32_UINT, src);
        Image dst_image = gate.Make(PixelFormat::B10G11R11_FLOAT, dst);
        gate.Copy("uint into part of packed float", dst_image, dst, src_image, src,
                  Region({1, 2, 0}, {3, 5, 0}, 5, 3));
        gate.Copy("packed float into part of uint", src_image, src, dst_image, dst,
                  Region({4, 1, 0}, {0, 3, 0}, 3, 3));
    }
    ThrowIfFailed(gate.device.Get()->GetDeviceRemovedReason(), "test device removed");
    std::cout << "PASS: partial depth copies (whole/partial destination, aligned and unaligned "
                 "regions, color<->depth), whole depth copies and color reinterpretation; "
                 "D3D12 debug clean\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Depth region copy gate failed: " << error.what() << '\n';
    return 1;
}
