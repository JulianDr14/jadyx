// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Which physical input of a controller drives each action. The actions are the keyboard's
// (keyboard_bindings.h), so both share one editor. Pure policy, testable on the host: reading the
// hardware into a PadSnapshot lives in uwp_controllers.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jadyx/input/keyboard_bindings.h"

namespace Jadyx {

enum class PadSourceKind : std::uint8_t {
    None, Button, AxisPositive, AxisNegative, SwitchUp, SwitchDown, SwitchLeft, SwitchRight,
};
inline constexpr unsigned PadSourceKindCount = 8;

struct PadSource {
    PadSourceKind kind{};
    std::uint8_t index{};
    constexpr bool operator==(const PadSource&) const = default;
};
using PadBindings = std::array<PadSource, KeyboardActionCount>;

inline constexpr std::size_t PadMaxButtons = 64;
inline constexpr std::size_t PadMaxAxes = 8;
inline constexpr std::size_t PadMaxSwitches = 4;
/// Bits of PadSnapshot::switches.
inline constexpr std::uint8_t PadSwitchUp = 1, PadSwitchDown = 2, PadSwitchLeft = 4, PadSwitchRight = 8;

/// One reading of any controller. Axes are -1..1; a raw stick rests near 0, a trigger may rest at
/// either end.
struct PadSnapshot {
    std::uint64_t buttons{};
    std::array<float, PadMaxAxes> axes{};
    std::array<std::uint8_t, PadMaxSwitches> switches{};
};

/// How far a source is pushed, 0..1.
inline float PadSourceValue(PadSource source, const PadSnapshot& snapshot) {
    const auto on_switch = [&](std::uint8_t bit) {
        return source.index < PadMaxSwitches && (snapshot.switches[source.index] & bit) ? 1.0f : 0.0f;
    };
    switch (source.kind) {
    case PadSourceKind::Button:
        return source.index < PadMaxButtons && ((snapshot.buttons >> source.index) & 1) ? 1.0f : 0.0f;
    case PadSourceKind::AxisPositive:
        return source.index < PadMaxAxes && snapshot.axes[source.index] > 0 ? snapshot.axes[source.index] : 0.0f;
    case PadSourceKind::AxisNegative:
        return source.index < PadMaxAxes && snapshot.axes[source.index] < 0 ? -snapshot.axes[source.index] : 0.0f;
    case PadSourceKind::SwitchUp: return on_switch(PadSwitchUp);
    case PadSourceKind::SwitchDown: return on_switch(PadSwitchDown);
    case PadSourceKind::SwitchLeft: return on_switch(PadSwitchLeft);
    case PadSourceKind::SwitchRight: return on_switch(PadSwitchRight);
    case PadSourceKind::None: break;
    }
    return 0.0f;
}

/// What the bindings make of a snapshot: buttons as VirtualButton bits, sticks before the dead zone.
struct PadOutput {
    std::uint32_t buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{};
};

inline constexpr float PadPressedAbove = 0.5f;

inline PadOutput ApplyPadBindings(const PadBindings& bindings, const PadSnapshot& snapshot) {
    PadOutput output;
    for (std::size_t action = 0; action < KeyboardButtonCount; ++action) {
        if (PadSourceValue(bindings[action], snapshot) > PadPressedAbove) output.buttons |= 1U << action;
    }
    const auto axis = [&](std::size_t positive, std::size_t negative) {
        return PadSourceValue(bindings[positive], snapshot) - PadSourceValue(bindings[negative], snapshot);
    };
    // Actions 20-27: left stick up, down, left, right, then the right stick's.
    output.left_y = axis(20, 21);
    output.left_x = axis(23, 22);
    output.right_y = axis(24, 25);
    output.right_x = axis(27, 26);
    return output;
}

// How a Windows Gamepad (or the Pro Controller reader) fills a snapshot.
enum StandardPadButton : std::uint8_t {
    StdA, StdB, StdX, StdY, StdLeftShoulder, StdRightShoulder, StdLeftThumb, StdRightThumb,
    StdMenu, StdView, StdDPadUp, StdDPadDown, StdDPadLeft, StdDPadRight, StdHome, StdCapture,
    StdButtonCount,
};
enum StandardPadAxis : std::uint8_t {
    StdLeftX, StdLeftY, StdRightX, StdRightY, StdLeftTrigger, StdRightTrigger, StdAxisCount,
};

/// What a standard pad does with no custom mapping: the same as the unmapped path.
inline constexpr PadBindings StandardPadBindings = [] {
    using K = PadSourceKind;
    const auto button = [](std::uint8_t index) { return PadSource{K::Button, index}; };
    const auto plus = [](std::uint8_t index) { return PadSource{K::AxisPositive, index}; };
    const auto minus = [](std::uint8_t index) { return PadSource{K::AxisNegative, index}; };
    PadBindings out{};
    out[0] = button(StdA); out[1] = button(StdB); out[2] = button(StdX); out[3] = button(StdY);
    out[4] = button(StdLeftThumb); out[5] = button(StdRightThumb);
    out[6] = button(StdLeftShoulder); out[7] = button(StdRightShoulder);
    out[8] = plus(StdLeftTrigger); out[9] = plus(StdRightTrigger);
    out[10] = button(StdMenu); out[11] = button(StdView);
    out[12] = button(StdDPadLeft); out[13] = button(StdDPadUp);
    out[14] = button(StdDPadRight); out[15] = button(StdDPadDown);
    out[18] = button(StdHome); out[19] = button(StdCapture);
    out[20] = plus(StdLeftY); out[21] = minus(StdLeftY); out[22] = minus(StdLeftX); out[23] = plus(StdLeftX);
    out[24] = plus(StdRightY); out[25] = minus(StdRightY); out[26] = minus(StdRightX); out[27] = plus(StdRightX);
    return out;
}();

/// A first guess for a controller Windows does not map itself. HID fixes no button order, so this
/// follows the common Xbox-like one and the player corrects the rest in the editor. `axis_rest` is
/// each axis at rest: an axis off-center is a trigger, which must never hold a stick over.
inline PadBindings GenericPadBindings(std::size_t button_count, std::span<const float> axis_rest,
                                      std::size_t switch_count) {
    using K = PadSourceKind;
    constexpr float CenteredBelow = 0.25f;
    const auto centered = [&](std::size_t axis) {
        return axis < axis_rest.size() && std::abs(axis_rest[axis]) < CenteredBelow;
    };
    PadBindings out{};
    constexpr std::array<std::size_t, 10> ButtonOrder{0, 1, 2, 3, 6, 7, 11, 10, 4, 5};
    for (std::size_t i = 0; i < ButtonOrder.size() && i < button_count; ++i) {
        out[ButtonOrder[i]] = {K::Button, static_cast<std::uint8_t>(i)};
    }
    if (switch_count > 0) {
        out[12] = {K::SwitchLeft, 0}; out[13] = {K::SwitchUp, 0};
        out[14] = {K::SwitchRight, 0}; out[15] = {K::SwitchDown, 0};
    }
    // HID's Y axes grow downwards, the opposite of a stick's up.
    const auto stick = [&](std::size_t first_action, std::uint8_t x, std::uint8_t y) {
        if (!centered(x) || !centered(y)) return;
        out[first_action] = {K::AxisNegative, y}; out[first_action + 1] = {K::AxisPositive, y};
        out[first_action + 2] = {K::AxisNegative, x}; out[first_action + 3] = {K::AxisPositive, x};
    };
    stick(20, 0, 1);
    stick(24, 2, 3);
    return out;
}

/// The other action already driven by this source, if any.
inline std::optional<std::size_t> PadSourceOwner(const PadBindings& bindings, std::size_t action, PadSource source) {
    if (source.kind == PadSourceKind::None) return std::nullopt;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (i != action && bindings[i] == source) return i;
    }
    return std::nullopt;
}

