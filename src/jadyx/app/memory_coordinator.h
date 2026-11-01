// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <optional>
#include "common/common_types.h"

namespace Jadyx {

/// One owner for the free app memory: Permafrost, the texture cache and the JIT used to read the
/// headroom each against fixed marks, and fought over it. Permafrost kept 1.4-1.7 GB free even
/// when the game's working set did not fit beside it: in a heavy area it wrote 3 GB to disk and
/// read 3 GB back every 10 s, the guest threads stalled 1.7 s per second, and none of it was cold.
///
/// The guest's stall on reads back says when Permafrost is evicting memory the game uses. Then
/// the others give way first, in order of what they cost to get back:
///   1. Yield: idle textures go (they decode again from guest memory), the free pages of the
///      guest's pools and of emulated DRAM are trimmed, and the textures stop growing.
///   2. Relief, only if the stall outlasts the yield's grace: Permafrost's marks come down, so it
///      stops chasing headroom the working set does not leave. They never go below `low_floor`,
///      and they come back slowly once the stall is over.
/// With no stall the textures may grow into the headroom above the line (Permafrost's watch mark
/// plus a margin), memory Permafrost freed from cold granules included. What they hold above
/// their floor is a loan: Permafrost counts it as headroom while the textures give it back.
///
/// With no relief every mark is what it was before the coordinator: the JIT and the urgency marks
/// derive from the low mark as they always did.
class MemoryCoordinator {
public:
    static constexpr u64 MiB = 1ULL << 20;

    struct Config {
        /// Permafrost's marks with no relief.
        u64 watch{1536 * MiB};
        u64 low{1024 * MiB};
        u64 high{1280 * MiB};
        /// The relief stops with the low mark here: the texture cache's critical level (384 MiB)
        /// stays below Permafrost's urgent mark, so the textures keep yielding before it acts.
        u64 low_floor{512 * MiB};
        /// The textures grow only into the headroom this far above the watch mark.
        u64 texture_margin{256 * MiB};
        /// texture_cap_line_mib: replaces watch + margin (0 never grows).
        std::optional<u64> texture_line;
        /// Guest stall on reads back, all guest threads, that starts and ends a yield. A heavy
        /// area at 18-38 ms/s was evicting granules that came back; a calm one stays at 0-1.
        u64 stall_on_us_per_s{32000};
        u64 stall_off_us_per_s{8000};
        /// Smoothing of the stall rate.
        u64 stall_tau_ms{1000};
        /// How long the stall must stay over stall_on before Permafrost's marks come down: the
        /// yield goes alone that long. A load burst of a second ends inside it.
        u64 grace_ms{3000};
        /// After a yield the relief holds, and the textures stay at their floor, this long.
        u64 hold_ms{30000};
        /// Relief per second while the stall lasts, and given back per second after the hold.
        u64 relief_up{256 * MiB};
        u64 relief_down{32 * MiB};
        /// A stall that comes back while the relief is easing: the relief does not ease below
        /// where it came back plus this, for relief_floor_ms. So the loop settles, not cycles.
        u64 relief_step{128 * MiB};
        u64 relief_floor_ms{120000};
    };

    struct Sample {
        u64 now_ms{};
        u64 headroom{};
        u64 stall_us{}; ///< Permafrost's fault stall, cumulative
        u64 texture_heap{};
        u64 texture_floor{};
        u64 texture_cap{}; ///< the cap the textures last ran with
    };

    struct Plan {
        /// Permafrost: samples below watch, writes below low (headroom plus the loan) up to high,
        /// is urgent below urgent and in an emergency below emergency.
        u64 watch{}, low{}, high{}, urgent{}, emergency{};
        /// JIT caches: stop growing, give back, grow again.
        u64 jit_stop{}, jit_release{}, jit_uncap{};
        /// The textures grow into the headroom above it; 0 holds them at their floor.
        u64 texture_line{};
        /// Texture bytes above their floor the coordinator lent: Permafrost's headroom too.
        u64 texture_loan{};
        u64 relief{};
        u64 stall_us_per_s{};
        bool yield{};
    };

    explicit MemoryCoordinator(const Config& config_) : config{config_} {}

