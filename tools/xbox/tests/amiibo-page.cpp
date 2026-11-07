// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>

#include "jadyx/game/amiibo_page.h"

namespace {
using namespace Jadyx;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAILED: %s\n", what);
    std::exit(1);
}
} // namespace

int main() {
    // Moving wraps and keeps the cursor in the window.
    AmiiboPage page;
    constexpr std::size_t Rows = 8, Visible = 6;
    check(HandleAmiiboInput(page, MenuAction::Up, Rows, Visible) == AmiiboEffect::Moved, "up moves");
    check(page.selected == 7 && page.first == 2, "wraps to the last row, in the window");
    check(HandleAmiiboInput(page, MenuAction::Down, Rows, Visible) == AmiiboEffect::Moved, "down moves");
    check(page.selected == 0 && page.first == 0, "wraps to the first row");
    for (int i = 0; i < 6; ++i) HandleAmiiboInput(page, MenuAction::Down, Rows, Visible);
    check(page.selected == 6 && page.first == 1, "the window follows");

    // Confirm chooses, and only when there is something to choose.
    check(HandleAmiiboInput(page, MenuAction::Confirm, Rows, Visible) == AmiiboEffect::Choose, "confirm");
    AmiiboPage empty;
    check(HandleAmiiboInput(empty, MenuAction::Confirm, 0, Visible) == AmiiboEffect::None, "nothing to choose");
    check(HandleAmiiboInput(empty, MenuAction::Up, 0, Visible) == AmiiboEffect::None, "nowhere to go");
    check(HandleAmiiboInput(empty, MenuAction::Back, 0, Visible) == AmiiboEffect::Back, "back when empty");

    // The single row that removes the tag does not move.
    AmiiboPage single;
    check(HandleAmiiboInput(single, MenuAction::Down, 1, Visible) == AmiiboEffect::None, "one row stays");
    check(HandleAmiiboInput(single, MenuAction::Confirm, 1, Visible) == AmiiboEffect::Choose, "removes");

    // Other buttons do nothing on this page.
    for (const auto action : {MenuAction::Left, MenuAction::Right, MenuAction::Alternate, MenuAction::Refresh,
                              MenuAction::PreviousTab, MenuAction::NextTab}) {
        check(HandleAmiiboInput(page, action, Rows, Visible) == AmiiboEffect::None, "other buttons");
    }
    check(page.selected == 6, "they leave the cursor");

    check(AmiiboDisplayName("figure.bin") == "figure", "no extension");
    check(AmiiboDisplayName("C:\\a\\b/Figure One.BIN") == "Figure One", "no folders");
    check(AmiiboDisplayName("a.b.bin") == "a.b", "only the last extension");
    check(AmiiboDisplayName("") == "", "no path");

    std::puts("amiibo page: PASS");
}
