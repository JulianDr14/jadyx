// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <dxgi1_3.h>

#include "common/logging.h"
#include "video_core/frame_trace.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"

namespace D3D12 {

namespace {

/// A wait longer than this means the swapchain stopped presenting (a suspended app, a lost
/// device): the frame is dropped, and a lost device is reported by the next submission.
constexpr DWORD FRAME_WAIT_TIMEOUT_MS = 1000;
/// How long the window may stay bound to the frontend's swapchain after the handoff.
constexpr int HANDOFF_RETRIES = 40, HANDOFF_RETRY_MS = 50;

std::mutex handoff_mutex;
std::function<void()> window_handoff;
std::function<void(void*)> swapchain_attach;

void AttachSwapChain(void* swap_chain) {
    std::function<void(void*)> attach;
    {
        std::scoped_lock lock{handoff_mutex};
        attach = swapchain_attach;
    }
    if (!attach) {
        throw std::runtime_error("D3D12: no composition swapchain callback is installed");
    }
    attach(swap_chain);
}

} // Anonymous namespace

void SetWindowHandoff(std::function<void()> release) {
    std::scoped_lock lock{handoff_mutex};
    window_handoff = std::move(release);
}

void SetSwapChainAttach(std::function<void(void*)> attach) {
    std::scoped_lock lock{handoff_mutex};
    swapchain_attach = std::move(attach);
}

Swapchain::Swapchain(const Device& device, IUnknown* panel, u32 width_, u32 height_)
    : composition_panel{panel}, width{width_}, height{height_} {
    DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = width,
        .Height = height,
        .Format = FORMAT,
        .Stereo = FALSE,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = IMAGE_COUNT,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
        .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
    };
    std::function<void()> handoff;
    {
        std::scoped_lock lock{handoff_mutex};
        handoff = std::exchange(window_handoff, nullptr);
    }
    if (handoff) handoff();
    ComPtr<IDXGISwapChain1> swapchain1;
    HRESULT hr = device.Factory()->CreateSwapChainForComposition(device.Queue(), &desc,
                                                                  nullptr, &swapchain1);
    for (int attempt = 0; handoff && hr == E_INVALIDARG && attempt < HANDOFF_RETRIES; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{HANDOFF_RETRY_MS});
        hr = device.Factory()->CreateSwapChainForComposition(device.Queue(), &desc, nullptr,
                                                              &swapchain1);
        if (SUCCEEDED(hr)) {
            LOG_INFO(Render, "D3D12: window free after the handoff in {} ms",
                     (attempt + 1) * HANDOFF_RETRY_MS);
        }
    }
    if (FAILED(hr)) {
        LOG_WARNING(Render, "D3D12: waitable swapchain rejected (0x{:08X}); Present will block",
                    static_cast<u32>(hr));
        desc.Flags = 0;
        hr = device.Factory()->CreateSwapChainForComposition(device.Queue(), &desc, nullptr,
                                                              &swapchain1);
    }
    ThrowIfFailed(hr, "CreateSwapChainForComposition");
    ThrowIfFailed(swapchain1.As(&swapchain), "IDXGISwapChain3 query");
    if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        // A waitable swapchain keeps its own latency, not the device's (IDXGIDevice1).
        if (FAILED(swapchain->SetMaximumFrameLatency(MAX_FRAME_LATENCY))) {
            LOG_WARNING(Render, "D3D12: SetMaximumFrameLatency({}) failed", MAX_FRAME_LATENCY);
        }
        frame_latency_waitable = swapchain->GetFrameLatencyWaitableObject();
    }
    for (u32 i = 0; i < IMAGE_COUNT; ++i) {
        ThrowIfFailed(swapchain->GetBuffer(i, IID_PPV_ARGS(&images[i])),
                      "IDXGISwapChain::GetBuffer");
    }
    AttachSwapChain(swapchain.Get());
    LOG_INFO(Render, "D3D12: swapchain {}x{}, {} images, frame latency {}", width, height,
             IMAGE_COUNT,
             frame_latency_waitable ? fmt::format("{} (waitable)", MAX_FRAME_LATENCY)
                                    : std::string("default (Present blocks)"));
}

Swapchain::~Swapchain() {
    if (composition_panel) {
        try {
            AttachSwapChain(nullptr);
        } catch (const std::exception& error) {
            LOG_WARNING(Render, "D3D12: could not detach the composition swapchain: {}", error.what());
        }
    }
    if (frame_latency_waitable != nullptr) {
        CloseHandle(frame_latency_waitable);
    }
}

u32 Swapchain::CurrentIndex() const {
    return swapchain->GetCurrentBackBufferIndex();
}

bool Swapchain::WaitForFrame() {
    if (frame_latency_waitable == nullptr) {
        return true;
    }
    const VideoCore::FrameTrace::ScopedSpan trace_wait{
        VideoCore::FrameTrace::Event::HostPresentLong, 1};
    // Not alertable: an APC ending the wait early would let Present block.
    if (WaitForSingleObjectEx(frame_latency_waitable, FRAME_WAIT_TIMEOUT_MS, FALSE) ==
        WAIT_TIMEOUT) {
        LOG_WARNING(Render, "D3D12: the swapchain accepted no frame for {} ms; frame dropped",
                    FRAME_WAIT_TIMEOUT_MS);
        return false;
    }
    return true;
}

void Swapchain::Present() {
    const VideoCore::FrameTrace::ScopedSpan trace_present{
        VideoCore::FrameTrace::Event::HostPresentLong, 0};
    ThrowIfFailed(swapchain->Present(1, 0), "IDXGISwapChain::Present");
}

} // namespace D3D12
