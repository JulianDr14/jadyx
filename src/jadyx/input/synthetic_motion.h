// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jadyx/input/controller_style.h"
#include "jadyx/input/menu_input_capture.h"
#include "jadyx/input/stick_motion.h"

namespace Jadyx {
// Player-one logical input, before style rotation. Modal ownership always wins.
class SyntheticMotion {
public:
    // Called after the menu combo sees raw +/- but before navigation reads the pad.
    // Preserve raw navigation while a modal owns input, and retain consumed holds
    // after closing. The caller's final modal filter releases gameplay input.
    StickMotionSample FilterCaptured(StylePad& pad, bool enabled, bool sensor_requested, bool modal,
                                     bool& minus, StickMotion::Clock::time_point now) {
        if (!enabled)
            return {};
        auto captured = pad;
        capture.Filter(captured, modal);
        const bool plus = (pad.buttons & (1U << PadBit::Plus)) != 0;
        const auto sample = Filter(captured, enabled, sensor_requested, modal || plus, now);
        if (sensor_requested)
            minus = (captured.buttons & (1U << PadBit::Minus)) != 0;
        if (!modal)
            pad = captured;
        return sample;
    }

    StickMotionSample Filter(StylePad& pad, bool enabled, bool sensor_requested, bool blocked,
                             StickMotion::Clock::time_point now) {
        const bool view = (pad.buttons & (1U << PadBit::Minus)) != 0;
        const bool l3 = (pad.buttons & (1U << PadBit::LStick)) != 0;
        const auto stick_sample =
            stick.Update(enabled && sensor_requested, l3, pad.left_x, pad.left_y, pad.right_x,
                         pad.right_y, now, blocked || view);
        const auto view_sample = view_stick.Update(enabled && sensor_requested, view, 0, 0,
                                                   pad.right_x, pad.right_y, now, blocked);
        if (!enabled || !sensor_requested)
            return {};
        pad.buttons &= ~((1U << PadBit::LStick) | (1U << PadBit::Minus));
        if (stick_sample.guest_l3)
            pad.buttons |= 1U << PadBit::LStick;
        if (view_sample.guest_l3)
            pad.buttons |= 1U << PadBit::Minus;
        if (stick_sample.suppress_sticks) {
            pad.left_x = pad.left_y = pad.right_x = pad.right_y = 0;
            return stick_sample;
        }
        if (view_sample.suppress_sticks) {
            pad.right_x = pad.right_y = 0;
            return view_sample;
        }
        return stick_sample;
    }

private:
    MenuInputCapture capture;
    StickMotion stick;
    StickMotion view_stick;
};
} // namespace Jadyx
