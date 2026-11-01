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
/// locked to 20 fps yields frames exactly 50 ms apart however late the host renders them. A game
/// that paces itself (no vsync lock, 25 fps shown on 2 and 3 vsyncs in turn) animates by its own
/// frame times instead: when the times it queued its frames at run evener than the vsyncs, they
/// are the content clock, kept within two vsyncs of the display's. Arrival (host time, seconds)
/// minus content time gives the host offset; arrivals are only ever late, so the offset follows
/// their lower envelope and drifts up slowly.
///
/// Output clock: the present thread shows content time t = slot - offset - delay. A frame given to
/// Pick is ready to show (on the GPU); the slot each frame first came in at is kept for the
/// recent pairs, and the delay is the least that has every pair's later frame there by the first
/// slot past the earlier frame's time. The real frames then land on slots and the frames between
/// them are blended as soon as they can be: 20 -> 30 waits two thirds of an interval, 15 -> 30
/// and 30 -> 60 half of one, not the whole interval plus the jitter. The latest few pairs of the
/// window are left out: a rare late frame is held through (one repeated frame) rather than kept
/// as latency on every frame. t falls between two frames A and B at some phase; a phase near 0
/// or 1 shows the real frame.
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

    /// A new guest frame (GPU thread). queue_time is the host time the game queued it (seconds,
    /// 0 when unknown). Returns the output rate frame generation should run at: 0 to present the
    /// game's frames as they are, else 30 or 60.
    u32 OnFrame(FrameGenMode mode, u64 vsync_index, double arrival, double queue_time = 0.0);

    /// Content time of the last frame OnFrame took, seconds.
    [[nodiscard]] double Content() const noexcept {
        return content;
    }
    /// Whether the content clock follows the game's queue times rather than the vsyncs.
    [[nodiscard]] bool QueueClock() const noexcept {
        return queue_clock;
    }

    /// The frame to show at host time slot_time (present thread). frames are the ones ready to
    /// show, sorted by content; call at every output slot (the slot a frame first comes in at
    /// sets the delay).
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
    /// Playback delay over the arrival offset, seconds.
    [[nodiscard]] double Delay() const noexcept {
        return lag - offset;
    }

private:
    static constexpr u32 HISTORY = 16;
    static constexpr u32 READY_WINDOW = 48;

    /// A pair of consecutive frames and the slot the later one was first ready at.
    struct Ready {
        double previous{};
        double content{};
        double ready{};
    };

    u32 Want(FrameGenMode mode) const;
    double NextContent(u64 vsync_index, double queue_time);
    void AddInterval(double interval);
    double NeededLag(double period) const;

    // GPU thread state (OnFrame), read by Pick under the caller's lock.
    std::array<double, HISTORY> intervals{};
    u32 num_intervals{};
    u32 next_interval{};
    u64 last_vsync{};
    bool has_last{};
    double content{};
    // Content clock: the vsync and queue intervals of the same frames, to compare.
    std::array<double, HISTORY> vsync_intervals{};
    std::array<double, HISTORY> queue_intervals{};
    u32 num_clock{};
    u32 next_clock{};
    double last_queue{};
    double queue_base{}; ///< content = queue time + queue_base
    bool has_queue_base{};
    bool queue_clock{};
    double median_interval{};
    double mean_interval{}; ///< picks the output rate
    double offset{};
    bool has_offset{};
    double last_arrival{};
    u32 late_resets{};
    bool resynced{}; ///< the offset was reset: the present thread restarts the lag
    u32 target{};
    u32 pending_target{};
    u32 pending_count{};

    // Present thread state.
    std::array<Ready, READY_WINDOW> readies{};
    u32 next_ready{};
    u32 num_ready{};
    double newest_seen{}; ///< content time of the newest frame Pick has seen
    bool has_seen{};
    double lag{}; ///< host time minus the content time shown
    double shown_floor{}; ///< content time last shown
    bool has_lag{};
    bool has_floor{};
};

} // namespace D3D12