/// One source per action (the two directions of an axis are separate sources). A source already in
/// use is refused: the player frees it first, so no action ever loses its input unnoticed.
inline bool AssignPadSource(PadBindings& bindings, std::size_t action, PadSource source) {
    if (action >= bindings.size() || PadSourceOwner(bindings, action, source)) return false;
    bindings[action] = source;
    return true;
}

/// The first action a mapping can't be saved without: A and B to confirm and go back, and each
/// direction on the d-pad or the left stick to move. A generic pad's menus read it through this
/// mapping, and in a game a pad without them gets stuck on the first prompt.
inline std::optional<std::size_t> PadMissingAction(const PadBindings& bindings) {
    const auto empty = [&](std::size_t action) { return bindings[action].kind == PadSourceKind::None; };
    if (empty(0)) return std::size_t{0};
    if (empty(1)) return std::size_t{1};
    static constexpr std::array<std::array<std::size_t, 2>, 4> Directions{{{13, 20}, {15, 21}, {12, 22}, {14, 23}}};
    for (const auto& pair : Directions) {
        if (empty(pair[0]) && empty(pair[1])) return pair[0];
    }
    return std::nullopt;
}

/// Whether leaving the editor would stop at this empty action, to mark it before that happens.
inline bool PadActionRequired(const PadBindings& bindings, std::size_t action) {
    const auto empty = [&](std::size_t i) { return bindings[i].kind == PadSourceKind::None; };
    if (action >= bindings.size() || !empty(action)) return false;
    if (action <= 1) return true;
    static constexpr std::array<std::array<std::size_t, 2>, 4> Directions{{{13, 20}, {15, 21}, {12, 22}, {14, 23}}};
    return std::ranges::any_of(Directions, [&](const auto& pair) {
        return (pair[0] == action || pair[1] == action) && empty(pair[0]) && empty(pair[1]);
    });
}

