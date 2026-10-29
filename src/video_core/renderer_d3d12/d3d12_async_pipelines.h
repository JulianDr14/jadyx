// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace D3D12 {

/// What a draw does while its pipeline is still being built on a worker ("async_pipelines=").
enum class AsyncPipelines : u8 {
    Off,      ///< Every draw waits for its pipeline: nothing is lost.
    Balanced, ///< Draws are skipped, except full-screen passes and the first draws to a target.
    Full,     ///< Draws are skipped; only those under the small-draw threshold wait.
};

void SetAsyncPipelines(AsyncPipelines mode);
[[nodiscard]] AsyncPipelines GetAsyncPipelines() noexcept;

/// Draws of this many vertices or fewer wait for their pipeline in any mode; 0 turns it off
/// ("wait_small_draws=").
void SetWaitSmallDraws(u32 vertices);
[[nodiscard]] u32 GetWaitSmallDraws() noexcept;

/// Pipeline build workers ("pso_workers=", 1-8); 0 keeps the default. Read when the pipeline
/// cache is made.
void SetPipelineWorkers(u32 count);
[[nodiscard]] u32 GetPipelineWorkers() noexcept;

/// A draw whose pipeline is still being built.
struct PendingDraw {
    u32 vertices;
    u32 instances;
    bool indexed;
    /// Some color or depth target of the draw was never drawn to: its first content.
    bool new_target;
};

enum class PendingDrawAction : u8 {
    Skip,
    WaitSmall,
    WaitFullScreen,
    WaitNewTarget,
};

[[nodiscard]] PendingDrawAction DecidePendingDraw(AsyncPipelines mode, u32 wait_small_draws,
                                                  const PendingDraw& draw) noexcept;

/// GPU thread: counts what draws with pending pipelines did, for the periodic stats line.
void CountPendingDraw(PendingDrawAction action) noexcept;

struct PendingDrawStats {
    u64 skipped;
    u64 waited_small;
    u64 waited_full_screen;
    u64 waited_new_target;
};
[[nodiscard]] PendingDrawStats GetPendingDrawStats() noexcept;

} // namespace D3D12
