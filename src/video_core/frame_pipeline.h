// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

/// Switches of the frame chain (game -> GPU thread -> composite -> vsync -> display), read on
/// every frame so a boot.cfg key takes effect at once. Each one has a check mode: the old path
/// runs, and what the new one would have done is counted (VideoCore::Perf) so a package can show
/// it is safe before it is turned on.
namespace VideoCore::FramePipeline {

enum class Mode : u8 {
    Off,
    On,
    Check,
};

/// boot.cfg "present_mailbox=": the GPU thread never waits for a free present frame. With every
/// frame taken it reuses the oldest one still queued (that frame is never shown), and the frame's
/// previous copy is waited for on the GPU instead of the CPU.
inline std::atomic<Mode> present_mailbox{Mode::Off};

/// boot.cfg "vsync_nowait=": the VSyncThread waits at most 2 ms for the previous composite, so
/// the guest's vsync event never comes late because the GPU thread is late. At most three
/// composites are ever in flight.
inline std::atomic<Mode> vsync_nowait{Mode::Off};

/// boot.cfg "swap_interval_hold=": a game with a dynamic vsync that asked for interval 2 in the last
/// two seconds gets its buffer back after 2 vsyncs even if it asks for 3. A game that keeps asking
/// for 3 gets it once the two seconds pass.
inline std::atomic<Mode> swap_interval_hold{Mode::Off};

inline void SetPresentMailbox(Mode mode) {
    present_mailbox.store(mode, std::memory_order_relaxed);
}

[[nodiscard]] inline Mode PresentMailbox() {
    return present_mailbox.load(std::memory_order_relaxed);
}

inline void SetVsyncNowait(Mode mode) {
    vsync_nowait.store(mode, std::memory_order_relaxed);
}

[[nodiscard]] inline Mode VsyncNowait() {
    return vsync_nowait.load(std::memory_order_relaxed);
}

inline void SetSwapIntervalHold(Mode mode) {
    swap_interval_hold.store(mode, std::memory_order_relaxed);
}

[[nodiscard]] inline Mode SwapIntervalHold() {
    return swap_interval_hold.load(std::memory_order_relaxed);
}

} // namespace VideoCore::FramePipeline
