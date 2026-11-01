// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "common/polyfill_thread.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"

namespace D3D12 {

class CpuDescriptorAllocator;
class DescriptorRing;
class PresentManager;
class CommandStream;
class SamplerHeap;
class Scheduler;
class ShaderCompiler;

/// An image the renderer composites one frame into, sized and formatted like the back buffers.
struct PresentFrame {
    ComPtr<ID3D12Resource> image;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    /// State of image after the commands recorded so far; COPY_SOURCE while it waits to be
    /// presented.
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COPY_SOURCE};
    /// Scheduler tick that submitted the frame's rendering.
    u64 render_tick{};
    /// Copy fence value of the frame's last copy to the back buffer.
    u64 copy_value{};
    u32 index{};

    /// Records a transition to next on cmd unless image is already there.
    void Transition(CommandStream* cmd, D3D12_RESOURCE_STATES next);
};

/// A frame lent to the renderer: it goes back to the pool when the lease ends without a Present
/// (a composition that threw), so a failed frame never starves the pool.
class FrameLease {
public:
    FrameLease() = default;
    FrameLease(PresentManager& manager, PresentFrame& frame) : manager{&manager}, frame{&frame} {}
    ~FrameLease();

    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    PresentFrame& operator*() const noexcept {
        return *frame;
    }
    PresentFrame* operator->() const noexcept {
        return frame;
    }

private:
    friend PresentManager;
    PresentManager* manager{};
    PresentFrame* frame{};
};

/// What the renderer knows about a frame it presents.
struct PresentInfo {
    /// Guest vsync the game's frame was composed on: frame generation's content clock.
    u64 vsync_index{};
    /// Frame generation may show it (a game frame, not a menu or a boot screen).
    bool frame_gen{};
    /// The game queued a new frame for it, at host time queue_time (seconds, 0 when unknown).
    /// Without one only the overlay changed: frame generation leaves it out.
    bool new_frame{true};
    double queue_time{};
};

/// Presents composited frames, on a thread of its own when it can (Vulkan's PresentManager).
///
/// The renderer composites each frame into a PresentFrame and hands it over with Present(). The
/// present thread waits until the swapchain accepts a frame (its frame latency waitable object),
/// copies the frame into the back buffer with a command list of its own and presents it. The GPU
/// thread never blocks on the display: it only waits when every frame is still queued.
///
/// Synchronization:
/// - GPU order: both threads submit to the one direct queue, and a frame is queued only after the
///   scheduler submitted its rendering, so its copy executes after it. A frame is lent again only
///   after its copy was submitted, so the next rendering into it executes after the copy too.
///   With a present queue of its own (Device::HasPresentQueue) the copy waits on the GPU for the
///   scheduler's fence instead, and a frame is lent again only once its copy is done.
/// - DXGI submits to the queue inside Present, and Present racing ExecuteCommandLists on another
///   thread can deadlock: both go through Device::QueueMutex. The present thread waits for the
///   swapchain before taking it, so Present does not block while holding it.
/// - CPU reuse: a frame's copy allocator is reset only once the copy fence has passed it, and a
///   lent frame waits for the GPU to finish the rendering it held before (at most FRAME_COUNT
///   frames ahead of the GPU).
///
/// present_mailbox (VideoCore::FramePipeline): the GPU thread is not paced by the display. With
/// no free frame it takes back the oldest queued one, which is never shown; the newer one queued
/// behind it shows in its place. A frame's previous copy is then waited for by the emulation
/// queue (Scheduler::QueueWaitBeforeNextSubmit) rather than the CPU, and the CPU runs at most
/// RENDER_AHEAD composites ahead of the GPU.
///
/// Without a waitable swapchain (Present would block holding the queue lock) or with
/// async_presentation off, the copy and Present run on the caller's thread instead.
///
/// Frame generation (FrameGen, present thread only): while it runs, a presented frame is ingested
/// into its history on the scheduler's list and goes back to the pool at once; the present thread
/// then wakes on the swapchain alone and shows a real or an in-between frame at every display
/// slot. Queued frames still go first, so a switch back to plain presentation is seamless.
class PresentManager {
public:
    static constexpr u32 FRAME_COUNT = Swapchain::IMAGE_COUNT;
    /// present_mailbox: composites the CPU may run ahead of the GPU.
    static constexpr u32 RENDER_AHEAD = 8;

