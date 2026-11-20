// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the performance HUD shows: a profile (everything, FPS + GPU + RAM, FPS alone, or the
// player's own lines), the online lines on their own switch and the text size. Pure logic plus
// the publish to the render thread (video_core/performance_overlay.h).

#include <cstdint>
#include <vector>

#include "video_core/performance_overlay.h"

namespace Jadyx {

enum class StatsProfile : std::uint8_t { Full, Standard, Minimal, Custom, Count };
enum class StatsTextSize : std::uint8_t { Small, Normal, Large, Huge, Count };
/// The HUD's own lines, in its order; each is bit (1 << item) of the overlay mask.
enum class StatsItem : std::uint8_t { Fps, FrameTime, Cpu, Gpu, Memory, Hitch, Count };

namespace OverlayItem = VideoCore::OverlayItem;
static_assert(OverlayItem::Fps == 1u << static_cast<unsigned>(StatsItem::Fps));
static_assert(OverlayItem::FrameTime == 1u << static_cast<unsigned>(StatsItem::FrameTime));
static_assert(OverlayItem::Cpu == 1u << static_cast<unsigned>(StatsItem::Cpu));
static_assert(OverlayItem::Gpu == 1u << static_cast<unsigned>(StatsItem::Gpu));
static_assert(OverlayItem::Memory == 1u << static_cast<unsigned>(StatsItem::Memory));
static_assert(OverlayItem::Hitch == 1u << static_cast<unsigned>(StatsItem::Hitch));

constexpr std::uint32_t StatsItemBit(StatsItem item) {
    return 1u << static_cast<unsigned>(item);
}

struct StatsOptions {
    StatsProfile profile{StatsProfile::Full};
    std::uint32_t custom_items{OverlayItem::OwnLines}; ///< The Custom profile's lines.
    bool network{true};                                ///< Online play's ping, loss and traffic.
    StatsTextSize text_size{StatsTextSize::Normal};

    bool operator==(const StatsOptions&) const = default;
};

/// The HUD's own lines a profile shows.
constexpr std::uint32_t ProfileItems(StatsProfile profile, std::uint32_t custom_items) {
    switch (profile) {
    case StatsProfile::Standard: return OverlayItem::Fps | OverlayItem::Gpu | OverlayItem::Memory;
    case StatsProfile::Minimal:  return OverlayItem::Fps;
    case StatsProfile::Custom:   return custom_items & OverlayItem::OwnLines;
    default:                     return OverlayItem::OwnLines;
    }
}

/// Everything the render thread draws: the profile's lines and, when on, the online ones.
constexpr std::uint32_t OverlayMask(const StatsOptions& options) {
    return ProfileItems(options.profile, options.custom_items) | (options.network ? OverlayItem::Network : 0u);
}

/// Steps of the HUD cell from its normal size (-1 small .. +2 huge).
constexpr int TextSizeStep(StatsTextSize size) {
    return static_cast<int>(size) - static_cast<int>(StatsTextSize::Normal);
}

/// Rows of Settings > System > Performance statistics; the line switches only in Custom.
enum class StatsRow : std::uint8_t { Show, Profile, Network, TextSize, Fps, FrameTime, Cpu, Gpu, Memory, Hitch };

inline std::vector<StatsRow> StatsRows(const StatsOptions& options) {
    std::vector<StatsRow> rows{StatsRow::Show, StatsRow::Profile, StatsRow::Network, StatsRow::TextSize};
    if (options.profile == StatsProfile::Custom)
        for (auto row = static_cast<unsigned>(StatsRow::Fps); row <= static_cast<unsigned>(StatsRow::Hitch); ++row)
            rows.push_back(static_cast<StatsRow>(row));
    return rows;
}

/// The HUD line a Custom row switches.
constexpr StatsItem RowItem(StatsRow row) {
    return static_cast<StatsItem>(static_cast<unsigned>(row) - static_cast<unsigned>(StatsRow::Fps));
}

template <typename E>
constexpr E StepStatsEnum(E value, bool back) {
    constexpr auto count = static_cast<unsigned>(E::Count);
    const auto index = static_cast<unsigned>(value) % count;
    return static_cast<E>(back ? (index + count - 1) % count : (index + 1) % count);
}

/// Loaded values out of range fall back to the defaults.
inline StatsOptions SanitizeStatsOptions(StatsOptions options) {
    if (static_cast<unsigned>(options.profile) >= static_cast<unsigned>(StatsProfile::Count))
        options.profile = StatsProfile::Full;
    if (static_cast<unsigned>(options.text_size) >= static_cast<unsigned>(StatsTextSize::Count))
        options.text_size = StatsTextSize::Normal;
    options.custom_items &= OverlayItem::OwnLines;
    return options;
}

/// Hands the choice to the render thread; the HUD picks it up at its next refresh.
inline void PublishStatsOverlay(bool shown, const StatsOptions& options) {
    VideoCore::performance_overlay_items.store(OverlayMask(options), std::memory_order_relaxed);
    VideoCore::performance_overlay_size.store(TextSizeStep(options.text_size), std::memory_order_relaxed);
    VideoCore::performance_overlay_enabled.store(shown, std::memory_order_relaxed);
}

} // namespace Jadyx
