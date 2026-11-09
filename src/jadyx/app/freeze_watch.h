// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Freeze watch: the play loop asks once per second when the game last drew a frame. Past
// FIRST_REPORT_S without one (and with the guest running) it writes to the diag what each thread
// is busy with (Common::StallWatch: which pipeline, in which stage, for how long) and, at the
// first and third report, every named thread's stack. Then again at longer marks, and once when
// the game draws again. Separately, any single piece of work that runs past LONG_WORK_S is
// reported once, frozen or not. Costs nothing while frames come.

#include "common/common_types.h"

namespace Jadyx {

class FreezeWatch {
public:
    static constexpr u64 FIRST_REPORT_S = 5;
    static constexpr u64 LONG_WORK_S = 10;

    /// At the start of a session: frames of an earlier game in the process do not count.
    FreezeWatch();

    /// Every play loop tick; checks once per second. `paused`: the menu is open or the guest is
    /// paused (Pulse), so no frames are expected.
    void Tick(bool paused);

private:
    void Check(u64 now_ms, bool paused);

    /// Steady ns when the session started; earlier frames belong to another game.
    u64 created_ns{};
    u64 next_check_ms{};
    u64 last_check_ms{};
    /// Frames are not expected before this (steady ns): the end of a pause or a suspension.
    u64 expect_since_ns{};
    /// Reports written for the freeze in progress; 0 when none is.
    u32 reports{};
    /// When the freeze in progress started (steady ns): the last frame or the end of a pause.
    u64 freeze_since_ns{};
    /// Start of the long work already reported, so it is reported once.
    u64 long_work_reported_ms{};
};

} // namespace Jadyx