/// Finds the input the player pushes while the editor waits. Whatever was already held when it
/// started (the A that opened the capture) counts only after being released.
class PadCapture {
public:
    void Begin(const PadSnapshot& current) { baseline = current; }
    std::optional<PadSource> Detect(const PadSnapshot& current) {
        constexpr float MinimumTravel = 0.5f, MinimumReach = 0.6f;
        baseline.buttons &= current.buttons;
        const std::uint64_t pressed = current.buttons & ~baseline.buttons;
        for (std::size_t i = 0; i < PadMaxButtons; ++i) {
            if ((pressed >> i) & 1) return PadSource{PadSourceKind::Button, static_cast<std::uint8_t>(i)};
        }
        for (std::size_t i = 0; i < PadMaxSwitches; ++i) {
            baseline.switches[i] &= current.switches[i];
            const std::uint8_t now = current.switches[i] & ~baseline.switches[i];
            const auto index = static_cast<std::uint8_t>(i);
            if (now & PadSwitchUp) return PadSource{PadSourceKind::SwitchUp, index};
            if (now & PadSwitchDown) return PadSource{PadSourceKind::SwitchDown, index};
            if (now & PadSwitchLeft) return PadSource{PadSourceKind::SwitchLeft, index};
            if (now & PadSwitchRight) return PadSource{PadSourceKind::SwitchRight, index};
        }
        for (std::size_t i = 0; i < PadMaxAxes; ++i) {
            const float value = current.axes[i];
            if (std::abs(value - baseline.axes[i]) > MinimumTravel && std::abs(value) > MinimumReach) {
                return PadSource{value > 0 ? PadSourceKind::AxisPositive : PadSourceKind::AxisNegative,
                                 static_cast<std::uint8_t>(i)};
            }
        }
        return std::nullopt;
    }

private:
    PadSnapshot baseline;
};

