// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>

#include <boost/container/flat_map.hpp>

#include "core/hle/service/nvnflinger/buffer_item.h"
#include "core/hle/service/nvnflinger/display.h"

namespace Service::Nvidia::Devices {
class nvdisp_disp0;
}

namespace Service::Nvnflinger {

using ConsumerId = s32;

class HardwareComposer {
public:
    explicit HardwareComposer();
    ~HardwareComposer();

    u32 ComposeLocked(f32* out_speed_scale, Display& display,
                      Nvidia::Devices::nvdisp_disp0& nvdisp);
    void RemoveLayerLocked(Display& display, ConsumerId consumer_id);

private:
    u64 m_frame_number{0};

    /// Frame chain counters (VideoCore::Perf) at each of the last HISTORY_VSYNCS vsyncs: what led
    /// to a change of the game's swap interval, logged with it.
    static constexpr size_t HISTORY_VSYNCS = 60;
    static constexpr size_t HISTORY_COUNTERS = 9;
    std::array<std::array<u64, HISTORY_COUNTERS>, HISTORY_VSYNCS> m_history{};
    s32 m_game_swap_interval{};
    /// swap_interval_hold: the vsync of the game's last request for interval 2.
    std::optional<u64> m_last_interval_2_frame{};

private:
    using ReleaseFrameNumber = u64;

    struct Framebuffer {
        android::BufferItem item{};
        ReleaseFrameNumber release_frame_number{};
        /// Vsyncs this buffer stays on screen, normally the interval the game asked for.
        s32 pacing_interval{1};
        u64 last_acquire_frame{0};
        bool is_acquired{false};
        /// Signalled once the last composite that read the buffer is done with it.
        android::Fence release_fence{android::Fence::NoFence()};
    };

    enum class CacheStatus : u32 {
        NoBufferAvailable,
        BufferAcquired,
        CachedBufferReused,
    };

    boost::container::flat_map<ConsumerId, Framebuffer> m_framebuffers{};

private:
    bool TryAcquireFramebufferLocked(Layer& layer, Framebuffer& framebuffer);
    /// The vsyncs a new buffer of the game stays on screen (swap_interval_hold).
    s32 GamePacingIntervalLocked(s32 requested);
    CacheStatus CacheFramebufferLocked(Layer& layer, ConsumerId consumer_id);
    void ReleaseFramebuffersLocked(Display& display);
    /// Once per vsync: records the counters, and logs a change of the game's swap interval.
    void TrackFrameChainLocked(std::optional<s32> game_swap_interval);
};

} // namespace Service::Nvnflinger
