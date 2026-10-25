// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <optional>

#include <boost/container/small_vector.hpp>

#include "common/adpf.h"
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/frame_trace.h"
#include "video_core/perf_counters.h"

namespace Service::Nvidia::Devices {

namespace {

Tegra::BlendMode ConvertBlending(Service::Nvnflinger::LayerBlending blending) {
    switch (blending) {
    case Service::Nvnflinger::LayerBlending::None:
    default:
        return Tegra::BlendMode::Opaque;
    case Service::Nvnflinger::LayerBlending::Premultiplied:
        return Tegra::BlendMode::Premultiplied;
    case Service::Nvnflinger::LayerBlending::Coverage:
        return Tegra::BlendMode::Coverage;
    }
}

// The last syncpoint: guests are given the free ones from the bottom, so no checkpoint has one
// of theirs here.
constexpr u32 ReleaseSyncpointId = MaxSyncPoints - 1;

std::atomic_bool release_fence_enabled{true};

} // namespace

void nvdisp_disp0::SetReleaseFence(bool enabled) {
    release_fence_enabled.store(enabled, std::memory_order_relaxed);
}

nvdisp_disp0::nvdisp_disp0(Core::System& system_, NvCore::Container& core)
    : nvdevice{system_}, container{core}, nvmap{core.GetNvMapFile()} {
    if (release_fence_enabled.load(std::memory_order_relaxed)) {
        release_syncpoint =
            container.GetSyncpointManager().AllocateSyncpointAt(ReleaseSyncpointId, false);
        LOG_INFO(Service_NVDRV, "display buffers released on syncpoint {}", release_syncpoint);
    }
}

nvdisp_disp0::~nvdisp_disp0() {
    if (release_syncpoint != 0) {
        container.GetSyncpointManager().FreeSyncpoint(release_syncpoint);
    }
}

NvResult nvdisp_disp0::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvdisp_disp0::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvdisp_disp0::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<u8> output, std::span<u8> inline_output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvdisp_disp0::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvdisp_disp0::OnClose(DeviceFD fd) {}

android::Fence nvdisp_disp0::Composite(std::span<const Nvnflinger::HwcLayer> sorted_layers) {
    std::vector<Tegra::FramebufferConfig> output_layers;
    std::vector<Service::Nvidia::NvFence> output_fences;
    output_layers.reserve(sorted_layers.size());
    output_fences.reserve(sorted_layers.size());

    for (auto& layer : sorted_layers) {
        output_layers.emplace_back(Tegra::FramebufferConfig{
            .address = nvmap.GetHandleAddress(layer.buffer_handle),
            .offset = layer.offset,
            .width = layer.width,
            .height = layer.height,
            .stride = layer.stride,
            .pixel_format = layer.format,
            .transform_flags = layer.transform,
            .crop_rect = layer.crop_rect,
            .blending = ConvertBlending(layer.blending),
            .layer_stack_mask = layer.layer_stack_mask,
        });

        for (size_t i = 0; i < layer.acquire_fence.num_fences; i++) {
            if (layer.acquire_fence.fences[i].id >= 0) {
                output_fences.push_back(layer.acquire_fence.fences[i]);
            }
        }
    }

    VideoCore::Perf::Add(VideoCore::Perf::Counter::VsyncFrames, 1);
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::VsyncComposed, 1);
    android::Fence release_fence = android::Fence::NoFence();
    std::optional<NvFence> release;
    if (release_syncpoint != 0) {
        release = NvFence{
            .id = static_cast<s32>(release_syncpoint),
            .value = container.GetSyncpointManager().IncrementSyncpointMaxExt(release_syncpoint, 1),
        };
        release_fence.num_fences = 1;
        release_fence.fences[0] = *release;
    }
    system.GPU().RequestComposite(std::move(output_layers), std::move(output_fences), release);
    Common::ADPF::ReportFrameInterval();
    system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
    system.GetPerfStats().EndSystemFrame();
    system.GetPerfStats().BeginSystemFrame();
    return release_fence;
}

void nvdisp_disp0::WaitForComposite() {
    const auto start = std::chrono::steady_clock::now();
    if (release_syncpoint != 0) {
        system.GPU().WaitForEarlierComposite();
    } else {
        system.GPU().WaitForComposite();
    }
    const u64 waited_us = VideoCore::Perf::ElapsedUs(start);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::VsyncComposeWaitUs, waited_us);
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::ComposeWaitEnd, waited_us);
}

Kernel::KEvent* nvdisp_disp0::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown DISP Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
