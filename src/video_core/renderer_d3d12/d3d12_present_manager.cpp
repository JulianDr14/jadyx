// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/thread.h"
#include "video_core/frame_pipeline.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_present_manager.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

namespace {

/// Overlay-only frames are left out of frame generation for this long after the game's last
/// frame, seconds; past it the game stopped presenting and the overlay is all there is to show.
constexpr double OVERLAY_ONLY_WINDOW = 0.25;

/// Host time in seconds, the clock frame generation paces on.
double HostSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void PresentFrame::Transition(CommandStream* cmd, D3D12_RESOURCE_STATES next) {
    if (state != next) {
        TransitionResource(cmd, image.Get(), state, next);
        state = next;
    }
}

FrameLease::~FrameLease() {
    if (manager && frame) {
        manager->Release(*frame);
    }
}

FrameLease::FrameLease(FrameLease&& other) noexcept
    : manager{std::exchange(other.manager, nullptr)}, frame{std::exchange(other.frame, nullptr)} {}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) {
        if (manager && frame) {
            manager->Release(*frame);
        }
        manager = std::exchange(other.manager, nullptr);
        frame = std::exchange(other.frame, nullptr);
    }
    return *this;
}

PresentManager::PresentManager(const Device& device_, Scheduler& scheduler_,
                               Swapchain& swapchain_, CpuDescriptorAllocator& rtv_descriptors_,
                               const ShaderCompiler& shader_compiler_,
                               DescriptorRing& descriptor_ring_, SamplerHeap& sampler_heap_,
                               bool async_presentation)
    : device{device_}, scheduler{scheduler_}, swapchain{swapchain_},
      rtv_descriptors{rtv_descriptors_}, shader_compiler{shader_compiler_},
      descriptor_ring{descriptor_ring_}, sampler_heap{sampler_heap_} {
    ID3D12Device* const dev = device.Get();
    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        PresentFrame& frame = frames[i];
        frame.index = i;
        frame.image = CreateFrameImage(i, swapchain.Width(), swapchain.Height());
        frame.rtv = rtv_descriptors.Allocate();
        dev->CreateRenderTargetView(frame.image.Get(), nullptr, frame.rtv);
        ThrowIfFailed(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&copy_contexts[i].allocator)),
                      "CreateCommandAllocator (present)");
        free_frames.push_back(&frame);
    }
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         copy_contexts[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&copy_list)),
                  "CreateCommandList (present)");
    ThrowIfFailed(copy_list->Close(), "ID3D12GraphicsCommandList::Close (present)");
    ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence)),
                  "CreateFence (present)");
    copy_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (copy_event == nullptr) {
        throw std::runtime_error("D3D12: CreateEventExW failed (present)");
    }

    // Present blocks without a waitable swapchain, and it would block holding the queue lock.
    const bool use_thread = async_presentation && swapchain.HasFrameLatencyWaitable();
    if (use_thread) {
        // Generated frames are shown by the present thread, between the game's.
        CreateFrameGen();
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
    LOG_INFO(Render, "D3D12: presentation {} ({} frames of {}x{})",
             use_thread ? "on its own thread"
                        : (async_presentation ? "on the GPU thread (no waitable swapchain)"
                                              : "on the GPU thread"),
             FRAME_COUNT, swapchain.Width(), swapchain.Height());
}

PresentManager::~PresentManager() {
    try {
        WaitIdle();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: present shutdown: {}", e.what());
    }
    if (present_thread.joinable()) {
        present_thread.request_stop();
        present_thread.join();
    }
    // The last copies may still read the frames on the GPU.
    try {
        WaitForCopy(copy_fence_value);
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: present shutdown: {}", e.what());
    }
    for (PresentFrame& frame : frames) {
        if (frame.rtv.ptr != 0) {
            rtv_descriptors.Free(frame.rtv);
        }
    }
    if (copy_event != nullptr) {
        CloseHandle(copy_event);
    }
}

