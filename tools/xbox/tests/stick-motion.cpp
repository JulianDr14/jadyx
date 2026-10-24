// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>
#include "hid_core/frontend/six_axis_requests.h"
#include "jadyx/game/game_menu.h"
#include "jadyx/input/synthetic_motion.h"
using namespace std::chrono_literals;
int main() {
    using namespace Jadyx;
    const auto start = StickMotion::Clock::time_point{};
    constexpr auto l3 = 1U << PadBit::LStick;
    constexpr auto view = 1U << PadBit::Minus;
    SyntheticMotion input;
    StylePad pad{.buttons = l3 | view, .right_x = 1};
    auto sample = input.Filter(pad, false, true, false, start);
    assert(pad.buttons == (l3 | view) && pad.right_x == 1 && sample.gyro[2] == 0);
    pad = {.buttons = l3, .right_x = 1};
    sample = input.Filter(pad, true, false, false, start + 10ms);
    assert(pad.buttons == l3 && pad.right_x == 1 && sample.gyro[2] == 0);
    pad = {};
    input.Filter(pad, true, true, false, start + 20ms);
    pad = {.buttons = l3};
    input.Filter(pad, true, true, false, start + 30ms);
    assert(pad.buttons == 0);
    pad = {};
    input.Filter(pad, true, true, false, start + 100ms);
    assert(pad.buttons == l3); // A short tap is delivered for 40 ms.
    pad = {};
    input.Filter(pad, true, true, false, start + 141ms);
    assert(pad.buttons == 0);
    pad = {.buttons = l3};
    input.Filter(pad, true, true, false, start + 200ms);
    pad = {.buttons = l3, .right_x = 1};
    sample = input.Filter(pad, true, true, false, start + 210ms);
    assert(pad.buttons == 0 && pad.right_x == 0);
    assert(std::abs(sample.gyro[2] + .5f) < .0001f);
    pad = {};
    sample = input.Filter(pad, true, true, false, start + 220ms);
    assert(pad.buttons == 0 && sample.gyro[2] == 0);
    // View + right stick also aims; movement never produces a View tap.
    pad = {.buttons = view, .left_x = 1, .right_y = 1};
    sample = input.Filter(pad, true, true, false, start + 300ms);
    assert(pad.buttons == 0 && pad.right_y == 0 && pad.left_x == 1);
    assert(std::abs(sample.gyro[0] - .5f) < .0001f);
    pad = {};
    input.Filter(pad, true, true, false, start + 310ms);
    assert(pad.buttons == 0);
    // A stationary L3 hold becomes motion at 250 ms; release must never tap.
    pad = {.buttons = l3};
    input.Filter(pad, true, true, false, start + 400ms);
    pad = {.buttons = l3};
    input.Filter(pad, true, true, false, start + 650ms);
    pad = {};
    input.Filter(pad, true, true, false, start + 660ms);
    assert(pad.buttons == 0);
    // A modal cancels the gesture until release, even after closing.
    pad = {.buttons = l3, .right_x = 1};
    input.Filter(pad, true, true, true, start + 700ms);
    pad = {.buttons = l3, .right_x = 1};
    sample = input.Filter(pad, true, true, false, start + 710ms);
    assert(sample.gyro[2] == 0);
    pad = {};
    input.Filter(pad, true, true, false, start + 720ms);
    assert(pad.buttons == 0);
    // Entering sensor mode with L3 already held cannot invent a tap.
    SyntheticMotion transition;
    pad = {.buttons = l3};
    transition.Filter(pad, true, false, false, start);
    pad = {.buttons = l3, .right_x = 1};
    sample = transition.Filter(pad, true, true, false, start + 1ms);
    assert(sample.gyro[2] == 0);
    pad = {};
    transition.Filter(pad, true, true, false, start + 2ms);
    assert(pad.buttons == 0);
    SyntheticMotion separate;
    pad = {.buttons = view};
    separate.Filter(pad, true, true, false, start);
    pad = {};
    separate.Filter(pad, true, true, false, start + 100ms);
    assert(pad.buttons == view);
    // Unit gravity and finite bounded rates at repeated timestamps and after stalls.
    StickMotion math;
    const auto first = math.Update(true, true, 1, 1, 0, 0, start);
    const auto second = math.Update(true, true, 1, 1, 0, 0, start);
    for (const auto& value : {first, second}) {
        float magnitude = 0;
        for (float axis : value.accel)
            magnitude += axis * axis;
        assert(std::abs(magnitude - 1) < .0001f);
        for (float axis : value.gyro)
            assert(std::isfinite(axis) && std::abs(axis) <= 2.001f);
        assert(value.delta_timestamp > 0 && value.delta_timestamp <= 100000);
    }
    const auto stalled = math.Update(true, true, -1, -1, 0, 0, start + 10s);
    assert(stalled.delta_timestamp == 100000);
    SyntheticMotion captured;
    MenuComboFilter combo;
    int toggles = 0;
    for (std::uint32_t tick = 0; tick <= 1200; tick += 4) {
        pad = {.buttons = view | (1U << PadBit::Plus), .right_x = 1};
        bool plus = true, minus = true;
        const bool toggle = combo.Filter(plus, minus, tick);
        toggles += toggle ? 1 : 0;
        sample = captured.FilterCaptured(pad, true, true, toggle, minus,
                                         start + std::chrono::milliseconds(tick));
        assert(!plus && !minus && sample.gyro[2] == 0);
    }
    assert(toggles == 1);
    // Navigation remains available in a modal, guest controls stay captured after close.
    pad = {.buttons = l3 | (1U << PadBit::A), .left_y = 1, .right_x = 1};
    bool minus = false;
    captured.FilterCaptured(pad, true, true, true, minus, start + 1300ms);
    assert((pad.buttons & (1U << PadBit::A)) && pad.left_y == 1);
    pad = {.buttons = l3 | (1U << PadBit::A), .left_y = 1, .right_x = 1};
    sample = captured.FilterCaptured(pad, true, true, false, minus, start + 1310ms);
    assert(pad.buttons == 0 && pad.left_y == 0 && sample.gyro[2] == 0);
    Core::HID::SixAxisRequests requests;
    assert(!requests.Any(5));
    assert(requests.DeliveryEnabled(true, 5));
    assert(!requests.DeliveryEnabled(false, 5));
    requests.SetSyntheticEnabled(true);
    assert(!requests.DeliveryEnabled(true, 5));
    requests.Set(5, 0, true);
    requests.Set(5, 0, true); // Start is idempotent.
    requests.Set(5, 1, true);
    requests.Set(5, 0, false);
    assert(requests.Any(5) && !requests.Any(3));
    assert(!requests.DeliveryEnabled(true, 5, 0));
    assert(requests.DeliveryEnabled(false, 5, 1));
    requests.Set(5, 1, false);
    assert(!requests.Any(5));
    requests.Set(5, 1, true);
    requests.Clear();
    assert(!requests.Any(5));
    std::puts("synthetic motion gestures, math and sensor requests PASS");
}
