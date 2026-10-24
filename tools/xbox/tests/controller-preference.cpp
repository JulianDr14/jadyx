// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/input/controller_style.h"
#include <cassert>
#include <cstdio>
int main() {
  using namespace Jadyx;
  using S = ConsoleControllerStyle;
  const SupportedControllerStyles broad{.fullkey = true, .joycon_dual = true};
  const SupportedControllerStyles restricted{
      .handheld = true, .joycon_left = true, .joycon_right = true};
  for (auto id : {0x010003F003A34000ULL, 0x0100187003A36000ULL}) {
    assert(PreferredControllerStyle(broad, id, true, true) == S::DualJoycon);
    assert(PreferredControllerStyle(restricted, id, true, true) ==
           S::RightJoycon);
    assert(PreferredControllerStyle(restricted, id, true, false) ==
           S::Handheld);
    assert(PreferredControllerStyle(broad, id, false, true) == S::Pro);
    assert(PreferredControllerStyle(restricted, id, false, true) ==
           S::Handheld);
  }
  assert(PreferredControllerStyle(broad, 0, true, true) == S::Pro);
  assert(PreferredControllerStyle(restricted, 0, true, true) == S::Handheld);
  assert(PreferredControllerStyle({}, 0, true, true) == S::Pro);
  const auto right = MapPadToStyle({.left_x = 1.0f}, S::RightJoycon);
  assert(right.right_y == 1 && right.left_x == 0);
  std::puts("restricted controller preference PASS");
}
