// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "jadyx/input/keyboard_bindings.h"
int main() {
    using namespace Jadyx;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    auto bindings = DefaultKeyboardBindings;
    check(AssignKeyboardKey(bindings, 0, 'Q'));
    check(AssignKeyboardKey(bindings, 1, 'Q'));
    check(bindings[0] == 0 && bindings[1] == 'Q');
    check(KeyboardKeyOwner(bindings, 0, 'Q') == size_t{1});
    check(!KeyboardKeyOwner(bindings, 1, 'Q'));
    check(!KeyboardKeyOwner(bindings, 0, 0));
    check(AssignKeyboardKey(bindings, 27, 'T'));
    check(bindings[27] == 'T');
    auto before = bindings;
    check(!AssignKeyboardKey(bindings, 0, 27));
    check(!AssignKeyboardKey(bindings, 0, 195));
    check(!AssignKeyboardKey(bindings, KeyboardActionCount, 65));
    check(!AssignKeyboardKey(bindings, 0, 65535));
    check(bindings == before);
    check(AssignKeyboardKey(bindings, 1, 0) && bindings[1] == 0);
    for (size_t i = 0; i < KeyboardActionCount; ++i) {
        if (i == 16 || i == 17) continue;
        const auto tile = KeyboardTileFor(i);
        check(Contains(tile, tile.x + tile.w / 2, tile.y + tile.h / 2));
        check(!Contains(tile, tile.x + tile.w, tile.y + tile.h));
        check(tile.y + tile.h <= 573);
        for (size_t j = 0; j < KeyboardActionCount; ++j) {
            if (i == j || KeyboardPageFor(i) != KeyboardPageFor(j)) continue;
            if (j == 16 || j == 17) continue;
            const auto other = KeyboardTileFor(j);
            check(!Contains(other, tile.x + tile.w / 2, tile.y + tile.h / 2));
        }
    }
    // The categories stay inside their bar and apart, for the keyboard's three and the controller's four.
    for (const unsigned pages : {3U, 4U}) {
        BindingEditor editor;
        editor.page_count = pages;
        for (unsigned page = 0; page < pages; ++page) {
            const auto tab = BindingTabFor(editor, page);
            check(tab.x >= BindingTabBar.x + 40 && tab.x + tab.w <= BindingTabBar.x + BindingTabBar.w - 40);
            check(tab.y >= BindingTabBar.y && tab.y + tab.h <= BindingTabBar.y + BindingTabBar.h);
            check(tab.y + tab.h <= KeyboardTileFor(0).y);
            if (page) check(BindingTabFor(editor, page - 1).x + BindingTabFor(editor, page - 1).w <= tab.x + 0.01f);
        }
    }
    std::puts("PASS keyboard conflicts, Q/T, clear, invalid keys, three pages, hit bounds and category bar");
}
