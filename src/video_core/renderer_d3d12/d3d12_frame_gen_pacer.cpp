// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <functional>

#include "video_core/renderer_d3d12/d3d12_frame_gen_pacer.h"

namespace D3D12 {

namespace {

constexpr double VSYNC = 1.0 / 60.0;
/// The offset's upward drift, seconds per second: follows a host that slowly falls behind.
constexpr double OFFSET_DRIFT = 0.0005;
/// The most the offset moves down per frame: an early arrival moves content time forward, and
/// a jump would show as a skip.
constexpr double OFFSET_STEP = 0.001;
/// Arrivals this much later (or earlier) than the offset, several in a row, mean the offset
/// moved for good (a stall the guest clock did not see, or a guest clock that jumped ahead).
constexpr double OFFSET_RESET = 0.150;
constexpr u32 OFFSET_RESET_FRAMES = 3;
/// Latest pairs of the window the delay leaves out (2 of 48: about the 96th percentile): one
/// late frame in a few seconds is held through instead of adding to every frame's latency.
constexpr u32 READY_SKIP = 2;
/// Content clock: frames compared before choosing, the vsync spread under which the vsync clock
/// always holds (a game locked to the vsync), and how much evener the queue clock must be to take
/// over, and to keep it.
constexpr u32 CLOCK_MIN_FRAMES = 8;
constexpr double CLOCK_MIN_SPREAD = 0.002;
constexpr double CLOCK_SWITCH = 4.0;
constexpr double CLOCK_SWITCH_BACK = 2.0;
/// The least content time between two frames, seconds.
constexpr double CONTENT_MIN_STEP = 0.0005;
constexpr double DELAY_GROW = 0.002;
constexpr double DELAY_SHRINK = 0.00025;
/// Fraction of the excess delay drained per output frame: a large excess (after a late burst)
/// drains in seconds without a visible fast-forward, a small one (under DELAY_SETTLE) settles in
/// a few frames, so the real frames land exactly on their slots.
constexpr double DELAY_DRAIN = 0.01;
constexpr double DELAY_SETTLE = 0.004;
constexpr double DELAY_SETTLE_GAIN = 0.15;
/// Frames a new output rate must persist before frame generation switches to it.
constexpr u32 SWITCH_FRAMES = 24;
/// Frames a change between two output rates (30 <-> 60) must persist: every change is a visible
/// step in the cadence.
constexpr u32 RATE_SWITCH_FRAMES = 90;
/// Hysteresis around each rate threshold, fps.
constexpr double RATE_BAND = 1.0;
/// Auto: games at or above this mean rate (with the band) go to 60, below it to 30.
constexpr double AUTO_60_FPS = 28.0;
/// Phases this close to a real frame show it.
constexpr double SNAP = 0.08;
/// Below this rate interpolation hides nothing: present the frames as they are.
constexpr double MIN_BASE_FPS = 11.0;

} // namespace

u32 FrameGenPacer::Want(FrameGenMode mode) const {
    if (mode == FrameGenMode::Off || num_intervals < 4 || mean_interval <= 0.0) {
        return 0;
    }
    // The mean, not the median: a game that mixes 2- and 3-vsync frames (20-30 fps) has a median
    // that flips between 30 and 20, and the rate would follow it. Each threshold has a band around
    // it: the current rate holds until the game is clearly past it.
    const double base_fps = 1.0 / mean_interval;
    const auto above = [&](double threshold, bool holding) {
        return base_fps >= threshold + (holding ? RATE_BAND : -RATE_BAND);
    };
    if (!above(MIN_BASE_FPS, target == 0)) {
        return 0;
    }
    switch (mode) {
    case FrameGenMode::Target30:
        return above(28.5, target == 30) ? 0 : 30;
    case FrameGenMode::Target60:
        return above(56.0, target == 60) ? 0 : 60;
    case FrameGenMode::Auto:
        // The main case: games locked below 30 (15, 20, or between) reach 30 with one real frame
        // out of two or three. Games that hold 30 (or close) go to 60.
        if (!above(AUTO_60_FPS, target == 30)) {
            return 30;
        }
        return above(56.0, target == 60) ? 0 : 60;
    default:
        return 0;
    }
}

double FrameGenPacer::NextContent(u64 vsync_index, double queue_time) {
    const double vsync_content = static_cast<double>(vsync_index) * VSYNC;
    if (queue_time <= 0.0) {
        if (queue_clock) {
            has_offset = false;
        }
        queue_clock = false;
        has_queue_base = false;
        num_clock = 0;
        last_queue = 0.0;
        return vsync_content;
    }
    // The vsync and queue intervals of the same frames: the evener clock is the game's.
    const double vsync_interval = static_cast<double>(vsync_index - last_vsync) * VSYNC;
    const double queue_interval = queue_time - last_queue;
    if (last_queue > 0.0 && queue_interval > 0.0 && vsync_interval < 0.5) {
        vsync_intervals[next_clock] = vsync_interval;
        queue_intervals[next_clock] = queue_interval;
        next_clock = (next_clock + 1) % HISTORY;
        num_clock = std::min(num_clock + 1, HISTORY);
    }
    last_queue = queue_time;
    if (num_clock >= CLOCK_MIN_FRAMES) {
        const auto variance = [this](const std::array<double, HISTORY>& values) {
            double mean = 0.0;
            for (u32 i = 0; i < num_clock; ++i) {
                mean += values[i];
            }
            mean /= static_cast<double>(num_clock);
            double sum = 0.0;
            for (u32 i = 0; i < num_clock; ++i) {
                sum += (values[i] - mean) * (values[i] - mean);
            }
            return sum / static_cast<double>(num_clock);
        };
        const double vsync_var = variance(vsync_intervals);
        const double queue_var = variance(queue_intervals);
        const bool was_queue = queue_clock;
        if (!queue_clock) {
            // A game locked to the vsync has even vsync intervals: it keeps the vsync clock.
            queue_clock = vsync_var > CLOCK_MIN_SPREAD * CLOCK_MIN_SPREAD &&
                          queue_var * CLOCK_SWITCH < vsync_var;
        } else {
            queue_clock = queue_var * CLOCK_SWITCH_BACK <= vsync_var;
        }
        if (queue_clock != was_queue) {
            // The content clock moved: the host offset is measured again.
            has_offset = false;
            has_queue_base = false;
        }
    }
    if (!queue_clock) {
        return vsync_content;
    }
    if (!has_queue_base) {
        queue_base = vsync_content - queue_time;
        has_queue_base = true;
    }
    // A queued frame is composed on a later vsync: content stays within two vsyncs before it.
    const double queue_content = queue_time + queue_base;
    if (queue_content > vsync_content) {
        queue_base -= queue_content - vsync_content;
    } else if (queue_content < vsync_content - 2.0 * VSYNC) {
        queue_base += vsync_content - 2.0 * VSYNC - queue_content;
    }
    return queue_time + queue_base;
}

void FrameGenPacer::AddInterval(double interval) {
    // Long pauses (loading, a menu) are not the game's frame rate.
    if (interval >= 0.5) {
        return;
    }
    intervals[next_interval] = interval;
    next_interval = (next_interval + 1) % HISTORY;
    num_intervals = std::min(num_intervals + 1, HISTORY);
    std::array<double, HISTORY> sorted{};
    std::copy_n(intervals.begin(), num_intervals, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + num_intervals);
    median_interval = sorted[num_intervals / 2];
    double sum = 0.0;
    for (u32 i = 0; i < num_intervals; ++i) {
        sum += sorted[i];
    }
    mean_interval = sum / static_cast<double>(num_intervals);
}

u32 FrameGenPacer::OnFrame(FrameGenMode mode, u64 vsync_index, double arrival, double queue_time) {
    if (has_last && vsync_index <= last_vsync) {
        // The same vsync again (a redraw), or a frame from outside the game (the menu over the
        // paused game comes with vsync 0): no new content time. Off still applies at once, or the
        // present thread would keep showing the game's frames over the menu.
        if (mode == FrameGenMode::Off) {
            target = 0;
            pending_target = 0;
            pending_count = 0;
        }
        return target;
    }
    if (has_last) {
        const double previous = content;
        // Never back, never twice the same time: the history is sorted by it.
        content = std::max(NextContent(vsync_index, queue_time), previous + CONTENT_MIN_STEP);
        AddInterval(content - previous);
    } else {
        content = static_cast<double>(vsync_index) * VSYNC;
        last_queue = queue_time;
    }
    last_vsync = vsync_index;
    has_last = true;

    const double sample = arrival - content;
    if (!has_offset) {
        offset = sample;
        has_offset = true;
        // A new offset: the ready slots measured against the old one say nothing.
        resynced = true;
    } else {
        offset += OFFSET_DRIFT * std::max(0.0, arrival - last_arrival);
        if (sample < offset) {
            offset = std::max(sample, offset - OFFSET_STEP);
        }
        const double late = std::max(0.0, sample - offset);
        // Far early: the guest clock jumped ahead (vsyncs caught up after a stall). Stepping the
        // offset down 1 ms per frame would take seconds; this is a resync as much as a stall is.
        const bool early = offset - sample > OFFSET_RESET;
        if (late > OFFSET_RESET || early) {
            if (++late_resets >= OFFSET_RESET_FRAMES) {
                offset = sample;
                late_resets = 0;
                // The delay grown while waiting through the stall is not the game's.
                resynced = true;
            }
        } else {
            late_resets = 0;
        }
    }
    last_arrival = arrival;

    const u32 want = Want(mode);
    if (want == target) {
        pending_count = 0;
    } else if (want == pending_target) {
        // Off is taken at once only when asked for; a rate change waits for it to hold.
        const u32 needed = target != 0 && want != 0 ? RATE_SWITCH_FRAMES : SWITCH_FRAMES;
        if (++pending_count >= needed || mode == FrameGenMode::Off) {
            target = want;
            pending_count = 0;
        }
    } else {
        pending_target = want;
        pending_count = mode == FrameGenMode::Off ? SWITCH_FRAMES : 1;
        if (mode == FrameGenMode::Off) {
            target = 0;
        }
    }
    return target;
}

double FrameGenPacer::NeededLag(double period) const {
    // Each pair A -> B needs B by the first slot past A's time: the slot before B was ready may
    // show A at most. That is the least lag that never holds (slot - lag = A's time at the last
    // slot B missed), and the one that shows A exactly on that slot. The slots are what the
    // measure is quantized to: no margin is needed. Host time against content time, without the
    // arrival offset: the offset steps (it follows the earliest arrivals) would move the content
    // shown.
    std::array<double, READY_WINDOW> needs{};
    for (u32 i = 0; i < num_ready; ++i) {
        const Ready& pair = readies[i];
        const double span = pair.content - pair.previous;
        const double need = pair.ready - period - pair.previous;
        // A frame later than an interval past its pair costs less as a hold than as latency.
        needs[i] = std::min(need, offset + span + median_interval);
    }
    const u32 rank = READY_SKIP * num_ready / READY_WINDOW;
    std::nth_element(needs.begin(), needs.begin() + rank, needs.begin() + num_ready,
                     std::greater<>{});
    return needs[rank];
}

FrameGenPacer::Choice FrameGenPacer::Pick(double slot_time, std::span<const Frame> frames) {
    Choice choice{};
    if (frames.empty() || !has_offset) {
        return choice;
    }
    const u32 newest = static_cast<u32>(frames.size() - 1);
    const double period = target != 0 ? 1.0 / static_cast<double>(target) : VSYNC;
    if (resynced) {
        resynced = false;
        has_lag = false;
        num_ready = 0;
        next_ready = 0;
        has_seen = false;
    }
    // The slot each frame was first there to show at: when its pair could first be blended.
    if (!has_seen) {
        // The frames there when the pacer (re)starts were ready at some earlier slot.
        newest_seen = frames[newest].content;
        has_seen = true;
    }
    for (u32 i = 1; i <= newest; ++i) {
        const Frame& frame = frames[i];
        if (frame.content <= newest_seen) {
            continue;
        }
        newest_seen = frame.content;
        if (frame.serial != frames[i - 1].serial + 1) {
            continue;
        }
        readies[next_ready] = {
            .previous = frames[i - 1].content, .content = frame.content, .ready = slot_time};
        next_ready = (next_ready + 1) % READY_WINDOW;
        num_ready = std::min(num_ready + 1, READY_WINDOW);
    }
    if (num_ready == 0) {
        // Nothing measured yet: the newest frame as it is.
        choice.kind = Choice::Kind::Real;
        choice.a = newest;
        return choice;
    }
    const double target_lag = NeededLag(period);
    if (!has_lag) {
        // The first pairs measure only where their frames fell between slots: a frame arriving
        // just after a slot needs one more. Start from a game interval over the offset (a frame
        // shown as it arrives, on a slot), which is near what the window settles at, and let the
        // measures move it from there.
        lag = std::max(target_lag, offset + median_interval);
        has_lag = true;
    } else if (target_lag > lag) {
        lag = std::min(target_lag, lag + DELAY_GROW);
    } else if (lag > target_lag) {
        // Shrinking the lag moves content time forward faster: a fraction of the excess per
        // frame.
        const double excess = lag - target_lag;
        const double gain = excess < DELAY_SETTLE ? DELAY_SETTLE_GAIN : DELAY_DRAIN;
        lag = std::max(target_lag, lag - std::max(DELAY_SHRINK, excess * gain));
    }
    const double interval = std::max(median_interval, VSYNC);
    // Content time never goes back (a resync after a stall would show frames again): it holds
    // until real time catches up.
    double t = slot_time - lag;
    if (has_floor && t < shown_floor) {
        t = shown_floor;
    }
    shown_floor = t;
    has_floor = true;

    if (t >= frames[newest].content) {
        // Late: the next frame has not arrived. Hold the newest. The delay grows through the
        // ready slots, not here: a pause (loading, a stall) would stay as input latency.
        choice.kind = Choice::Kind::Real;
        choice.a = newest;
        shown_floor = frames[newest].content;
        choice.idle = slot_time - (frames[newest].content + offset) > std::max(4.0 * interval, 0.3);
        return choice;
    }
    if (t <= frames[0].content) {
        choice.kind = Choice::Kind::Real;
        choice.a = 0;
        return choice;
    }
    u32 a = 0;
    while (a + 1 < newest && frames[a + 1].content <= t) {
        ++a;
    }
    const u32 b = a + 1;
    const double span = frames[b].content - frames[a].content;
    const double phase = span > 0.0 ? (t - frames[a].content) / span : 0.0;
    if (frames[b].serial != frames[a].serial + 1 || span > 0.25) {
        // No motion between them (a gap, a passthrough period): the nearer real frame.
        choice.kind = Choice::Kind::Real;
        choice.a = phase < 0.5 ? a : b;
        return choice;
    }
    if (phase < SNAP) {
        choice.kind = Choice::Kind::Real;
        choice.a = a;
    } else if (phase > 1.0 - SNAP) {
        choice.kind = Choice::Kind::Real;
        choice.a = b;
    } else {
        choice.kind = Choice::Kind::Blend;
        choice.a = a;
        choice.b = b;
        choice.phase = static_cast<float>(phase);
    }
    return choice;
}

void FrameGenPacer::Reset() {
    has_lag = false;
    has_floor = false;
    has_seen = false;
    num_ready = 0;
    next_ready = 0;
}

} // namespace D3D12
