// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "jadyx/input/pad_mapping.h"

int main() {
    using namespace Jadyx;
    using K = PadSourceKind;
    const auto check = [](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL %s\n", what);
            std::exit(1);
        }
    };
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.001f; };

    // A standard pad maps as the unmapped path does.
    PadSnapshot pad{};
    pad.buttons = (1ULL << StdA) | (1ULL << StdLeftShoulder) | (1ULL << StdMenu) | (1ULL << StdDPadUp);
    pad.axes[StdLeftX] = -0.5f;
    pad.axes[StdLeftY] = 0.25f;
    pad.axes[StdRightTrigger] = 0.9f;
    auto output = ApplyPadBindings(StandardPadBindings, pad);
    check(output.buttons == ((1U << 0) | (1U << 6) | (1U << 10) | (1U << 13) | (1U << 9)), "standard buttons");
    check(near(output.left_x, -0.5f) && near(output.left_y, 0.25f), "standard left stick");
    pad = {};
    pad.axes[StdLeftTrigger] = 0.4f;
    check(ApplyPadBindings(StandardPadBindings, pad).buttons == 0, "a trigger under the threshold stays up");

    // A generic pad: a hat for the d-pad, an inverted Y, and triggers that do not hold a stick over.
    const std::array<float, 6> sticks_rest{0.0f, 0.02f, -1.0f, 0.0f, -1.0f, -1.0f};
    auto generic = GenericPadBindings(12, sticks_rest, 1);
    check(generic[0] == PadSource{K::Button, 0} && generic[10] == PadSource{K::Button, 7}, "generic buttons");
    check(generic[13] == PadSource{K::SwitchUp, 0}, "generic hat");
    PadSnapshot raw{};
    raw.axes = {0.0f, -1.0f, -1.0f, 0.0f, -1.0f, -1.0f};
    output = ApplyPadBindings(generic, raw);
    check(near(output.left_y, 1.0f), "HID Y axis grows downwards");
    check(output.right_x == 0.0f && output.right_y == 0.0f, "off-center axes never bind a stick");
    const auto few = GenericPadBindings(2, {}, 0);
    check(few[1].kind == K::Button && few[2].kind == K::None && few[12].kind == K::None, "only existing inputs");

    // A remap: a source in use is refused and names its owner, until the player frees it.
    auto bindings = StandardPadBindings;
    const auto untouched = bindings;
    check(!AssignPadSource(bindings, 1, {K::Button, StdA}) && bindings == untouched, "a source in use is refused");
    check(PadSourceOwner(bindings, 1, {K::Button, StdA}) == std::size_t{0}, "its owner");
    check(!PadSourceOwner(bindings, 0, {K::Button, StdA}), "an action does not own against itself");
    check(!PadSourceOwner(bindings, 0, {}), "nothing owns an empty source");
    check(AssignPadSource(bindings, 0, {K::Button, StdA}) && bindings == untouched, "the same source again");
    // Swapping A and B goes through an empty action, which only leaving has to resolve.
    check(AssignPadSource(bindings, 0, {}) && bindings[0].kind == K::None, "clear");
    check(PadMissingAction(bindings) == std::size_t{0}, "no way to confirm");
    check(!PadSourceOwner(bindings, 1, {K::Button, StdA}), "the source is free now");
    check(AssignPadSource(bindings, 1, {}) && PadMissingAction(bindings) == std::size_t{0}, "A comes first");
    check(AssignPadSource(bindings, 0, {K::Button, StdB}), "A on the old B");
    check(PadMissingAction(bindings) == std::size_t{1}, "no way back");
    check(AssignPadSource(bindings, 1, {K::Button, StdA}) && !PadMissingAction(bindings), "swapped");
    // The two directions of an axis are separate sources.
    check(!AssignPadSource(bindings, 8, {K::AxisNegative, StdLeftY}), "that axis direction is in use");
    check(AssignPadSource(bindings, 21, {}) && AssignPadSource(bindings, 8, {K::AxisNegative, StdLeftY}), "freed");
    check(bindings[20] == PadSource{K::AxisPositive, StdLeftY}, "directions apart");
    // A direction may live on the d-pad or the stick, but it must live somewhere.
    check(!PadMissingAction(bindings), "down remains on the d-pad");
    check(AssignPadSource(bindings, 15, {}) && PadMissingAction(bindings) == std::size_t{15}, "no way down");
    check(!PadMissingAction(StandardPadBindings), "the default is complete");
    check(!AssignPadSource(bindings, KeyboardActionCount, {K::Button, 1}), "action out of range");

    // Capture: the held button counts only after a release; an axis must really travel.
    PadSnapshot held{};
    held.buttons = 1;
    held.axes[2] = -1.0f;
    PadCapture capture;
    capture.Begin(held);
    check(!capture.Detect(held), "held input ignored");
    PadSnapshot released = held;
    released.buttons = 0;
    check(!capture.Detect(released), "release is not a press");
    check(capture.Detect(held) == PadSource{K::Button, 0}, "pressed again counts");
    capture.Begin(held);
    PadSnapshot trigger = held;
    trigger.axes[2] = 0.3f;
    check(!capture.Detect(trigger), "an axis must reach far enough");
    trigger.axes[2] = 0.9f;
    check(capture.Detect(trigger) == PadSource{K::AxisPositive, 2}, "a trigger resting at -1");
    capture.Begin({});
    PadSnapshot drift{};
    drift.axes[0] = 0.2f;
    check(!capture.Detect(drift), "drift is not a press");
    PadSnapshot hat{};
    hat.switches[1] = PadSwitchLeft;
    check(capture.Detect(hat) == PadSource{K::SwitchLeft, 1}, "hat direction");

    // Storage round trip, and anything damaged is rejected whole.
    const auto text = SerializePadBindings(generic);
    check(ParsePadBindings(text) == generic, "round trip");
    check(!ParsePadBindings(""), "empty");
    check(!ParsePadBindings(text.substr(0, text.size() - 1)), "cut short");
    check(!ParsePadBindings(text + "0:0,"), "too long");
    auto bad = text;
    bad[0] = '9';
    check(!ParsePadBindings(bad), "unknown kind");
    check(!ParsePadBindings("1:64," + text.substr(4)), "button out of range");

    // The editor's movement.
    BindingEditor editor;
    MoveBindingSelection(editor, false, true);
    check(editor.selected == 11, "left wraps");
    MoveBindingSelection(editor, true, false);
    check(editor.selected == 1, "down wraps");
    TurnBindingPage(editor, false);
    check(editor.page == 1 && editor.selected == 12, "next page");
    TurnBindingPage(editor, true);
    TurnBindingPage(editor, true);
    check(editor.page == 2 && editor.selected == 20, "previous page wraps");

    // Jadyx Hub has its own tab, only on the controller's editor, reached from either side.
    BindingEditor keyboard_like;
    for (int i = 0; i < 3; ++i) {
        TurnBindingPage(keyboard_like, false);
        check(keyboard_like.selected != MenuShortcutAction, "the keyboard has no Jadyx Hub tab");
    }
    PadMapEditor map_editor;
    TurnBindingPage(map_editor, true);
    check(map_editor.page == 3 && map_editor.selected == MenuShortcutAction, "the last tab");
    MoveBindingSelection(map_editor, false, false);
    MoveBindingSelection(map_editor, true, true);
    check(map_editor.selected == MenuShortcutAction, "a lone tile stays selected");
    check(KeyboardPageFor(MenuShortcutAction) == 3, "its page");
    TurnBindingPage(map_editor, false);
    check(map_editor.page == 0 && map_editor.selected == 0, "wraps to the first tab");

    // Capturing a combination: the held A only counts after a release, inputs add up, a release ends it.
    PadShortcutCapture combo_capture;
    PadSnapshot a_held{};
    a_held.buttons = 1ULL << StdA;
    combo_capture.Begin(a_held);
    check(!combo_capture.Update(a_held), "the A that opened the capture is not part of it");
    PadSnapshot both{};
    both.buttons = (1ULL << StdLeftThumb) | (1ULL << StdRightThumb);
    check(!combo_capture.Update(both), "two pressed in one poll");
    check(!combo_capture.Update(both), "still held, still waiting");
    PadSnapshot one_left{};
    one_left.buttons = 1ULL << StdRightThumb;
    const auto captured = combo_capture.Update(one_left);
    check(captured && captured->count == 2, "both inputs captured");
    check(captured->sources[0] == PadSource{K::Button, StdLeftThumb} &&
          captured->sources[1] == PadSource{K::Button, StdRightThumb}, "captured inputs");
    combo_capture.Begin({});
    PadSnapshot trigger_held{};
    trigger_held.axes[StdLeftTrigger] = 1.0f;
    check(!combo_capture.Update(trigger_held), "a trigger joins");
    const auto single = combo_capture.Update({});
    check(single && single->count == 1 && single->sources[0] == PadSource{K::AxisPositive, StdLeftTrigger},
          "a single input is a shortcut too");

    // The filter on a combination: what reaches the game and when it fires.
    const PadMenuShortcut sticks_combo{{PadSource{K::Button, StdLeftThumb}, PadSource{K::Button, StdRightThumb}}, 2};
    PadShortcutFilter filter;
    PadSnapshot frame = both;
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 0) && frame.buttons == 0, "held back while formed");
    frame = both;
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 999) && frame.buttons == 0, "not before a second");
    frame = both;
    check(ApplyMenuShortcut(sticks_combo, frame, filter, 1000) && frame.buttons == 0, "fires at a second");
    frame = both;
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 1500), "fires once");
    frame = one_left;
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 1510) && frame.buttons == 0, "hidden until all released");
    frame = {};
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 1520), "released");
    frame = one_left;
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 2000) && frame.buttons == 0, "a lone press waits");
    frame = {};
    ApplyMenuShortcut(sticks_combo, frame, filter, 2020);
    check(frame.buttons == one_left.buttons, "a tap still arrives");
    frame = {};
    ApplyMenuShortcut(sticks_combo, frame, filter, 2030);
    check(frame.buttons == 0, "and is released");
    frame = one_left;
    ApplyMenuShortcut(sticks_combo, frame, filter, 3000);
    frame = one_left;
    ApplyMenuShortcut(sticks_combo, frame, filter, 3100);
    check(frame.buttons == one_left.buttons, "a long lone press reaches the game");
    frame = both;
    check(!ApplyMenuShortcut(sticks_combo, frame, filter, 3200) && frame.buttons == 0, "the combination takes over");
    frame = both;
    check(ApplyMenuShortcut(sticks_combo, frame, filter, 4200), "and fires a second later");
    frame = {};
    ApplyMenuShortcut(sticks_combo, frame, filter, 4210);

    // A trigger in a combination is hidden through its axis; it does not disturb the opposite side.
    const PadMenuShortcut triggers{{PadSource{K::AxisPositive, StdLeftTrigger}, PadSource{K::Button, StdA}}, 2};
    PadShortcutFilter trigger_filter;
    frame = {};
    frame.axes[StdLeftTrigger] = 1.0f;
    frame.buttons = 1ULL << StdA;
    ApplyMenuShortcut(triggers, frame, trigger_filter, 0);
    check(frame.axes[StdLeftTrigger] == 0.0f && frame.buttons == 0, "axis hidden");

    // A single input keeps playing: it reaches the game and a long hold opens the menu, once.
    const PadMenuShortcut lone{{PadSource{K::Button, StdView}}, 1};
    PadShortcutFilter lone_filter;
    PadSnapshot view{};
    view.buttons = 1ULL << StdView;
    frame = view;
    check(!ApplyMenuShortcut(lone, frame, lone_filter, 0) && frame.buttons == view.buttons, "a lone input plays");
    frame = view;
    check(ApplyMenuShortcut(lone, frame, lone_filter, 1000) && frame.buttons == view.buttons, "held opens it");
    frame = view;
    check(!ApplyMenuShortcut(lone, frame, lone_filter, 2000), "once per hold");
    frame = {};
    check(!ApplyMenuShortcut(lone, frame, lone_filter, 2010), "released");
    frame = view;
    check(!ApplyMenuShortcut(lone, frame, lone_filter, 2500), "a new hold starts over");

    // Its storage: damaged text means back to the default, never half a shortcut.
    const auto shortcut_text = SerializePadMenuShortcut(sticks_combo);
    check(ParsePadMenuShortcut(shortcut_text) == sticks_combo, "shortcut round trip");
    check(!ParsePadMenuShortcut(""), "an empty shortcut");
    check(!ParsePadMenuShortcut("0:0,"), "a shortcut without inputs");
    check(!ParsePadMenuShortcut("1:1,1:2,1:3,1:4,"), "too many inputs");
    check(!ParsePadMenuShortcut("1:64,"), "shortcut input out of range");

    // Each controller has its own key; a session id says nothing next time, so it follows the model.
    const auto key_one = PadMappingKey(L"045e_0b12", L"{id-one}");
    check(key_one != PadMappingKey(L"045e_0b12", L"{id-two}"), "two units of a model differ");
    check(key_one == PadMappingKey(L"045e_0b12", L"{id-one}"), "a unit keeps its key");
    check(key_one.starts_with(L"045e_0b12_") && key_one.size() == 9 + 1 + 16, "model and hash");
    check(PadMappingKey(L"0000_0000", L"session:0") == L"0000_0000", "a session id follows the model");
    check(PadMappingKey(L"0000_0000", L"") == L"0000_0000", "no id follows the model");

    // The picker's cards: primary first, unreadable ones out, eight at most.
    using Order = std::vector<std::size_t>;
    check(PadPickerOrder({true, true, true}, 2) == Order{2, 0, 1}, "primary first");
    check(PadPickerOrder({true, false, true}, std::nullopt) == Order{0, 2}, "unusable excluded");
    check(PadPickerOrder({false, true}, 0) == Order{1}, "an unusable primary has no card");
    check(PadPickerOrder(std::vector<bool>(12, true), 11).size() == PadPickerMaxCards, "eight at most");
    check(PadPickerOrder({}, std::nullopt).empty(), "nothing connected");

    // They fit the panel, without touching, whatever the count.
    for (std::size_t count = 1; count <= PadPickerMaxCards; ++count) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto a = PadPickerCardFor(i, count);
            check(a.x >= 120 && a.x + a.w <= 1160.5f && a.y >= 170 && a.y + a.h <= 640, "card inside the panel");
            for (std::size_t j = i + 1; j < count; ++j) {
                const auto b = PadPickerCardFor(j, count);
                check(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y, "cards apart");
            }
        }
    }

    // Moving over them.
    check(MovePadPicker(0, 3, false, false) == 1 && MovePadPicker(2, 3, false, false) == 0, "right wraps");
    check(MovePadPicker(0, 3, false, true) == 2, "left wraps");
    check(MovePadPicker(1, 3, true, false) == 1 && MovePadPicker(1, 3, true, true) == 1, "one row: no vertical move");
    check(MovePadPicker(1, 6, true, false) == 5 && MovePadPicker(5, 6, true, true) == 1, "between rows");
    check(MovePadPicker(3, 6, true, false) == 3, "no card below stays");

    // What leaving would stop at, marked before getting there.
    PadBindings required = StandardPadBindings;
    check(!PadActionRequired(required, 0), "A is bound");
    AssignPadSource(required, 0, {});
    check(PadActionRequired(required, 0), "A is missing");
    check(!PadActionRequired(required, 2), "X is optional");
    AssignPadSource(required, 13, {});
    check(!PadActionRequired(required, 13), "up still has the stick");
    AssignPadSource(required, 20, {});
    check(PadActionRequired(required, 13) && PadActionRequired(required, 20), "up has nothing");
    check(!PadActionRequired(required, MenuShortcutAction), "the shortcut is not a binding");

    std::puts("PASS pad mapping: standard, generic, remap, capture, storage, editor movement, menu shortcut, picker");
    return 0;
}