    /// The plan with nothing measured yet: the marks as configured, the textures free to grow.
    [[nodiscard]] Plan Initial() const {
        return Compute(Sample{}, true);
    }

    Plan Update(const Sample& s) {
        if (!started) {
            started = true;
            last_ms = s.now_ms;
            last_stall_us = s.stall_us;
            return Compute(s, false);
        }
        const u64 dt = s.now_ms > last_ms ? s.now_ms - last_ms : 0;
        if (dt == 0) {
            return Compute(s, false);
        }
        const u64 stalled = s.stall_us > last_stall_us ? s.stall_us - last_stall_us : 0;
        last_ms = s.now_ms;
        last_stall_us = s.stall_us;
        const double rate = static_cast<double>(stalled) * 1000.0 / static_cast<double>(dt);
        stall += (rate - stall) * static_cast<double>(dt) /
                 static_cast<double>(config.stall_tau_ms + dt);

        const bool stalling = stall >= static_cast<double>(config.stall_on_us_per_s);
        if (!stalling) {
            stalling_since.reset();
        } else if (!stalling_since) {
            stalling_since = s.now_ms;
        }
        if (!yield && stalling) {
            yield = true;
            if (relief != 0) {
                // It came back while the relief eased: that much was too little.
                relief_floor = std::min(MaxRelief(), relief + config.relief_step);
                relief_floor_until = s.now_ms + config.relief_floor_ms;
            }
        } else if (yield && stall <= static_cast<double>(config.stall_off_us_per_s)) {
            yield = false;
        }
        if (yield) {
            hold_until = s.now_ms + config.hold_ms;
            // Only while it still stalls: the yield outlasts the stall by its hysteresis, and a
            // load burst that is over must not bring the marks down.
            if (stalling_since && s.now_ms - *stalling_since >= config.grace_ms) {
                relief = std::min(MaxRelief(), relief + config.relief_up * dt / 1000);
            }
        } else if (s.now_ms >= hold_until) {
            const u64 floor = s.now_ms < relief_floor_until ? relief_floor : 0;
            const u64 down = config.relief_down * dt / 1000;
            relief = std::max(floor, relief > down ? relief - down : 0);
        }
        return Compute(s, false);
    }

private:
    [[nodiscard]] u64 MaxRelief() const {
        return config.low > config.low_floor ? config.low - config.low_floor : 0;
    }

    [[nodiscard]] Plan Compute(const Sample& s, bool initial) const {
        Plan p;
        p.relief = std::min(relief, MaxRelief());
        p.low = config.low - p.relief;
        p.high = std::max(config.high > p.relief ? config.high - p.relief : 0, p.low);
        p.watch = std::max(config.watch > p.relief ? config.watch - p.relief : 0, p.high);
        p.urgent = std::min(std::max(p.low / 2, 384 * MiB), p.low);
        p.emergency = std::min(std::max(p.low / 4, 192 * MiB), p.urgent);
        // As before: the JIT stops growing where Permafrost turns urgent, gives back below its
        // emergency mark and grows again at its low mark.
        p.jit_stop = p.urgent;
        p.jit_release = std::max(p.low * 3 / 16, 160 * MiB);
        p.jit_uncap = std::max(p.low, 1024 * MiB);
        p.yield = yield;
        p.stall_us_per_s = static_cast<u64>(stall);
        const bool grow = initial || (!yield && p.relief == 0 && s.now_ms >= hold_until);
        p.texture_line = grow ? config.texture_line.value_or(p.watch + config.texture_margin) : 0;
        if (p.texture_line != 0 && s.texture_cap > s.texture_floor &&
            s.texture_heap > s.texture_floor) {
            p.texture_loan = std::min(s.texture_heap, s.texture_cap) - s.texture_floor;
        }
        return p;
    }

    Config config;
    bool started{};
    u64 last_ms{}, last_stall_us{};
    double stall{}; ///< us of guest stall per second, smoothed
    bool yield{};
    std::optional<u64> stalling_since;
    u64 hold_until{};
    u64 relief{}, relief_floor{}, relief_floor_until{};
};

} // namespace Jadyx
