// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace VideoCore {
// Frontend preference shared with the render thread; disabled until settings are loaded.
inline std::atomic<bool> performance_overlay_enabled{false};

/// One more line of the performance HUD, from the frontend. Only the HUD font's characters show
/// (digits, A-Z and / . % - : < >); `warning` draws it in amber.
struct OverlayLine {
    std::string text;
    bool warning{};
};
/// Asked for the frontend's lines (online play) at each HUD refresh, on the render thread.
using OverlayLinesSource = std::vector<OverlayLine> (*)();
inline std::atomic<OverlayLinesSource> overlay_extra_lines{nullptr};
} // namespace VideoCore
