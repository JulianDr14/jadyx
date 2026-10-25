// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The screen between the library and the game's first frame: the app's splash image with the
// hexagon indicator under it, a snake crawling round its track. Three presenters show it in turn
// on one clock, so the window never goes black (or blue) and the snake never jumps:
//   1. the frontend, on a D3D12 swapchain of its own, from the library's exit until the renderer
//      takes the window (ShowLaunchSplash / ReleaseLaunchSplash);
//   2. the renderer, from its creation until the guest is about to run (LaunchSplashDrawer, for
//      D3D12::SetBootScreen), with the shader cache and JIT progress under the indicator;
//   3. LaunchScreen, over the guest until the game presents.

#include <cstddef>
#include <memory>
#include <utility>

#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace Core {
class System;
}

namespace Jadyx {

/// Starts the splash on `core_window` (the library has released it) and restarts its clock.
/// UI thread.
void ShowLaunchSplash(void* core_window, unsigned width, unsigned height);

/// Stops the frontend's splash and lets the window go; nothing happens when none is shown. Any
/// thread (the renderer's handoff calls it from the boot thread).
void ReleaseLaunchSplash();

/// The splash as the renderer draws it (screen size in, centered image out). Any thread.
[[nodiscard]] D3D12::MenuImage DrawLaunchSplash(std::pair<u32, u32> screen);

/// What the boot is doing, written under the indicator.
enum class LaunchStep { Loading, Shaders, Cpu, Starting };

/// The step under the indicator, with its bar when `total` is known (a LoadDiskResources or JIT
/// prewarm callback passes its count). Any thread.
void SetLaunchStep(LaunchStep step, std::size_t done = 0, std::size_t total = 0);

/// The splash over the guest's first moments, until the game has presented a few frames. The
/// system's GPU thread must be running, and stay so until the screen is destroyed.
class LaunchScreen {
public:
    explicit LaunchScreen(Core::System& system);
    /// Stops drawing and removes the splash (the game's frame shows again).
    ~LaunchScreen();
    LaunchScreen(const LaunchScreen&) = delete;
    LaunchScreen& operator=(const LaunchScreen&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Jadyx
