// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <dxgi1_4.h>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Flip-model swapchain on a UWP CoreWindow (the only presentation surface a UWP app on Xbox has).
///
/// Created with a frame latency waitable object when DXGI allows it ("Reduce latency with DXGI 1.3
/// swap chains"): WaitForFrame() then blocks until the swapchain accepts another frame, so the
/// Present() that follows does not block. Without it, Present() blocks instead.
class Swapchain {
public:
    static constexpr u32 IMAGE_COUNT = 3;
    static constexpr DXGI_FORMAT FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
    /// Frames queued for display at most. Two lets the GPU work on one frame while the previous
    /// one waits for its vblank.
    static constexpr u32 MAX_FRAME_LATENCY = 2;

    /// core_window is the CoreWindow's IUnknown, as winrt::get_unknown hands it out.
    Swapchain(const Device& device, IUnknown* core_window, u32 width, u32 height);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    u32 Width() const {
        return width;
    }
    u32 Height() const {
        return height;
    }
    u32 CurrentIndex() const;
    ID3D12Resource* Image(u32 index) const {
        return images[index].Get();
    }

    /// Whether WaitForFrame() can wait (the swapchain has a frame latency waitable object).
    [[nodiscard]] bool HasFrameLatencyWaitable() const noexcept {
        return frame_latency_waitable != nullptr;
    }
    /// Blocks until the swapchain can queue another frame. Call once before every Present().
    /// False when it accepted none for a second (a lost device, a stalled display): Present()
    /// would block then, so the caller drops the frame instead. True without a waitable object.
    ///
    /// latency (1 to MAX_FRAME_LATENCY) is how many frames may be queued ahead of the display:
    /// frame generation at 30 fps keeps one, a 33 ms frame less of input latency than two. DXGI
    /// does not take back the waitable's count when SetMaximumFrameLatency lowers the latency,
    /// so a lower latency holds the extra counts instead (waits for them without presenting) and
    /// gives them back when it is raised.
    [[nodiscard]] bool WaitForFrame(u32 latency = MAX_FRAME_LATENCY);
    /// sync_interval: vertical blanks the frame stays on screen (frame generation at 30 uses 2).
    void Present(u32 sync_interval = 1);
    /// Changes the back buffers' size. Nothing may be using them, on the CPU or the GPU, and the
    /// caller holds the queue's lock. False leaves the swapchain as it was.
    [[nodiscard]] bool Resize(u32 new_width, u32 new_height);

private:
    void GetImages();

    ComPtr<IDXGISwapChain3> swapchain;
    std::array<ComPtr<ID3D12Resource>, IMAGE_COUNT> images;
    HANDLE frame_latency_waitable{};
    u32 held_frames{}; ///< waitable counts taken without a Present (a latency under the maximum)
    u32 width;
    u32 height;
};

} // namespace D3D12
