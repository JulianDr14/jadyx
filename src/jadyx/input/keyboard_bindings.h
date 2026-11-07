// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <string>
#include <cstddef>
#include <optional>
#include <span>

namespace Jadyx {
// First 20 actions deliberately match VirtualGamepad::VirtualButton ordering.
inline constexpr size_t KeyboardButtonCount = 20;
inline constexpr size_t KeyboardActionCount = 28;
using KeyboardBindings = std::array<unsigned, KeyboardActionCount>;
inline constexpr KeyboardBindings DefaultKeyboardBindings{
    'B', 'N', 'X', 'Y', 'F', 'H', 'C', 'V', 'Z', 'E', 'M', 'K',
    37, 38, 39, 40, 0, 0, 36, 35, 'W', 'S', 'A', 'D', 'I', 'L', 'J', 'O'};
// Interface names: KeyboardActionName in ui_text.h.
/// Virtual keys a gamepad generates on the console: never a real keyboard's key.
inline constexpr bool IsGamepadVirtualKey(unsigned key) { return key >= 195 && key <= 218; }
// One key per action. Move duplicate bindings instead of leaving hidden conflicts.
inline bool AssignKeyboardKey(KeyboardBindings& bindings, size_t action, unsigned key) {
    if (action >= bindings.size() || key == 27 || key > 254 || IsGamepadVirtualKey(key)) return false;
    if (key) for (auto& existing : bindings) if (existing == key) existing = 0;
    bindings[action] = key;
    return true;
}
/// The other action already driven by this key, if any.
inline std::optional<size_t> KeyboardKeyOwner(const KeyboardBindings& bindings, size_t action, unsigned key) {
    if (!key) return std::nullopt;
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (i != action && bindings[i] == key) return i;
    }
    return std::nullopt;
}
KeyboardBindings LoadKeyboardBindings();
bool SaveKeyboardBindings(const KeyboardBindings& bindings);
std::wstring KeyboardKeyName(unsigned key);
void SetKeyboardKey(unsigned key, bool pressed);
void ResetKeyboardKeys();
void InitializeKeyboardBindings();
bool RunKeyboardInputSelfTest(); // Explicit boot.cfg gate; isolated engine, no user events.

/// What the keyboard and controller editors share: the same pages, tiles and diagram.
struct BindingEditor {
    std::wstring notice;
    bool open{}, capturing{};
    size_t selected{};
    unsigned page{};
    unsigned page_count{3}; // The controller adds Jadyx Hub: Escape opens it on a keyboard.
};
struct KeyboardEditor : BindingEditor {
    KeyboardBindings bindings{DefaultKeyboardBindings};
};
/// An editor entry past the bindings: the inputs that open the in-game menu.
inline constexpr size_t MenuShortcutAction = KeyboardActionCount;
inline std::span<const size_t> KeyboardPageActions(unsigned page) {
    static constexpr std::array<size_t, 12> buttons{0,1,2,3,4,5,6,7,8,9,10,11};
    static constexpr std::array<size_t, 6> system{12,13,14,15,18,19};
    static constexpr std::array<size_t, 8> sticks{20,21,22,23,24,25,26,27};
    static constexpr std::array<size_t, 1> hub{MenuShortcutAction};
    if (page == 0) return buttons;
    if (page == 1) return system;
    if (page == 2) return sticks;
    return hub;
}
/// Moves over the page's two-column grid: left/right by one tile, up/down by a row.
inline void MoveBindingSelection(BindingEditor& editor, bool vertical, bool reverse) {
    const auto items = KeyboardPageActions(editor.page);
    const size_t count = items.size();
    size_t index = 0;
    while (index < count && items[index] != editor.selected) ++index;
    if (index == count) index = 0;
    const size_t step = vertical ? 2 : 1;
    editor.selected = items[(index + (reverse ? count - step : step)) % count];
}
/// Next or previous page, on its first action.
inline void TurnBindingPage(BindingEditor& editor, bool reverse) {
    editor.page = (editor.page + (reverse ? editor.page_count - 1 : 1)) % editor.page_count;
    editor.selected = KeyboardPageActions(editor.page).front();
}
inline unsigned KeyboardPageFor(size_t action) {
    return action < 12 ? 0U : action < 20 ? 1U : action < KeyboardActionCount ? 2U : 3U;
}
struct KeyboardTile { float x, y, w, h; };
inline constexpr KeyboardTile BindingCloseButton{1120, 96, 48, 40};
inline constexpr KeyboardTile BindingClearButton{635, 595, 155, 36};
inline constexpr KeyboardTile BindingRestoreButton{802, 595, 170, 36};
/// The category bar over the tiles; the bumpers' names sit at its ends.
inline constexpr KeyboardTile BindingTabBar{635, 196, 514, 44};
inline KeyboardTile BindingTabFor(const BindingEditor& editor, unsigned page) {
    constexpr float Left = 683, Width = 418;
    const float width = Width / editor.page_count;
    return {Left + page * width, BindingTabBar.y + 4, width, BindingTabBar.h - 8};
}
inline KeyboardTile KeyboardTileFor(size_t action) {
    // Both columns wide: a combination's name is long.
    if (action == MenuShortcutAction) return {635, 253, 514, 45};
    const auto actions = KeyboardPageActions(KeyboardPageFor(action));
    size_t index = 0;
    while (index < actions.size() && actions[index] != action) ++index;
    if (index == actions.size()) return {};
    return {635 + (index % 2) * 263.0f, 253 + (index / 2) * 53.0f, 251, 45};
}
inline bool Contains(KeyboardTile tile, float x, float y) {
    return x >= tile.x && x < tile.x + tile.w && y >= tile.y && y < tile.y + tile.h;
}
inline KeyboardTile KeyboardDiagramTile(size_t action) {
    switch (action) {
    case 0: return {471, 335, 30, 30}; case 1: return {440, 361, 30, 30};
    case 2: return {440, 309, 30, 30}; case 3: return {407, 335, 30, 30};
    case 4: return {224, 315, 64, 64}; case 5: return {373, 370, 64, 64};
    case 6: return {188, 260, 120, 20}; case 7: return {412, 260, 120, 20};
    case 8: return {232, 247, 66, 14}; case 9: return {422, 247, 66, 14};
    case 10: return {394, 310, 18, 18}; case 11: return {302, 310, 18, 18};
    case 12: return {271, 392, 20, 20}; case 13: return {291, 372, 20, 20};
    case 14: return {311, 392, 20, 20}; case 15: return {291, 412, 20, 20};
    case 18: return {371, 341, 18, 18}; case 19: return {326, 342, 16, 16};
    default: return {};
    }
}
}
