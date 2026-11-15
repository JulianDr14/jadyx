// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings > Controller's sections: the main page keeps the basics and opens one page each for the
// right stick, the mouse, the rumble and the gyro. The library session and its canvas share the
// rows' order from here.

#pragma once

#include "jadyx/input/uwp_input.h"

namespace Jadyx {

/// The main page's first section entry; rows before it are the basics (device ... mapping).
inline constexpr unsigned ControllerSectionEntry = 6;
/// The sections, numbered from 1 (0 is the main page), in their entries' order.
enum ControllerSection : unsigned { SectionRightStick = 1, SectionMouse, SectionRumble, SectionGyro };
inline constexpr unsigned ControllerSectionCount = 4;

/// One row of a section.
enum class ControllerSettingRow {
    InvertX, InvertY, StickSensitivity,  // Right stick.
    MouseUse, MouseSensitivity,          // Mouse.
    Rumble, RumbleStrength,              // Rumble.
    GyroSource, GyroShake,               // Gyro (and the mouse's sensitivity while it moves it).
};

/// The rows `section` shows now: the gyro's add the mouse's sensitivity while the mouse moves it.
[[nodiscard]] inline unsigned ControllerSectionRows(unsigned section, const ControllerOptions& options) {
    switch (section) {
    case SectionRightStick: return 3;
    case SectionMouse: return 2;
    case SectionRumble: return 2;
    default: return options.motion_source == MotionSource::Mouse ? 3 : 2;
    }
}

/// What `row` of `section` sets (the gyro's third row is the mouse's sensitivity).
[[nodiscard]] inline ControllerSettingRow ControllerSectionRow(unsigned section, unsigned row) {
    using R = ControllerSettingRow;
    switch (section) {
    case SectionRightStick: return row == 0 ? R::InvertX : row == 1 ? R::InvertY : R::StickSensitivity;
    case SectionMouse: return row == 0 ? R::MouseUse : R::MouseSensitivity;
    case SectionRumble: return row == 0 ? R::Rumble : R::RumbleStrength;
    default: return row == 0 ? R::GyroSource : row == 1 ? R::GyroShake : R::MouseSensitivity;
    }
}

} // namespace Jadyx
