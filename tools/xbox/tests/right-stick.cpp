// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// The right stick's settings and the mouse as the right stick (pure logic).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "jadyx/input/mouse_stick.h"
#include "jadyx/input/right_stick_settings.h"

int main() {
    using namespace Jadyx;
    const auto check = [](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };

    // Sensitivity on a pad's stick: scales the length, never past the edge.
    {
        float x = 0.4f, y = 0.0f;
        ScaleStick(x, y, 100);
        check(near(x, 0.4f), "100% leaves the stick alone");
        ScaleStick(x, y, 150);
        check(near(x, 0.6f) && near(y, 0.0f), "150% scales");
        x = 0.8f, y = 0.6f;
        ScaleStick(x, y, 200);
        check(near(std::hypot(x, y), 1.0f) && near(x, 0.8f) && near(y, 0.6f), "capped at the edge, same direction");
        x = 0.5f, y = -0.5f;
        ScaleStick(x, y, 50);
        check(near(x, 0.25f) && near(y, -0.25f), "50% halves");
    }
    // Inversion flips each axis on its own.
    {
        float x = 0.3f, y = -0.7f;
        InvertStick(x, y, true, false);
        check(near(x, -0.3f) && near(y, -0.7f), "invert X");
        InvertStick(x, y, false, true);
        check(near(x, -0.3f) && near(y, 0.7f), "invert Y");
    }
    // Steps wrap both ways and snap a value off the steps to the nearest one.
    {
        check(StepSensitivity(StickSensitivitySteps, 100, false) == 110, "stick forward");
        check(StepSensitivity(StickSensitivitySteps, 100, true) == 90, "stick back");
        check(StepSensitivity(StickSensitivitySteps, 200, false) == 50, "stick wraps forward");
        check(StepSensitivity(StickSensitivitySteps, 50, true) == 200, "stick wraps back");
        check(StepSensitivity(StickSensitivitySteps, 123, false) == 150, "snaps to 125 first");
        check(StepSensitivity(MouseSensitivitySteps, 100, false) == 125, "mouse forward");
        check(StepSensitivity(MouseSensitivitySteps, 25, true) == 250, "mouse wraps back");
        check(IsSensitivityStep(StickSensitivitySteps, 175) && !IsSensitivityStep(StickSensitivitySteps, 176), "steps");
    }
    // A game's values: unset fields follow Settings; landing on Settings' value unsets.
    {
        RightStickSettings global{};
        global.invert_y = true;
        global.mouse_sensitivity = 150;
        GameRightStick game{};
        game.invert_x = true;
        game.mouse = true;
        const auto resolved = ResolveRightStick(global, game);
        check(resolved.invert_x && resolved.invert_y && resolved.mouse, "resolve bools");
        check(resolved.stick_sensitivity == 100 && resolved.mouse_sensitivity == 150, "resolve numbers");
        check(!GameOverride<bool>(true, true) && GameOverride<bool>(false, true) == false, "override bool");
        check(GameOverride<std::uint8_t>(125, 100) == std::uint8_t{125}, "override number");
    }
    // The mouse: motion pushes the stick, a small push clears the game's deadzone, and it
    // re-centers on its own once the mouse stops.
    {
        MouseStick mouse;
        check(mouse.Idle(), "starts idle");
        mouse.Move(1, 0, 100);
        auto p = mouse.Update(0);
        check(near(p.x, MouseStick::Counterweight) && near(p.y, 0.0f), "one pixel is lifted to the counterweight");
        mouse.Reset();
        mouse.Move(0, 10, 100); // Down on the mouse is down on the stick (y up).
        p = mouse.Update(0);
        check(near(p.x, 0.0f) && near(p.y, -0.5f), "10 px down at 100%");
        mouse.Reset();
        mouse.Move(10, 0, 200);
        p = mouse.Update(0);
        check(near(p.x, 1.0f), "sensitivity doubles the push");
        mouse.Reset();
        mouse.Move(100, 0, 100);
        p = mouse.Update(0);
        check(near(std::hypot(p.x, p.y), 1.0f), "inside the unit circle");
        // Decay: the same 100 ms in one poll or in twenty gives the same stick.
        MouseStick one, many;
        one.Move(8, 0, 100);
        many.Move(8, 0, 100);
        one.Update(100);
        for (int i = 0; i < 20; ++i) many.Update(5);
        check(near(one.Update(0).x, many.Update(0).x), "decay does not depend on the poll rate");
        // Without motion it reaches the center and stays idle.
        for (int i = 0; i < 400 && !one.Idle(); ++i) one.Update(5);
        check(one.Idle(), "re-centers");
        p = one.Update(5);
        check(p.x == 0.0f && p.y == 0.0f, "idle reads centered");
    }
    std::puts("right-stick: ok");
    return 0;
}