/// Presses or releases a source in a snapshot; a released axis direction rests at the center.
inline void SetPadSource(PadSnapshot& snapshot, PadSource source, bool held) {
    const auto set_switch = [&](std::uint8_t bit) {
        if (source.index >= PadMaxSwitches) return;
        auto& value = snapshot.switches[source.index];
        value = held ? value | bit : value & ~bit;
    };
    switch (source.kind) {
    case PadSourceKind::Button:
        if (source.index >= PadMaxButtons) return;
        snapshot.buttons = held ? snapshot.buttons | (1ULL << source.index)
                                : snapshot.buttons & ~(1ULL << source.index);
        return;
    case PadSourceKind::AxisPositive:
        if (source.index >= PadMaxAxes) return;
        if (held) snapshot.axes[source.index] = 1.0f;
        else snapshot.axes[source.index] = std::min(snapshot.axes[source.index], 0.0f);
        return;
    case PadSourceKind::AxisNegative:
        if (source.index >= PadMaxAxes) return;
        if (held) snapshot.axes[source.index] = -1.0f;
        else snapshot.axes[source.index] = std::max(snapshot.axes[source.index], 0.0f);
        return;
    case PadSourceKind::SwitchUp: return set_switch(PadSwitchUp);
    case PadSourceKind::SwitchDown: return set_switch(PadSwitchDown);
    case PadSourceKind::SwitchLeft: return set_switch(PadSwitchLeft);
    case PadSourceKind::SwitchRight: return set_switch(PadSwitchRight);
    case PadSourceKind::None: return;
    }
}

/// The inputs that open the in-game menu when the player replaced the default View + Menu.
struct PadMenuShortcut {
    static constexpr std::size_t MaxInputs = 3;
    std::array<PadSource, MaxInputs> sources{};
    std::size_t count{};
    constexpr bool operator==(const PadMenuShortcut&) const = default;
};

/// A combination works like View + Menu: its inputs reach the game only if it is not being formed,
/// so a press is delayed by PendingMs and a tap still arrives. A single input can't be told apart
/// from normal use, so it keeps playing: a press still reaches the game, and holding it opens the menu.
class PadShortcutFilter {
public:
    static constexpr std::uint32_t PendingMs = 100;
    static constexpr std::uint32_t HoldMs = 1000;

    /// `held` has one bit per input and comes back with what the game may see. True when it fires.
    bool Filter(std::uint8_t& held, std::size_t count, std::uint32_t now_ms) {
        const auto all = static_cast<std::uint8_t>((1U << count) - 1);
        if (count == 1) {
            if (!held) {
                state = State::Idle;
                return false;
            }
            if (state != State::Combo) Enter(now_ms);
            return Fires(true, now_ms);
        }
        if (held == all && state != State::Combo) Enter(now_ms);
        switch (state) {
        case State::Idle:
            if (!held) return false;
            state = State::Pending;
            since = now_ms;
            remembered = held;
            held = 0;
            return false;
        case State::Pending:
            if (!held) {
                // Released before the window closed: a tap, delivered now and released next poll.
                state = State::Idle;
                since = NotPending;
                held = remembered;
                return false;
            }
            remembered = held;
            if (now_ms - since < PendingMs) {
                held = 0;
                return false;
            }
            state = State::Passing;
            since = NotPending;
            return false;
        case State::Passing:
            if (!held) state = State::Idle;
            return false;
        case State::Combo: {
            // Letting go of part of it starts the second over.
            if (held != all) since = now_ms;
            const bool fire = Fires(held == all, now_ms);
            if (!held) {
                state = State::Idle;
                since = NotPending;
            }
            held = 0;
            return fire;
        }
        }
        return false;
    }

private:
    static constexpr std::uint32_t NotPending = ~0U;
    enum class State : std::uint8_t { Idle, Pending, Passing, Combo };
    void Enter(std::uint32_t now_ms) {
        state = State::Combo;
        since = now_ms;
        fired = false;
    }
    bool Fires(bool complete, std::uint32_t now_ms) {
        const bool fire = !fired && complete && now_ms - since >= HoldMs;
        if (fire) fired = true;
        return fire;
    }
    State state{State::Idle};
    std::uint32_t since{NotPending};
    std::uint8_t remembered{};
    bool fired{};
};

