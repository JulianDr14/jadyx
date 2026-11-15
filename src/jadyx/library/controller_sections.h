// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings > Controller's sections: the main page keeps the basics and opens one page each for the
// right stick, the mouse, the rumble, the gyro and the DSU controllers. The library session and its
// canvas share the rows' order from here.

#pragma once

#include <cstddef>

#include "jadyx/input/uwp_input.h"

namespace Jadyx {

/// The main page's first section entry; rows before it are the basics (device ... mapping).
inline constexpr unsigned ControllerSectionEntry = 6;
/// The sections, numbered from 1 (0 is the main page), in their entries' order.
enum ControllerSection : unsigned { SectionRightStick = 1, SectionMouse, SectionRumble, SectionGyro, SectionDsu };
inline constexpr unsigned ControllerSectionCount = 5;

/// One row of a section.
enum class ControllerSettingRow {
    InvertX, InvertY, StickSensitivity,  // Right stick.
    MouseUse, MouseSensitivity,          // Mouse.
    Rumble, RumbleStrength,              // Rumble.
    GyroSource, GyroShake,               // Gyro (and the mouse's sensitivity while it moves it).
    GyroDsuPad,                          // Gyro while a DSU pad moves it: which pad.
    DsuSearch, DsuAdd, DsuEntry,         // DSU controllers: search, add, then a server or a pad each.
};

/// The rows `section` shows now: the gyro's add the mouse's sensitivity while the mouse moves it,
/// and show the DSU pad instead of the shake button while a DSU pad does. `dsu_rows`: the DSU
/// page's server and pad rows (DsuRows), after its Search and Add.
[[nodiscard]] inline unsigned ControllerSectionRows(unsigned section, const ControllerOptions& options,
                                                    std::size_t dsu_rows = 0) {
    switch (section) {
    case SectionRightStick: return 3;
    case SectionMouse: return 2;
    case SectionRumble: return 2;
    case SectionDsu: return 2 + static_cast<unsigned>(dsu_rows);
    default: return options.motion_source == MotionSource::Mouse ? 3 : 2;
    }
}

/// What `row` of `section` sets (the gyro's third row is the mouse's sensitivity; the DSU page's
/// rows from the third on are DsuRows' entry `row - 2`).
[[nodiscard]] inline ControllerSettingRow ControllerSectionRow(unsigned section, unsigned row,
                                                               const ControllerOptions& options) {
    using R = ControllerSettingRow;
    switch (section) {
    case SectionRightStick: return row == 0 ? R::InvertX : row == 1 ? R::InvertY : R::StickSensitivity;
    case SectionMouse: return row == 0 ? R::MouseUse : R::MouseSensitivity;
    case SectionRumble: return row == 0 ? R::Rumble : R::RumbleStrength;
    case SectionDsu: return row == 0 ? R::DsuSearch : row == 1 ? R::DsuAdd : R::DsuEntry;
    default:
        if (options.motion_source == MotionSource::Phone) return row == 0 ? R::GyroSource : R::GyroDsuPad;
        return row == 0 ? R::GyroSource : row == 1 ? R::GyroShake : R::MouseSensitivity;
    }
}

} // namespace Jadyx
