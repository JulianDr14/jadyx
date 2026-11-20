// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace VideoCore {
// Frontend preference shared with the render thread; disabled until settings are loaded.
inline std::atomic<bool> performance_overlay_enabled{false};

/// The HUD's lines, one bit each; Network is every line the frontend adds (online play).
namespace OverlayItem {
inline constexpr std::uint32_t Fps = 1u << 0, FrameTime = 1u << 1, Cpu = 1u << 2, Gpu = 1u << 3,
                               Memory = 1u << 4, Hitch = 1u << 5, Network = 1u << 6;
inline constexpr std::uint32_t OwnLines = Fps | FrameTime | Cpu | Gpu | Memory | Hitch;
inline constexpr std::uint32_t All = OwnLines | Network;
} // namespace OverlayItem
/// Which lines the HUD draws (OverlayItem bits).
inline std::atomic<std::uint32_t> performance_overlay_items{OverlayItem::All};
/// Text size in steps from the normal one: -1 smaller, +1 and +2 larger.
inline std::atomic<int> performance_overlay_size{0};

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
