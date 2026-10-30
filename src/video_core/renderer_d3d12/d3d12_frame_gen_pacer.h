// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"

namespace D3D12 {

/// Frame generation modes (boot.cfg "frame_gen").
enum class FrameGenMode : u32 {
    Off = 0,
    Auto = 1,     ///< games under 28 fps to 30, under 56 fps to 60 (each with a 1 fps band)
    Target30 = 30,
    Target60 = 60,
};

/// boot.cfg "frame_gen": off, auto, 30 or 60. Read on every frame, so it may change at run time.
/// Declared here, free of D3D12 headers, for the frontend.
void SetFrameGenMode(FrameGenMode mode);
[[nodiscard]] FrameGenMode GetFrameGenMode() noexcept;

/// When frame generation shows which frame: pure timing logic, no GPU.
///
/// Content clock: a guest frame's time is the guest vsync it was composed on (60 Hz), so a game
/// locked to 20 fps yields frames exactly 50 ms apart however late the host renders them. Arrival
/// (host time, seconds) minus content time gives the host offset; arrivals are only ever late, so
/// the offset follows their lower envelope and drifts up slowly.
///
/// Output clock: the present thread shows content time t = slot - offset - delay, where delay is
/// the longest recent frame interval plus the arrival jitter, so the frame after t has arrived by
/// then. t falls between two frames A and B at some phase; a phase near 0 or 1 shows the real
/// frame, and a small bias steers the phases onto the real frames (20 -> 30 then shows one real
/// frame out of three, 30 -> 60 one of two).
class FrameGenPacer {
public:
    /// One frame of the history, as the pacer sees it.
    struct Frame {
        double content{}; ///< seconds (guest vsync / 60)
        u64 serial{};     ///< ingest order; consecutive serials have motion between them
        u32 slot{};
    };

    struct Choice {
        enum class Kind : u8 {
            None,  ///< nothing to show yet
            Real,  ///< show frame a
            Blend, ///< interpolate from a to b at phase
        };
        Kind kind{Kind::None};
        u32 a{}; ///< index into the frames given to Pick
        u32 b{};
        float phase{};
        bool idle{}; ///< the game stopped presenting: the screen may keep the last frame
    };

    /// A new guest frame (GPU thread). Returns the output rate frame generation should run at:
    /// 0 to present the game's frames as they are, else 30 or 60.
    u32 OnFrame(FrameGenMode mode, u64 vsync_index, double arrival);

    /// The frame to show at host time slot_time (present thread). frames are sorted by content.
    [[nodiscard]] Choice Pick(double slot_time, std::span<const Frame> frames);

    /// Forgets the history (mode change, idle, a passthrough period).
    void Reset();

    [[nodiscard]] u32 Target() const noexcept {
        return target;
    }
    /// Mean recent guest frame interval, seconds: the rate frame generation goes by.
    [[nodiscard]] double BaseInterval() const noexcept {
        return mean_interval;
    }
    [[nodiscard]] double Delay() const noexcept {
        return delay + bias;
    }

private:
    static constexpr u32 HISTORY = 16;
    static constexpr u32 LATE_WINDOW = 32;

    u32 Want(FrameGenMode mode) const;

    // GPU thread state (OnFrame), read by Pick under the caller's lock.
    std::array<double, HISTORY> intervals{};
    u32 num_intervals{};
    u32 next_interval{};
    u64 last_vsync{};
    bool has_last{};
    double median_interval{};
    double mean_interval{}; ///< picks the output rate
    double max_interval{};
    double offset{};
    bool has_offset{};
    double last_arrival{};
    std::array<double, LATE_WINDOW> lates{}; ///< arrival lateness over the offset, seconds
    u32 next_late{};
    u32 num_late{};
    double jitter_peak{};
    u32 late_resets{};
    bool resynced{}; ///< the offset was reset: the present thread restarts the delay
    u32 target{};
    u32 pending_target{};
    u32 pending_count{};

    // Present thread state.
    double delay{};
    double bias{};
    double shown_floor{}; ///< content time last shown
    bool has_delay{};
    bool has_floor{};
};

} // namespace D3D12
