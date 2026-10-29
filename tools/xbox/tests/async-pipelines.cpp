// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Async pipelines gate: which draws with a pipeline still building wait for it, per mode and
// small-draw threshold, and the counts the stats line reports.
#include "video_core/renderer_d3d12/d3d12_async_pipelines.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace D3D12;
namespace {
void Expect(AsyncPipelines mode, u32 small_draws, PendingDraw draw, PendingDrawAction expected,
            const char* what) {
    const PendingDrawAction got = DecidePendingDraw(mode, small_draws, draw);
    if (got != expected) {
        throw std::runtime_error(std::string{what} + ": got " +
                                 std::to_string(static_cast<int>(got)) + ", expected " +
                                 std::to_string(static_cast<int>(expected)));
    }
}
} // namespace
int main() try {
    using enum PendingDrawAction;
    constexpr PendingDraw mesh{.vertices = 3000, .instances = 1, .indexed = true, .new_target = false};
    constexpr PendingDraw mesh_new{.vertices = 3000, .instances = 1, .indexed = true, .new_target = true};
    constexpr PendingDraw triangle{.vertices = 3, .instances = 1, .indexed = false, .new_target = false};
    constexpr PendingDraw quad{.vertices = 4, .instances = 1, .indexed = false, .new_target = false};
    constexpr PendingDraw quads{.vertices = 4, .instances = 50, .indexed = false, .new_target = false};
    constexpr PendingDraw indexed_quad{.vertices = 6, .instances = 1, .indexed = true, .new_target = false};
    constexpr PendingDraw sprite{.vertices = 6, .instances = 1, .indexed = false, .new_target = false};
    // Indirect draws: their counts live in a GPU buffer.
    constexpr PendingDraw indirect{.vertices = UINT32_MAX, .instances = UINT32_MAX, .indexed = false, .new_target = false};
    constexpr PendingDraw indirect_new{.vertices = UINT32_MAX, .instances = UINT32_MAX, .indexed = true, .new_target = true};

    // Off never reaches the decision in the renderer, but skips like Full without a threshold.
    Expect(AsyncPipelines::Balanced, 0, mesh, Skip, "balanced mesh");
    Expect(AsyncPipelines::Balanced, 0, mesh_new, WaitNewTarget, "balanced first draw to target");
    Expect(AsyncPipelines::Balanced, 0, triangle, WaitFullScreen, "balanced full-screen triangle");
    Expect(AsyncPipelines::Balanced, 0, quad, WaitFullScreen, "balanced full-screen quad");
    Expect(AsyncPipelines::Balanced, 0, quads, Skip, "balanced instanced quads");
    Expect(AsyncPipelines::Balanced, 0, indexed_quad, Skip, "balanced indexed quad");
    Expect(AsyncPipelines::Balanced, 0, sprite, Skip, "balanced 6-vertex list");
    Expect(AsyncPipelines::Full, 0, mesh_new, Skip, "full first draw to target");
    Expect(AsyncPipelines::Full, 0, triangle, Skip, "full full-screen triangle");
    Expect(AsyncPipelines::Full, 6, sprite, WaitSmall, "full under threshold");
    Expect(AsyncPipelines::Full, 6, indexed_quad, WaitSmall, "full indexed at threshold");
    Expect(AsyncPipelines::Full, 6, mesh, Skip, "full over threshold");
    Expect(AsyncPipelines::Balanced, 6, triangle, WaitSmall, "threshold before full-screen");
    Expect(AsyncPipelines::Balanced, 2999, mesh_new, WaitNewTarget, "over threshold, new target");
    Expect(AsyncPipelines::Balanced, 3000, mesh, WaitSmall, "threshold inclusive");
    Expect(AsyncPipelines::Full, 3000, indirect, Skip, "full indirect over any threshold");
    Expect(AsyncPipelines::Balanced, 4, indirect, Skip, "balanced indirect is not full-screen");
    Expect(AsyncPipelines::Balanced, 0, indirect_new, WaitNewTarget, "balanced indirect, new target");

    // Settings round trip.
    if (GetAsyncPipelines() != AsyncPipelines::Off || GetWaitSmallDraws() != 0)
        throw std::runtime_error("defaults are not off");
    SetAsyncPipelines(AsyncPipelines::Balanced);
    SetWaitSmallDraws(12);
    if (GetAsyncPipelines() != AsyncPipelines::Balanced || GetWaitSmallDraws() != 12)
        throw std::runtime_error("settings not kept");
    if (GetPipelineWorkers() != 0) throw std::runtime_error("pipeline workers not default");
    SetPipelineWorkers(6);
    if (GetPipelineWorkers() != 6) throw std::runtime_error("pipeline workers not kept");

    // Stats.
    for (int i = 0; i < 5; ++i) CountPendingDraw(Skip);
    CountPendingDraw(WaitSmall);
    CountPendingDraw(WaitFullScreen);
    CountPendingDraw(WaitFullScreen);
    CountPendingDraw(WaitNewTarget);
    const PendingDrawStats stats = GetPendingDrawStats();
    if (stats.skipped != 5 || stats.waited_small != 1 || stats.waited_full_screen != 2 ||
        stats.waited_new_target != 1)
        throw std::runtime_error("stats miscounted");
    std::cout << "PASS async pipelines: 18 decisions (balanced, full, small-draw threshold), "
                 "settings and stats\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
