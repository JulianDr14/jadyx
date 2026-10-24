// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>
#include "jadyx/library/game_options_page.h"
int main() {
    using namespace Jadyx;
    // Run in a clean environment. The opt-in adds exactly one in-place action.
    assert(!MoveToUsbEnabled() && GameOptions().size() == 8);
    move_to_usb_boot_option = true;
    assert(GameOptions().size() == 9 &&
           GameOptions().back() == GameOption::MoveToUsb);
    assert(!GameOptionHasManager(GameOption::MoveToUsb));
    GameOptionsPage page;
    page.open = true;
    SelectGameOption(page, GameOptions().size() - 1);
    HandleGameOptionsInput(page, GameOptionsInput::Confirm);
    assert(page.confirm_discard && !page.discard);
    HandleGameOptionsInput(page, GameOptionsInput::Confirm);
    assert(page.discard && !page.confirm_discard);
    page.discard = false;
    HandleGameOptionsInput(page, GameOptionsInput::Confirm);
    HandleGameOptionsInput(page, GameOptionsInput::Up);
    assert(!page.confirm_discard && !page.discard);
    move_to_usb_boot_option = false;
    assert(GameOptions().size() == 8);
    std::cout << "move options: default off, opt-in, confirmation and navigation PASS\n";
}
