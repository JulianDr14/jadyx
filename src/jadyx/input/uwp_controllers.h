// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/Windows.Gaming.Input.h>
#include "jadyx/input/controller_selection.h"
#include "jadyx/input/keyboard_bindings.h"
#include "jadyx/input/pad_mapping.h"

namespace Jadyx {
class ProControllerReader;
struct ControllerReading {
    winrt::Windows::Gaming::Input::GamepadReading gamepad{};
    bool home{}, capture{};
};
struct ControllerDevice {
    std::wstring id;
    std::wstring name;
    bool wireless{};
    winrt::Windows::Gaming::Input::Gamepad pad{nullptr};
    std::shared_ptr<ProControllerReader> pro;
    bool pro_ready{}; // Availability snapshot for UI diffing.
    // Nintendo layout (A right, B bottom). ReadController reports its buttons by their printed
    // letter whichever path reads it, so "by letter" and "by position" are the same on it.
    bool nintendo{};
    // Set only when Windows has no Gamepad for it: a generic pad, read by index.
    winrt::Windows::Gaming::Input::RawGameController raw{nullptr};
    std::wstring mapping_key; // PadMappingKey. Empty: no identity to save under.
    // Null on a standard pad with no custom mapping: it reads as Windows reports it.
    std::shared_ptr<const PadBindings> bindings;
    // Null: View + Menu, read by the same path as any other pad.
    std::shared_ptr<const PadMenuShortcut> menu_shortcut;
    bool custom_mapping{}; // The player saved something for it, not just the first guess.
    explicit operator bool() const;
};
/// The controller as the game plays it: through its mapping, if it has one.
ControllerReading ReadController(const ControllerDevice& device);
/// Windows' Gamepad or the Pro Controller reader, ignoring any mapping.
ControllerReading ReadStandardController(const ControllerDevice& device);
/// For the library: a standard pad ignores its mapping, so a bad one can't lose the menus. A
/// generic pad has no other reading.
ControllerReading ReadMenuController(const ControllerDevice& device);
/// The physical inputs, for the mapping editor to detect the one being pushed.
PadSnapshot ReadPadSnapshot(const ControllerDevice& device);
/// What the game gets from a snapshot, through the controller's mapping or the standard layout.
ControllerReading ReadingFromSnapshot(const ControllerDevice& device, const PadSnapshot& snapshot);
inline PadBindings CurrentPadBindings(const ControllerDevice& device) {
    return device.bindings ? *device.bindings : StandardPadBindings;
}
/// Saved per controller. Both update `device` at once; the catalog follows on its next look.
bool SavePadBindings(ControllerDevice& device, const PadBindings& bindings);
bool ResetPadBindings(ControllerDevice& device);
/// Saved per controller too. Nothing goes back to View + Menu.
bool SavePadMenuShortcut(ControllerDevice& device, const std::optional<PadMenuShortcut>& shortcut);
std::wstring PadSourceName(PadSource source, bool standard);
/// "LS + RS"; with no shortcut, whatever drives View and Menu on this mapping.
std::wstring PadMenuShortcutName(const std::optional<PadMenuShortcut>& shortcut, const PadBindings& bindings,
                                 bool standard);
/// Every controller Windows reports, usable or not: whether the console sees a pad at all.
void LogRawControllers();
// Involution: maps physical labels to Switch labels, and logical prompts back to physical labels.
inline winrt::Windows::Gaming::Input::GamepadButtons MapFaceButtons(
    winrt::Windows::Gaming::Input::GamepadButtons buttons, bool by_position) {
    using B = winrt::Windows::Gaming::Input::GamepadButtons;
    if (!by_position) return buttons;
    const auto face = B::A | B::B | B::X | B::Y;
    auto mapped = buttons & ~face;
    if ((buttons & B::A) != B::None) mapped |= B::B;
    if ((buttons & B::B) != B::None) mapped |= B::A;
    if ((buttons & B::X) != B::None) mapped |= B::Y;
    if ((buttons & B::Y) != B::None) mapped |= B::X;
    return mapped;
}
inline bool SwapControllerFaceButtons(const ControllerDevice& device, bool by_position) {
    return by_position && device.pad && !device.nintendo && !device.bindings;
}

// UI-thread discovery; rate limited, asynchronous and PC-only.
void RefreshProControllers();
bool RunProControllerDecoderSelfTest();
std::vector<ControllerDevice> ProControllers();
// Empty preferred ID means auto; KeyboardControllerId explicitly disables pad.
std::vector<ControllerDevice> EnumerateControllers();
// Opt-in transport gate; call on the activated CoreWindow thread.
void ProbeProControllerHid(std::function<void(std::string)> diagnostic);
std::optional<size_t> SelectController(const std::vector<ControllerDevice>& devices,
                                     std::wstring_view preferred);
std::wstring ControllerLabel(const std::vector<ControllerDevice>& devices,
                             std::wstring_view preferred);
bool SameControllerList(const std::vector<ControllerDevice>& a,
                        const std::vector<ControllerDevice>& b);
/// Which controller to map. Stays open behind the editor, which comes back to it.
struct PadPicker {
    bool open{};
    std::size_t selected{};
    std::wstring primary_id;
    std::vector<ControllerDevice> cards;
    std::array<bool, PadPickerMaxCards> lit{}; // Its controller has an input held right now.
    std::array<PadCapture, PadPickerMaxCards> watch;
    // The controllers other than the primary work it too; the primary reads through `previous`.
    std::array<winrt::Windows::Gaming::Input::GamepadButtons, PadPickerMaxCards> previous{};
};
struct ControllerPanel {
    std::wstring notice;
    KeyboardEditor keyboard;
    PadMapEditor pad_map;
    PadPicker pad_picker;
    bool xbox{};
    bool expanded{};
    size_t choice{};
    std::vector<ControllerDevice> devices;
    BindingEditor* OpenEditor() {
        return keyboard.open ? static_cast<BindingEditor*>(&keyboard) : pad_map.open ? &pad_map : nullptr;
    }
    bool BindingEditorOpen() const { return keyboard.open || pad_map.open || pad_picker.open; }
    void CloseBindingEditors() {
        keyboard.open = keyboard.capturing = pad_map.open = pad_map.capturing = pad_picker.open = false;
    }
};
inline constexpr float ControllerRowTop = 258;
inline constexpr float ControllerRowHeight = 62;
inline constexpr float ControllerChoiceTop = 303;
inline constexpr float ControllerChoiceHeight = 42;
inline constexpr size_t ControllerVisibleChoices = 6;
inline size_t ControllerChoiceFirst(const ControllerPanel& panel) {
    return panel.choice / ControllerVisibleChoices * ControllerVisibleChoices;
}
}
