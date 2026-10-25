// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include "common/common_types.h"
#include "common/math_util.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"
#include "core/hle/service/nvnflinger/hwc_layer.h"

namespace Service::Nvidia::NvCore {
class Container;
class NvMap;
} // namespace Service::Nvidia::NvCore

namespace Service::Nvidia::Devices {

class nvmap;

class nvdisp_disp0 final : public nvdevice {
public:
    explicit nvdisp_disp0(Core::System& system_, NvCore::Container& core);
    ~nvdisp_disp0() override;

    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<u8> output) override;
    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output) override;
    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output) override;

    void OnOpen(NvCore::SessionId session_id, DeviceFD fd) override;
    void OnClose(DeviceFD fd) override;

    /// Performs a screen flip, compositing each buffer. Returns the fence the buffers are
    /// released with: signalled once the composite read them, or NoFence without a release
    /// syncpoint.
    android::Fence Composite(std::span<const Nvnflinger::HwcLayer> sorted_layers);

    /// Waits before the next composite. Buffers released with a fence leave one composite in
    /// flight; without one, the last composite is waited for.
    void WaitForComposite();

    /// Release the composited buffers with a fence on a display syncpoint (the default) instead
    /// of NoFence. Read when the device is created.
    static void SetReleaseFence(bool enabled);

    Kernel::KEvent* QueryEvent(u32 event_id) override;

    // Pulse: the display device keeps no state of its own; composition belongs to vi.
    bool PulseCapture(DeviceFD fd, Service::Pulse::StateWriter& out) const override {
        return true;
    }

private:
    NvCore::Container& container;
    NvCore::NvMap& nvmap;
    /// The display's release syncpoint, 0 without one. It has the same id on every boot, so a
    /// Pulse restore of the syncpoints raises the values the checkpoint's buffers wait on.
    u32 release_syncpoint{};
};

} // namespace Service::Nvidia::Devices