/// Filters the shortcut's inputs in `snapshot`. True when the menu should toggle.
inline bool ApplyMenuShortcut(const PadMenuShortcut& shortcut, PadSnapshot& snapshot,
                              PadShortcutFilter& filter, std::uint32_t now_ms) {
    std::uint8_t held = 0;
    for (std::size_t i = 0; i < shortcut.count; ++i) {
        if (PadSourceValue(shortcut.sources[i], snapshot) > PadPressedAbove) held |= 1U << i;
    }
    const std::uint8_t before = held;
    const bool fire = filter.Filter(held, shortcut.count, now_ms);
    for (std::size_t i = 0; i < shortcut.count; ++i) {
        const bool now = (held >> i) & 1;
        if (now != (((before >> i) & 1) != 0)) SetPadSource(snapshot, shortcut.sources[i], now);
    }
    return fire;
}

/// Gathers every input pressed together; letting go of any of them ends the combination.
class PadShortcutCapture {
public:
    void Begin(const PadSnapshot& current) {
        capture.Begin(current);
        shortcut = {};
    }
    std::optional<PadMenuShortcut> Update(const PadSnapshot& current) {
        PadSnapshot rest = current;
        for (std::size_t i = 0; i < shortcut.count; ++i) {
            if (PadSourceValue(shortcut.sources[i], current) <= PadPressedAbove) return shortcut;
            SetPadSource(rest, shortcut.sources[i], false);
        }
        while (shortcut.count < PadMenuShortcut::MaxInputs) {
            const auto source = capture.Detect(rest);
            if (!source) break;
            shortcut.sources[shortcut.count++] = *source;
            SetPadSource(rest, *source, false);
        }
        return std::nullopt;
    }

private:
    PadCapture capture;
    PadMenuShortcut shortcut;
};

/// "kind:index," for every action, in order.
inline std::string SerializePadSources(std::span<const PadSource> sources) {
    std::string out;
    for (const PadSource& source : sources) {
        out += std::to_string(static_cast<unsigned>(source.kind)) + ':' + std::to_string(source.index) + ',';
    }
    return out;
}
inline std::string SerializePadBindings(const PadBindings& bindings) { return SerializePadSources(bindings); }
inline std::string SerializePadMenuShortcut(const PadMenuShortcut& shortcut) {
    return SerializePadSources(std::span{shortcut.sources}.first(shortcut.count));
}

/// Takes one "kind:index," off the front of `text`.
inline std::optional<PadSource> ParsePadSource(std::string_view& text) {
    const auto number = [&](char end) -> std::optional<unsigned> {
        unsigned value = 0;
        std::size_t digits = 0;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9' && digits < 3) {
            value = value * 10 + static_cast<unsigned>(text[digits] - '0');
            ++digits;
        }
        if (digits == 0 || digits >= text.size() || text[digits] != end) return std::nullopt;
        text.remove_prefix(digits + 1);
        return value;
    };
    const auto kind = number(':');
    if (!kind || *kind >= PadSourceKindCount) return std::nullopt;
    const auto index = number(',');
    if (!index) return std::nullopt;
    PadSource source;
    source.kind = static_cast<PadSourceKind>(*kind);
    switch (source.kind) {
    case PadSourceKind::None: break;
    case PadSourceKind::Button:
        if (*index >= PadMaxButtons) return std::nullopt;
        break;
    case PadSourceKind::AxisPositive:
    case PadSourceKind::AxisNegative:
        if (*index >= PadMaxAxes) return std::nullopt;
        break;
    default:
        if (*index >= PadMaxSwitches) return std::nullopt;
        break;
    }
    source.index = static_cast<std::uint8_t>(*index);
    return source;
}

