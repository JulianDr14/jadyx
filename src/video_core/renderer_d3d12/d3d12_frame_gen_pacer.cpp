// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

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
constexpr double JITTER_MARGIN = 0.004;
/// The delay only shrinks when it is this much over what the jitter needs (a delay that follows
/// every jitter peak moves content time on every frame).
constexpr double DELAY_HYSTERESIS = 0.003;
constexpr double DELAY_GROW = 0.002;
constexpr double DELAY_SHRINK = 0.00025;
/// Fraction of the excess delay drained per output frame.
constexpr double DELAY_DRAIN = 0.01;
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
/// Phase-lock gain: fraction of the phase error steered away per output frame.
constexpr double LOCK_GAIN = 0.15;
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

u32 FrameGenPacer::OnFrame(FrameGenMode mode, u64 vsync_index, double arrival) {
    if (has_last && vsync_index > last_vsync) {
        // Long pauses (loading, a menu) are not the game's frame rate.
        const double interval = static_cast<double>(vsync_index - last_vsync) * VSYNC;
        if (interval < 0.5) {
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
            // The second longest interval of the last eight frames sets the delay, at most twice
            // the median: uneven cadences (2:3) keep their long interval, while a single hitch
            // is held through instead of staying in the delay as latency.
            double longest = 0.0;
            double second = 0.0;
            for (u32 i = 0; i < std::min(num_intervals, 8U); ++i) {
                const double value = intervals[(next_interval + HISTORY - 1 - i) % HISTORY];
                if (value > longest) {
                    second = longest;
                    longest = value;
                } else if (value > second) {
                    second = value;
                }
            }
            max_interval = std::min(num_intervals > 1 ? second : longest, 2.0 * median_interval);
        }
    } else if (has_last && vsync_index <= last_vsync) {
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
    last_vsync = vsync_index;
    has_last = true;

    const double content = static_cast<double>(vsync_index) * VSYNC;
    const double sample = arrival - content;
    if (!has_offset) {
        offset = sample;
        has_offset = true;
        num_late = 0;
        jitter_peak = 0.0;
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
                num_late = 0;
                jitter_peak = 0.0;
                // The delay grown while waiting through the stall is not the game's jitter.
                resynced = true;
            }
        } else {
            late_resets = 0;
            // The jitter is the latest arrival of the window: steady while the game is.
            lates[next_late] = late;
            next_late = (next_late + 1) % LATE_WINDOW;
            num_late = std::min(num_late + 1, LATE_WINDOW);
            jitter_peak = *std::max_element(lates.begin(), lates.begin() + num_late);
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

FrameGenPacer::Choice FrameGenPacer::Pick(double slot_time, std::span<const Frame> frames) {
    Choice choice{};
    if (frames.empty() || !has_offset) {
        return choice;
    }
    const u32 newest = static_cast<u32>(frames.size() - 1);
    const double target_delay = max_interval + jitter_peak + JITTER_MARGIN;
    if (resynced) {
        resynced = false;
        has_delay = false;
    }
    if (!has_delay) {
        delay = target_delay;
        bias = 0.0;
        has_delay = true;
    } else if (target_delay > delay) {
        delay = std::min(target_delay, delay + DELAY_GROW);
    } else if (delay > target_delay + DELAY_HYSTERESIS) {
        // Shrinking the delay moves content time forward faster: only slowly, a fraction of the
        // excess per frame, so a large excess (after a jitter burst) drains in seconds without a
        // visible fast-forward.
        const double excess = delay - target_delay;
        delay = std::max(target_delay, delay - std::max(DELAY_SHRINK, excess * DELAY_DRAIN));
    }
    const double interval = std::max(median_interval, VSYNC);
    // Content time never goes back (a resync after a stall would show frames again): it holds
    // until real time catches up.
    double t = slot_time - offset - delay - bias;
    if (has_floor && t < shown_floor) {
        t = shown_floor;
    }
    shown_floor = t;
    has_floor = true;

    if (t >= frames[newest].content) {
        // Late: the next frame has not arrived. Hold the newest. The delay does not grow here:
        // late arrivals raise the jitter window, and a pause (loading, a stall) is not jitter;
        // every extra millisecond would stay as input latency.
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

    // Phase lock: steer the nearest real frame onto an output slot. Only the output nearest to a
    // real frame steers: outputs advance the phase by step, so at most one of them is closer than
    // min(step, 1 - step) / 2 to a frame. Two (20 -> 30: phases 1/6 and 5/6) would pull against
    // each other and hold every output between frames.
    const double error = phase < 0.5 ? phase : phase - 1.0;
    const double step = target != 0 && span > 0.0 ? 1.0 / (target * span) : 1.0;
    if (step < 1.0 && std::abs(error) < 0.5 * std::min(step, 1.0 - step)) {
        bias += LOCK_GAIN * error * span;
        bias = std::clamp(bias, -0.5 * interval, 0.5 * interval);
    }

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
    has_delay = false;
    has_floor = false;
    bias = 0.0;
}

} // namespace D3D12