    PresentManager(const Device& device, Scheduler& scheduler, Swapchain& swapchain,
                   CpuDescriptorAllocator& rtv_descriptors, const ShaderCompiler& shader_compiler,
                   DescriptorRing& descriptor_ring, SamplerHeap& sampler_heap,
                   bool async_presentation);
    ~PresentManager();

    PresentManager(const PresentManager&) = delete;
    PresentManager& operator=(const PresentManager&) = delete;

    /// A frame to composite into; blocks while every frame is queued for presentation. With
    /// present_mailbox on, reuses the oldest queued frame instead, and only blocks while every
    /// frame is being copied to the display.
    [[nodiscard]] FrameLease AcquireFrame();

    /// Submits the commands recorded so far (they must have rendered the frame) and presents it.
    void Present(FrameLease&& lease, const PresentInfo& info = {});

    /// Blocks until every queued frame has been presented.
    void WaitIdle();

    /// Changes the size of the swapchain and of the frames, once everything in flight is done.
    /// GPU thread, with no frame lent. False keeps the current size, and everything still works.
    [[nodiscard]] bool Resize(u32 width, u32 height);

    [[nodiscard]] bool IsAsync() const noexcept {
        return present_thread.joinable();
    }

    /// The error that stopped the present thread, once. Frames presented after it are dropped.
    [[nodiscard]] std::optional<std::string> TakeError();

private:
    friend FrameLease;

    struct CopyContext {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 fence_value{}; ///< copy fence value of the last copy recorded with it
    };

    void PresentThread(std::stop_token token);
    [[nodiscard]] ComPtr<ID3D12Resource> CreateFrameImage(u32 index, u32 width, u32 height) const;
    void CreateFrameGen();
    /// Copies frame into the back buffer and presents it (present thread, or the caller).
    void CopyToSwapchain(PresentFrame& frame);
    /// Present thread: shows frame generation's next output, when it has one.
    void PresentGenerated(u64 epoch);
    /// Present thread: frame generation stops waking it, unless a frame came after epoch.
    void StopGenerating(u64 epoch);
    void ReportError(const std::exception& e);
    void WaitForCopy(u64 value);
    /// Returns frame to the pool.
    void Release(PresentFrame& frame);
    [[nodiscard]] bool HasFreeFrame();
    /// A free frame, or null.
    [[nodiscard]] PresentFrame* TakeFreeFrame();
    /// present_mailbox: the oldest frame queued and not yet taken by the present thread, or null.
    [[nodiscard]] PresentFrame* ReuseQueuedFrame();

    const Device& device;
    Scheduler& scheduler;
    Swapchain& swapchain;
    CpuDescriptorAllocator& rtv_descriptors;
    const ShaderCompiler& shader_compiler;
    DescriptorRing& descriptor_ring;
    SamplerHeap& sampler_heap;

    std::array<PresentFrame, FRAME_COUNT> frames;
    std::array<CopyContext, FRAME_COUNT> copy_contexts;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    ComPtr<ID3D12Fence> copy_fence;
    u64 copy_fence_value{}; ///< last value signaled, by whoever presents
    HANDLE copy_event{};

    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::deque<PresentFrame*> free_frames;

    std::mutex queue_mutex;
    std::condition_variable_any queue_cv;
    std::condition_variable_any idle_cv;
    std::deque<PresentFrame*> queued_frames;
    u32 frames_in_flight{}; ///< queued or being presented
    bool fg_active{};       ///< the present thread generates frames between queued ones
    u64 fg_epoch{};         ///< frames ingested by frame generation

    /// Async presentation only; built lazily on its first use.
    std::unique_ptr<FrameGen> frame_gen;
    bool fg_running{};      ///< GPU thread: the last frame went to frame generation
    double last_new_frame{}; ///< GPU thread: host time of the game's last new frame
    u32 fg_context{};       ///< present thread: next copy context for a generated frame
    /// GPU thread: render ticks of the last RENDER_AHEAD composites, oldest at the index.
    std::array<u64, RENDER_AHEAD> composite_ticks{};
    u32 composite_tick_index{};

    std::mutex error_mutex;
    std::optional<std::string> error;
    bool failed{}; ///< present thread only

    /// Last member: stopped and joined before the state it uses goes away.
    std::jthread present_thread;
};

} // namespace D3D12