/// Anything malformed, short or out of range is rejected whole.
inline std::optional<PadBindings> ParsePadBindings(std::string_view text) {
    PadBindings out{};
    for (PadSource& source : out) {
        const auto parsed = ParsePadSource(text);
        if (!parsed) return std::nullopt;
        source = *parsed;
    }
    if (!text.empty()) return std::nullopt;
    return out;
}

/// Likewise: one to MaxInputs real inputs.
inline std::optional<PadMenuShortcut> ParsePadMenuShortcut(std::string_view text) {
    PadMenuShortcut out;
    while (!text.empty()) {
        const auto parsed = ParsePadSource(text);
        if (!parsed || parsed->kind == PadSourceKind::None || out.count == PadMenuShortcut::MaxInputs)
            return std::nullopt;
        out.sources[out.count++] = *parsed;
    }
    if (out.count == 0) return std::nullopt;
    return out;
}

/// What a controller's mapping is saved under: its own, so two pads of one model can differ. An id
/// the console makes up for the session says nothing next time, so those pads follow the model.
/// Hashed because settings keys are short and Windows gives no limit for the id.
inline std::wstring PadMappingKey(std::wstring_view model, std::wstring_view id) {
    if (id.empty() || id.starts_with(L"session:")) return std::wstring{model};
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t c : id) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    std::wstring key{model};
    key += L'_';
    for (int shift = 60; shift >= 0; shift -= 4) key += L"0123456789abcdef"[(hash >> shift) & 15];
    return key;
}

struct PadMapEditor : BindingEditor {
    PadMapEditor() { page_count = 4; }
    PadBindings bindings{StandardPadBindings}; // A draft: the pad keeps its saved mapping until leaving.
    std::optional<PadMenuShortcut> menu_shortcut; // Empty: View + Menu.
    std::wstring device_name;
    std::wstring device_id;
    std::size_t number{1}; // Its card's number in the picker.
    bool standard{};    // Named buttons (A, LB...) instead of numbered ones.
    PadCapture capture;
    PadShortcutCapture shortcut_capture;
};

/// The controller picker: one card per pad, the most the console can have connected.
inline constexpr std::size_t PadPickerMaxCards = 8;

/// The cards' order, as indices into the controller list: the primary pad first, then the others
/// as listed. Controllers that can't be read get no card.
inline std::vector<std::size_t> PadPickerOrder(const std::vector<bool>& usable, std::optional<std::size_t> primary) {
    std::vector<std::size_t> cards;
    if (primary && *primary < usable.size() && usable[*primary]) cards.push_back(*primary);
    for (std::size_t i = 0; i < usable.size() && cards.size() < PadPickerMaxCards; ++i) {
        if (usable[i] && (!primary || i != *primary)) cards.push_back(i);
    }
    return cards;
}

/// Up to four cards share a row; fewer cards are bigger.
inline std::size_t PadPickerColumns(std::size_t count) { return std::clamp<std::size_t>(count, 1, 4); }

inline KeyboardTile PadPickerCardFor(std::size_t index, std::size_t count) {
    constexpr float Left = 120, Width = 1040, Gap = 16;
    const std::size_t columns = PadPickerColumns(count);
    const bool one_row = count <= columns;
    const float w = (Width - Gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float h = one_row ? 300.0f : 198.0f;
    const float top = one_row ? 240.0f : 196.0f;
    return {Left + static_cast<float>(index % columns) * (w + Gap),
            top + static_cast<float>(index / columns) * (h + Gap), w, h};
}

/// Left and right go along the cards; up and down change row and stay in place on a single row.
inline std::size_t MovePadPicker(std::size_t selected, std::size_t count, bool vertical, bool reverse) {
    if (count == 0) return 0;
    if (!vertical) return (selected + (reverse ? count - 1 : 1)) % count;
    const std::size_t columns = PadPickerColumns(count);
    if (reverse) return selected >= columns ? selected - columns : selected;
    return selected + columns < count ? selected + columns : selected;
}

} // namespace Jadyx
