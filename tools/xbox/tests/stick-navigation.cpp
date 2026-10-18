// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstdio>
#include <limits>
#include "jadyx/input/stick_navigation.h"
int main() {
    using namespace Jadyx;
    using namespace std::chrono;
    StickNavigation input;
    const auto at = [](int ms) { return StickNavigation::Clock::time_point{milliseconds{ms}}; };
    const auto check = [](bool value) { if (!value) std::abort(); };
    check(input.Update(.3, .3, at(0)) == StickDirection::None);
    check(input.Update(.8, 0, at(1)) == StickDirection::Right);
    check(input.Update(.8, 0, at(350)) == StickDirection::None);
    check(input.Update(.8, 0, at(351)) == StickDirection::Right);
    check(input.Update(.8, 0, at(470)) == StickDirection::None);
    check(input.Update(.8, 0, at(471)) == StickDirection::Right);
    check(input.Update(.7, .75, at(500)) == StickDirection::None);
    check(input.Update(.7, .75, at(591)) == StickDirection::Right);
    check(input.Update(.4, 0, at(711)) == StickDirection::Right);
    check(input.Update(.2, 0, at(712)) == StickDirection::None);
    check(input.Update(0, -1, at(713)) == StickDirection::Down);
    check(input.Update(0, 1, at(714)) == StickDirection::Up);
    input.Reset();
    check(input.Update(0, 1, at(1000)) == StickDirection::None);
    check(input.Update(0, 0, at(1001)) == StickDirection::None);
    check(input.Update(-1, 0, at(1002)) == StickDirection::Left);
    check(input.Update(-1, 0, at(9000)) == StickDirection::Left);
    check(input.Update(-1, 0, at(9000)) == StickDirection::None);
    check(input.Update(std::numeric_limits<double>::quiet_NaN(), 1, at(9100)) == StickDirection::None);
    check(input.Update(1, 0, at(9101)) == StickDirection::None);
    StickNavigation manager{milliseconds{280}, milliseconds{80}};
    check(manager.Update(0, -1, at(0)) == StickDirection::Down);
    check(manager.Update(0, -1, at(279)) == StickDirection::None);
    check(manager.Update(0, -1, at(280)) == StickDirection::Down);
    check(manager.Update(0, -1, at(359)) == StickDirection::None);
    check(manager.Update(0, -1, at(360)) == StickDirection::Down);
    TriggerTabs triggers;
    check(triggers.Update(.54, 0) == 0);
    check(triggers.Update(.55, 0) == 1);
    check(triggers.Update(1, 0) == 0);
    check(triggers.Update(.4, 0) == 0);
    check(triggers.Update(.35, 0) == 0);
    check(triggers.Update(.6, 0) == 1);
    triggers.Reset();
    check(triggers.Update(1, 1) == 0); // held across a page transition
    check(triggers.Update(0, 0) == 0);
    check(triggers.Update(1, 1) == 0); // simultaneous presses cancel
    check(triggers.Update(0, 0) == 0);
    check(triggers.Update(0, 1) == 2);
    check(triggers.Update(0, 1) == 0);
    std::puts("stick navigation: drift/hysteresis/diagonal/repeat/reversal/modal/stall/invalid PASS");
}
