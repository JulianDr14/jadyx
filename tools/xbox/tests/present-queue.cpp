// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Present queue gate: on WARP with the debug layer, a device made with SetPresentQueue has a
// second direct queue, and the presentation's synchronization holds across the two queues. Frames
// rendered on the scheduler's queue are copied on the present queue by a thread of its own (the
// copy waits for the scheduler's fence on the GPU), and a frame is rendered into again only once
// its copy fence passed; every copy must hold its own frame's color, never the next one.
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <d3d12sdklayers.h>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace D3D12;

namespace {
constexpr u32 FRAMES = 3;
constexpr u32 ITERATIONS = 240;
constexpr u32 FRAME_SIZE = 1024;      ///< frame width and height: big clears keep the GPU busy
constexpr u32 COPIED = 64;      ///< corner copied to the readback buffer
constexpr u64 SLOT = COPIED * COPIED * 4; ///< rows of 256 bytes, already pitch aligned

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

struct Frame {
    ComPtr<ID3D12Resource> image;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    u64 render_tick{};
    u64 copy_value{};
    u32 iteration{};
};

u8 Red(u32 iteration) {
    return u8(iteration * 37 + 11);
}
} // namespace

int main() try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    _putenv("JADYX_D3D12_WARP=1");
    SetPresentQueue(true);
    Device device;
    Require(device.HasPresentQueue(), "no present queue of its own");
    Require(device.PresentQueue() != device.Queue(), "present queue is the emulation queue");
    Require(&device.PresentQueueMutex() != &device.QueueMutex(), "present queue shares the lock");
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    ID3D12Device* const dev = device.Get();

    std::vector<Frame> frames(FRAMES);
    const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = FRAME_SIZE, .Height = FRAME_SIZE,
        .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    for (Frame& frame : frames) {
        ThrowIfFailed(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                   IID_PPV_ARGS(&frame.image)),
                      "frame");
        frame.rtv = rtvs.Allocate();
        dev->CreateRenderTargetView(frame.image.Get(), nullptr, frame.rtv);
    }
    const D3D12_HEAP_PROPERTIES readback_heap{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC buffer{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = SLOT * ITERATIONS, .Height = 1,
        .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {.Count = 1},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(dev->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback)),
                  "readback");

    // The present side: per-frame allocators, one list, a copy fence (like PresentManager).
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators(FRAMES);
    std::vector<u64> allocator_values(FRAMES);
    for (auto& allocator : allocators) {
        ThrowIfFailed(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator)),
                      "allocator");
    }
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(),
                                         nullptr, IID_PPV_ARGS(&copy_list)),
                  "copy list");
    ThrowIfFailed(copy_list->Close(), "close");
    ComPtr<ID3D12Fence> copy_fence;
    ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence)), "fence");
    u64 copy_fence_value = 0;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Frame*> queued;
    std::deque<Frame*> free_frames;
    for (Frame& frame : frames) free_frames.push_back(&frame);
    bool done = false;
    std::string present_error;

    std::thread presenter([&] {
        try {
            while (true) {
                Frame* frame{};
                {
                    std::unique_lock lock{mutex};
                    cv.wait(lock, [&] { return done || !queued.empty(); });
                    if (queued.empty()) return;
                    frame = queued.front();
                    queued.pop_front();
                }
                const size_t index = static_cast<size_t>(frame - frames.data());
                if (copy_fence->GetCompletedValue() < allocator_values[index]) {
                    ThrowIfFailed(copy_fence->SetEventOnCompletion(allocator_values[index],
                                                                   nullptr),
                                  "allocator wait");
                }
                ThrowIfFailed(allocators[index]->Reset(), "allocator reset");
                ThrowIfFailed(copy_list->Reset(allocators[index].Get(), nullptr), "list reset");
                const D3D12_TEXTURE_COPY_LOCATION dst{
                    .pResource = readback.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    .PlacedFootprint = {.Offset = SLOT * frame->iteration,
                                        .Footprint = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                                      .Width = COPIED, .Height = COPIED,
                                                      .Depth = 1, .RowPitch = COPIED * 4}}};
                const D3D12_TEXTURE_COPY_LOCATION src{
                    .pResource = frame->image.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = 0};
                const D3D12_BOX box{0, 0, 0, COPIED, COPIED, 1};
                copy_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                ThrowIfFailed(copy_list->Close(), "list close");
                scheduler.WaitSubmitted(frame->render_tick);
                std::scoped_lock queue_lock{device.PresentQueueMutex()};
                ID3D12CommandQueue* const queue = device.PresentQueue();
                ThrowIfFailed(queue->Wait(scheduler.Fence(), frame->render_tick), "queue wait");
                ID3D12CommandList* const lists[] = {copy_list.Get()};
                queue->ExecuteCommandLists(1, lists);
                ThrowIfFailed(queue->Signal(copy_fence.Get(), ++copy_fence_value), "signal");
                allocator_values[index] = copy_fence_value;
                frame->copy_value = copy_fence_value;
                {
                    std::scoped_lock lock{mutex};
                    free_frames.push_back(frame);
                }
                cv.notify_all();
            }
        } catch (const std::exception& e) {
            std::scoped_lock lock{mutex};
            present_error = e.what();
            done = true;
            cv.notify_all();
        }
    });

    for (u32 i = 0; i < ITERATIONS; ++i) {
        Frame* frame{};
        {
            std::unique_lock lock{mutex};
            cv.wait(lock, [&] { return !free_frames.empty() || !present_error.empty(); });
            Require(present_error.empty(), "present thread: " + present_error);
            frame = free_frames.front();
            free_frames.pop_front();
        }
        // AcquireFrame with a present queue of its own.
        scheduler.Wait(frame->render_tick);
        if (copy_fence->GetCompletedValue() < frame->copy_value) {
            ThrowIfFailed(copy_fence->SetEventOnCompletion(frame->copy_value, nullptr),
                          "frame reuse wait");
        }
        frame->iteration = i;
        auto* const cmd = scheduler.CommandList();
        TransitionResource(cmd, frame->image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Several full clears first, so the frame's GPU work lasts while the copy is queued.
        for (u32 pass = 0; pass < 6; ++pass) {
            const FLOAT busy[4]{0.0f, float(pass) / 8.0f, 0.5f, 1.0f};
            cmd->ClearRenderTargetView(frame->rtv, busy, 0, nullptr);
        }
        const FLOAT color[4]{float(Red(i)) / 255.0f, 0.25f, 0.75f, 1.0f};
        cmd->ClearRenderTargetView(frame->rtv, color, 0, nullptr);
        TransitionResource(cmd, frame->image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        frame->render_tick = scheduler.Flush();
        {
            std::scoped_lock lock{mutex};
            queued.push_back(frame);
        }
        cv.notify_all();
    }
    {
        std::scoped_lock lock{mutex};
        done = true;
    }
    cv.notify_all();
    presenter.join();
    Require(present_error.empty(), "present thread: " + present_error);
    if (copy_fence->GetCompletedValue() < copy_fence_value) {
        ThrowIfFailed(copy_fence->SetEventOnCompletion(copy_fence_value, nullptr), "final wait");
    }
    scheduler.Finish();

    u8* mapped{};
    const D3D12_RANGE range{0, SLOT * ITERATIONS};
    ThrowIfFailed(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map");
    u32 wrong = 0;
    for (u32 i = 0; i < ITERATIONS; ++i) {
        const u8* const slot = mapped + SLOT * i;
        for (u64 texel = 0; texel < COPIED * COPIED; ++texel) {
            if (slot[texel * 4] != Red(i) || slot[texel * 4 + 2] != 191) {
                if (wrong++ < 4) {
                    std::cerr << "frame " << i << " texel " << texel << ": red "
                              << int(slot[texel * 4]) << ", expected " << int(Red(i)) << '\n';
                }
                break;
            }
        }
    }
    readback->Unmap(0, nullptr);
    Require(wrong == 0, std::to_string(wrong) + " copies held the wrong frame");
    ThrowIfFailed(dev->GetDeviceRemovedReason(), "device removed");
    Require(!DebugErrors(messages.Get()), "D3D12 debug layer errors");
    for (Frame& frame : frames) rtvs.Free(frame.rtv);
    std::cout << "PASS present queue: a queue of its own with its own lock; " << ITERATIONS
              << " frames rendered on the emulation queue and copied on the present queue from "
                 "another thread, each copy holds its own frame; debug layer clean\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "present queue gate: " << e.what() << '\n';
    return 1;
}
