// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The motion view: a small 3D controller in a corner of the screen that leans, turns and shakes
// with the emulated gyro (motion_emu.h), shakes harder while the game rumbles, and shows the
// player's sticks and buttons. The model follows the carbon black Series controller; the card
// around it follows the frontend's palette. Drawn on the CPU (smooth-shaded triangles, z-buffer,
// 2x supersampling) and handed to the renderer as an overlay image.

#pragma once

#include <utility>

#include "common/common_types.h"
#include "jadyx/input/motion_emu.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace Jadyx {

/// The panel for a screen of `screen` pixels, placed in its bottom-left corner. `seconds` drives
/// the rumble's jitter.
[[nodiscard]] D3D12::MenuImage DrawMotionView(const MotionViewState& state, std::pair<u32, u32> screen,
                                              float seconds);

} // namespace Jadyx
