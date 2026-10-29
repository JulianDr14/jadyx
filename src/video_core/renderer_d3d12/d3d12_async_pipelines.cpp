// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_async_pipelines.h"

#include <atomic>

namespace D3D12 {

namespace {
std::atomic<AsyncPipelines> async_pipelines{AsyncPipelines::Off};
std::atomic<u32> wait_small_draws{0};
std::atomic<u32> pipeline_workers{0};
// Read by the stats line on the same GPU thread that counts.
PendingDrawStats pending_draw_stats{};

// A full-screen pass: a triangle or a quad strip, drawn once.
constexpr u32 FULL_SCREEN_MAX_VERTICES = 4;
} // namespace

void SetAsyncPipelines(AsyncPipelines mode) {
    async_pipelines.store(mode, std::memory_order_relaxed);
}

AsyncPipelines GetAsyncPipelines() noexcept {
    return async_pipelines.load(std::memory_order_relaxed);
}

void SetWaitSmallDraws(u32 vertices) {
    wait_small_draws.store(vertices, std::memory_order_relaxed);
}

u32 GetWaitSmallDraws() noexcept {
    return wait_small_draws.load(std::memory_order_relaxed);
}

void SetPipelineWorkers(u32 count) {
    pipeline_workers.store(count, std::memory_order_relaxed);
}

u32 GetPipelineWorkers() noexcept {
    return pipeline_workers.load(std::memory_order_relaxed);
}

PendingDrawAction DecidePendingDraw(AsyncPipelines mode, u32 small_draws,
                                    const PendingDraw& draw) noexcept {
    if (small_draws != 0 && draw.vertices <= small_draws) {
        return PendingDrawAction::WaitSmall;
    }
    if (mode == AsyncPipelines::Balanced) {
        // Passes over the whole target (post-processing, resolves, copies) run once a frame and
        // what they leave out stays wrong until the next frame; a target's first draws are what
        // later passes sample.
        if (!draw.indexed && draw.instances == 1 && draw.vertices <= FULL_SCREEN_MAX_VERTICES) {
            return PendingDrawAction::WaitFullScreen;
        }
        if (draw.new_target) {
            return PendingDrawAction::WaitNewTarget;
        }
    }
    return PendingDrawAction::Skip;
}

void CountPendingDraw(PendingDrawAction action) noexcept {
    switch (action) {
    case PendingDrawAction::Skip:
        ++pending_draw_stats.skipped;
        break;
    case PendingDrawAction::WaitSmall:
        ++pending_draw_stats.waited_small;
        break;
    case PendingDrawAction::WaitFullScreen:
        ++pending_draw_stats.waited_full_screen;
        break;
    case PendingDrawAction::WaitNewTarget:
        ++pending_draw_stats.waited_new_target;
        break;
    }
}

PendingDrawStats GetPendingDrawStats() noexcept {
    return pending_draw_stats;
}

} // namespace D3D12
