// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/frame_pipeline.h"

namespace D3D12 {

/// What a draw does while its pipeline is still being built on a worker ("async_pipelines=").
enum class AsyncPipelines : u8 {
    Off,      ///< Every draw waits for its pipeline: nothing is lost.
    Balanced, ///< Draws are skipped, except full-screen passes and the first draws to a target.
    Full,     ///< Draws are skipped; only those under the small-draw threshold wait.
};

void SetAsyncPipelines(AsyncPipelines mode);
[[nodiscard]] AsyncPipelines GetAsyncPipelines() noexcept;

/// "pso_budget=0|1|check" (VideoCore::FramePipeline::Mode), in Balanced mode only: draws with a
/// pending pipeline wait for it until the GPU thread has waited PSO_WAIT_BUDGET_US for pipelines
/// since the last composite, cutting the wait that spends it; past that, Balanced's rules apply.
/// Off always waits and Full never does, so each mode keeps the meaning the user chose. Check:
/// the mode alone decides, and draws the budget would decide otherwise are counted.
void SetPsoBudget(VideoCore::FramePipeline::Mode mode);
[[nodiscard]] VideoCore::FramePipeline::Mode GetPsoBudget() noexcept;
constexpr u64 PSO_WAIT_BUDGET_US = 6000;

/// "pso_loading=0|1|check" (VideoCore::FramePipeline::Mode): on a loading screen
/// (LoadingDetector) the background prewarm builds back to back, and the pipelines the game drew
/// in earlier sessions whose PSOs were released while idle are built again. Check: loading screens
/// are detected and logged with what would be rebuilt, and nothing changes.
void SetPsoLoading(VideoCore::FramePipeline::Mode mode);
[[nodiscard]] VideoCore::FramePipeline::Mode GetPsoLoading() noexcept;

/// GPU thread: pipeline waits since the last composite, against PSO_WAIT_BUDGET_US.
void AddPendingWait(u64 us) noexcept;
void ResetPendingWaitBudget() noexcept;
[[nodiscard]] bool PendingWaitBudgetLeft() noexcept;
[[nodiscard]] u64 PendingWaitBudgetRemaining() noexcept;

/// Draws of this many vertices or fewer wait for their pipeline in any mode; 0 turns it off
/// ("wait_small_draws=").
void SetWaitSmallDraws(u32 vertices);
[[nodiscard]] u32 GetWaitSmallDraws() noexcept;

/// Pipeline build workers ("pso_workers=", 1-8); 0 keeps the default. Read when the pipeline
/// cache is made.
void SetPipelineWorkers(u32 count);
[[nodiscard]] u32 GetPipelineWorkers() noexcept;

/// Workers that build the disk cache's pipelines on the loading screen ("pso_boot_workers=",
/// 1-12); 0 keeps the default of one per logical CPU. Read when the disk cache is loaded.
void SetPipelineBootWorkers(u32 count);
[[nodiscard]] u32 GetPipelineBootWorkers() noexcept;

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
    WaitBudget, ///< pso_budget: the frame's wait budget is not spent yet (or the mode is Off)
};

[[nodiscard]] PendingDrawAction DecidePendingDraw(AsyncPipelines mode, u32 wait_small_draws,
                                                  const PendingDraw& draw) noexcept;

/// Balanced with pso_budget on: wait while budget is left, then Balanced's rules.
[[nodiscard]] PendingDrawAction DecideBudgetedDraw(u32 wait_small_draws, const PendingDraw& draw,
                                                   bool budget_left) noexcept;

/// GPU thread: counts what draws with pending pipelines did, for the periodic stats line.
void CountPendingDraw(PendingDrawAction action) noexcept;
/// pso_budget=check: a draw the budget would have skipped (would_skip) or waited for.
void CountBudgetDifference(bool would_skip) noexcept;

struct PendingDrawStats {
    u64 skipped;
    u64 waited_small;
    u64 waited_full_screen;
    u64 waited_new_target;
    u64 waited_budget;
    u64 budget_would_skip;
    u64 budget_would_wait;
};
[[nodiscard]] PendingDrawStats GetPendingDrawStats() noexcept;

} // namespace D3D12