ComPtr<ID3D12Resource> PresentManager::CreateFrameImage(u32 index, u32 width, u32 height) const {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = Swapchain::FORMAT,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    const D3D12_CLEAR_VALUE clear{.Format = Swapchain::FORMAT, .Color = {0.0f, 0.0f, 0.0f, 1.0f}};
    ComPtr<ID3D12Resource> image;
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE, &clear,
                                                        IID_PPV_ARGS(&image)),
                  "CreateCommittedResource (present frame)");
    image->SetName((L"Present frame " + std::to_wstring(index)).c_str());
    return image;
}

void PresentManager::CreateFrameGen() {
    frame_gen = std::make_unique<FrameGen>(device, scheduler, shader_compiler, descriptor_ring,
                                           sampler_heap, copy_fence.Get(), swapchain.Width(),
                                           swapchain.Height());
}

bool PresentManager::Resize(u32 width, u32 height) {
    WaitIdle();
    {
        std::scoped_lock lock{free_mutex};
        if (free_frames.size() != FRAME_COUNT) {
            LOG_ERROR(Render, "D3D12: output resize with a frame still lent");
            return false;
        }
    }
    const bool was_async = IsAsync();
    if (was_async) {
        {
            std::scoped_lock lock{queue_mutex};
            fg_active = false;
        }
        present_thread.request_stop();
        present_thread.join();
    }
    bool resized = false;
    try {
        scheduler.Finish();
        WaitForCopy(copy_fence_value);
        // The new frames first: without memory for them nothing has changed yet.
        std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> images;
        for (u32 i = 0; i < FRAME_COUNT; ++i) {
            images[i] = CreateFrameImage(i, width, height);
        }
        // Its history is the size of the output.
        frame_gen.reset();
        fg_running = false;
        {
            std::scoped_lock lock{device.PresentQueueMutex()};
            resized = swapchain.Resize(width, height);
        }
        if (resized) {
            for (u32 i = 0; i < FRAME_COUNT; ++i) {
                PresentFrame& frame = frames[i];
                frame.image = std::move(images[i]);
                frame.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                frame.render_tick = 0;
                frame.copy_value = 0;
                device.Get()->CreateRenderTargetView(frame.image.Get(), nullptr, frame.rtv);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: output resize to {}x{}: {}", width, height, e.what());
    }
    if (was_async) {
        try {
            CreateFrameGen();
        } catch (const std::exception& e) {
            LOG_ERROR(Render, "D3D12: frame generation after the resize: {}", e.what());
        }
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
    return resized;
}

FrameLease PresentManager::AcquireFrame() {
    using VideoCore::FramePipeline::Mode;
    using VideoCore::Perf::Counter;
    // Without the present thread nothing stays queued: there is no frame to reuse.
    const Mode mailbox = IsAsync() ? VideoCore::FramePipeline::PresentMailbox() : Mode::Off;
    const auto start = std::chrono::steady_clock::now();
    PresentFrame* frame{};
    if (mailbox == Mode::On) {
        frame = TakeFreeFrame();
        if (frame == nullptr) {
            frame = ReuseQueuedFrame();
        }
    } else if (mailbox == Mode::Check && !HasFreeFrame()) {
        std::scoped_lock lock{queue_mutex};
        if (!queued_frames.empty()) {
            VideoCore::Perf::Add(Counter::PresentsWouldReplace, 1);
        }
    }
    if (frame == nullptr) {
        // Every frame is queued or being copied (the mailbox only waits for the latter).
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_frames.empty(); });
        frame = free_frames.front();
        free_frames.pop_front();
    }
    const auto free_end = std::chrono::steady_clock::now();
    if (mailbox == Mode::On) {
        // The queue already orders new commands after the frame's previous rendering: this only
        // keeps the CPU from running more than RENDER_AHEAD composites ahead of the GPU.
        scheduler.Wait(composite_ticks[composite_tick_index]);
    } else {
        // The queue already orders new commands after the frame's previous rendering and copy;
        // this only keeps the CPU from running more than FRAME_COUNT frames ahead of the GPU.
        scheduler.Wait(frame->render_tick);
    }
    const auto render_end = std::chrono::steady_clock::now();
    // The copy then runs outside the emulation queue's order: rendering into the frame again
    // must wait until the copy has read it. A frame comes back FRAME_COUNT frames later, so this
    // practically never blocks.
    if (device.HasPresentQueue() && copy_fence->GetCompletedValue() < frame->copy_value) {
        if (mailbox == Mode::On) {
            // On the GPU: the submission rendering into the frame comes after this call.
            scheduler.QueueWaitBeforeNextSubmit(copy_fence.Get(), frame->copy_value);
        } else {
            // No event: the present thread owns copy_event.
            ThrowIfFailed(copy_fence->SetEventOnCompletion(frame->copy_value, nullptr),
                          "ID3D12Fence::SetEventOnCompletion (frame reuse)");
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const auto us = [](auto from, auto to) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
    };
    VideoCore::Perf::Add(Counter::PresentFreeWaitUs, us(start, free_end));
    VideoCore::Perf::Add(Counter::PresentRenderWaitUs, us(free_end, render_end));
    VideoCore::Perf::Add(Counter::PresentCopyWaitUs, us(render_end, end));
    if (us(start, end) > 1000) {
        VideoCore::Perf::Add(Counter::PresentWaitsLong, 1);
    }
    return FrameLease{*this, *frame};
}

bool PresentManager::HasFreeFrame() {
    std::scoped_lock lock{free_mutex};
    return !free_frames.empty();
}

PresentFrame* PresentManager::TakeFreeFrame() {
    std::scoped_lock lock{free_mutex};
    if (free_frames.empty()) {
        return nullptr;
    }
    PresentFrame* const frame = free_frames.front();
    free_frames.pop_front();
    return frame;
}

PresentFrame* PresentManager::ReuseQueuedFrame() {
    PresentFrame* frame{};
    {
        // The oldest: a newer frame stays queued to be shown in its place. A frame the present
        // thread took is no longer in the queue, so one being copied is never reused.
        std::scoped_lock lock{queue_mutex};
        if (queued_frames.empty()) {
            return nullptr;
        }
        frame = queued_frames.front();
        queued_frames.pop_front();
        --frames_in_flight;
    }
    idle_cv.notify_all();
    VideoCore::Perf::Add(VideoCore::Perf::Counter::PresentsReplaced, 1);
    return frame;
}

void PresentManager::Present(FrameLease&& lease, const PresentInfo& info) {
    PresentFrame& frame = *lease.frame;
    lease.manager = nullptr; // the frame comes back through Release once presented
    lease.frame = nullptr;
    u32 rate = 0;
    const double now = HostSeconds();
    // A composition with only the overlay new (it runs at its own rate) is no frame of the game:
    // frame generation would take its vsync as the game's cadence and blend two equal images.
    // It is left out while the game presents; the overlay shows with the game's next frame.
    const bool overlay_only = fg_running && !info.new_frame &&
                              now - last_new_frame < OVERLAY_ONLY_WINDOW;
    if (info.new_frame) {
        last_new_frame = now;
    }
    try {
        frame.Transition(scheduler.CommandList(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (frame_gen && !overlay_only) {
            rate = frame_gen->OnFrame(info.vsync_index, info.frame_gen, now, info.queue_time);
        }
        if (overlay_only) {
            frame.render_tick = scheduler.Flush();
        } else if (rate != 0) {
            // Into the history on the scheduler's list; the present thread shows it in time.
            const std::optional<u32> slot = frame_gen->RecordIngest(frame.image.Get());
            frame.render_tick = scheduler.Flush();
            if (slot) {
                frame_gen->Commit(*slot, frame.render_tick, info.vsync_index);
            }
        } else {
            if (fg_running) {
                frame_gen->Break();
                fg_running = false;
            }
            frame.render_tick = scheduler.Flush();
        }
    } catch (...) {
        Release(frame);
        throw;
    }
    composite_ticks[composite_tick_index] = frame.render_tick;
    composite_tick_index = (composite_tick_index + 1) % RENDER_AHEAD;
    if (overlay_only) {
        Release(frame);
        return;
    }
    if (rate != 0) {
        fg_running = true;
        // The history holds a copy: the frame can be rendered into again.
        Release(frame);
        {
            std::scoped_lock lock{queue_mutex};
            fg_active = true;
            ++fg_epoch;
        }
        queue_cv.notify_one();
        return;
    }
    if (!IsAsync()) {
        // Release even if the copy throws: the frame holds nothing the next one depends on.
        struct ReleaseOnExit {
            PresentManager& manager;
            PresentFrame& frame;
            ~ReleaseOnExit() {
                manager.Release(frame);
            }
        } release{*this, frame};
        CopyToSwapchain(frame);
        return;
    }
    {
        std::scoped_lock lock{queue_mutex};
        queued_frames.push_back(&frame);
        ++frames_in_flight;
    }
    queue_cv.notify_one();
}

void PresentManager::WaitIdle() {
    if (!IsAsync()) {
        return;
    }
    std::unique_lock lock{queue_mutex};
    idle_cv.wait(lock, [this] { return frames_in_flight == 0; });
}

std::optional<std::string> PresentManager::TakeError() {
    std::scoped_lock lock{error_mutex};
    return std::exchange(error, std::nullopt);
}

void PresentManager::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("D3D12Present");
    // Late presents are visible stutter; the thread sleeps on the swapchain otherwise.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    while (true) {
        PresentFrame* frame{};
        u64 epoch{};
        {
            std::unique_lock lock{queue_mutex};
            if (!queue_cv.wait(lock, token,
                               [this] { return !queued_frames.empty() || fg_active; })) {
                return; // stop requested
            }
            epoch = fg_epoch;
            if (!queued_frames.empty()) {
                frame = queued_frames.front();
                queued_frames.pop_front();
            }
        }
        if (frame == nullptr) {
            // Frame generation: the swapchain paces this loop.
            if (failed) {
                StopGenerating(epoch);
                continue;
            }
            try {
                PresentGenerated(epoch);
            } catch (const std::exception& e) {
                ReportError(e);
                StopGenerating(epoch);
            }
            continue;
        }
        if (!failed) {
            try {
                CopyToSwapchain(*frame);
            } catch (const std::exception& e) {
                ReportError(e);
            }
        }
        Release(*frame);
        {
            std::scoped_lock lock{queue_mutex};
            --frames_in_flight;
        }
        idle_cv.notify_all();
    }
}

void PresentManager::ReportError(const std::exception& e) {
    // Logged here; the renderer stops presenting on its next frame (TakeError).
    LOG_CRITICAL(Render, "D3D12: present thread: {}", e.what());
    failed = true;
    std::scoped_lock lock{error_mutex};
    error = e.what();
}

void PresentManager::StopGenerating(u64 epoch) {
    std::scoped_lock lock{queue_mutex};
    if (fg_epoch == epoch) {
        fg_active = false; // the next ingested frame wakes the thread again
    }
}

void PresentManager::PresentGenerated(u64 epoch) {
    if (!frame_gen->HasFrames()) {
        StopGenerating(epoch);
        return;
    }
    // One frame queued ahead of the display: a second one would add a whole slot to the input
    // latency. A slot only composes frames the GPU has finished (FrameGen::Next) on the present
    // queue, well inside one vsync.
    if (!swapchain.WaitForFrame(1)) {
        return;
    }
    // From here on the swapchain expects a Present: every path below presents.
    const FrameGen::Output output = frame_gen->Next(HostSeconds());
    CopyContext& context = copy_contexts[fg_context];
    fg_context = (fg_context + 1) % FRAME_COUNT;
    WaitForCopy(context.fence_value);
    ThrowIfFailed(context.allocator->Reset(), "ID3D12CommandAllocator::Reset (frame gen)");
    ThrowIfFailed(copy_list->Reset(context.allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset (frame gen)");
    ID3D12Resource* const back_buffer = swapchain.Image(swapchain.CurrentIndex());
    TransitionResource(copy_list.Get(), back_buffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    frame_gen->RecordOutput(copy_list.Get(), output, back_buffer);
    TransitionResource(copy_list.Get(), back_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    ThrowIfFailed(copy_list->Close(), "ID3D12GraphicsCommandList::Close (frame gen)");

    // The ingest of the slots may still be on its way to the queue, as rendering may.
    scheduler.WaitSubmitted(output.tick);
    {
        std::scoped_lock lock{device.PresentQueueMutex()};
        ID3D12CommandQueue* const queue = device.PresentQueue();
        if (device.HasPresentQueue()) {
            ThrowIfFailed(queue->Wait(scheduler.Fence(), output.tick),
                          "ID3D12CommandQueue::Wait (frame gen)");
        }
        ID3D12CommandList* const lists[] = {copy_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ThrowIfFailed(queue->Signal(copy_fence.Get(), ++copy_fence_value),
                      "ID3D12CommandQueue::Signal (frame gen)");
        context.fence_value = copy_fence_value;
        frame_gen->Unpin(output, copy_fence_value);
        swapchain.Present(output.sync_interval);
    }
    if (output.idle) {
        // The game stopped presenting: the screen keeps this frame until it comes back.
        StopGenerating(epoch);
    }
}

void PresentManager::CopyToSwapchain(PresentFrame& frame) {
    // Waiting here, before taking the queue lock, is what keeps Present from blocking under it.
    // A frame the swapchain did not accept in time is dropped: presenting it would block.
    if (!swapchain.WaitForFrame()) {
        return;
    }
    CopyContext& context = copy_contexts[frame.index];
    WaitForCopy(context.fence_value);
    ThrowIfFailed(context.allocator->Reset(), "ID3D12CommandAllocator::Reset (present)");
    ThrowIfFailed(copy_list->Reset(context.allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset (present)");

    ID3D12Resource* const back_buffer = swapchain.Image(swapchain.CurrentIndex());
    TransitionResource(copy_list.Get(), back_buffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    copy_list->CopyResource(back_buffer, frame.image.Get());
    TransitionResource(copy_list.Get(), back_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    ThrowIfFailed(copy_list->Close(), "ID3D12GraphicsCommandList::Close (present)");

    // The queue runs lists in submission order, and the frame's rendering may still be on its way
    // to the queue from the recording worker: the copy must not overtake it.
    scheduler.WaitSubmitted(frame.render_tick);
    std::scoped_lock lock{device.PresentQueueMutex()};
    ID3D12CommandQueue* const queue = device.PresentQueue();
    if (device.HasPresentQueue()) {
        // Another queue: submission order says nothing, the scheduler's fence does.
        ThrowIfFailed(queue->Wait(scheduler.Fence(), frame.render_tick),
                      "ID3D12CommandQueue::Wait (present)");
    }
    ID3D12CommandList* const lists[] = {copy_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ThrowIfFailed(queue->Signal(copy_fence.Get(), ++copy_fence_value),
                  "ID3D12CommandQueue::Signal (present)");
    context.fence_value = copy_fence_value;
    frame.copy_value = copy_fence_value;
    swapchain.Present();
}

void PresentManager::WaitForCopy(u64 value) {
    const u64 completed = copy_fence->GetCompletedValue();
    if (completed >= value || completed == UINT64_MAX) {
        return; // done, or the device is gone (the fence then reads all ones)
    }
    ThrowIfFailed(copy_fence->SetEventOnCompletion(value, copy_event),
                  "ID3D12Fence::SetEventOnCompletion (present)");
    WaitForSingleObjectEx(copy_event, INFINITE, FALSE);
}

void PresentManager::Release(PresentFrame& frame) {
    {
        std::scoped_lock lock{free_mutex};
        free_frames.push_back(&frame);
    }
    free_cv.notify_one();
}

} // namespace D3D12
