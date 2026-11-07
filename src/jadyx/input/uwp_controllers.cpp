// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/input/uwp_controllers.h"
#include "jadyx/i18n/localization.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>
#include "common/common_types.h"
#include "common/logging.h"

namespace Jadyx {
namespace {
using winrt::Windows::Gaming::Input::GameControllerSwitchPosition;
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::Gaming::Input::RawGameController;

/// Fewer inputs than this is not a pad: a headset, a remote, a pedal.
bool EnoughToPlay(const RawGameController& raw) {
    return raw.ButtonCount() >= 4 && (raw.AxisCount() >= 2 || raw.SwitchCount() >= 1);
}

std::uint8_t SwitchBits(GameControllerSwitchPosition position) {
    using P = GameControllerSwitchPosition;
    switch (position) {
    case P::Up: return PadSwitchUp;
    case P::UpRight: return PadSwitchUp | PadSwitchRight;
    case P::Right: return PadSwitchRight;
    case P::DownRight: return PadSwitchDown | PadSwitchRight;
    case P::Down: return PadSwitchDown;
    case P::DownLeft: return PadSwitchDown | PadSwitchLeft;
    case P::Left: return PadSwitchLeft;
    case P::UpLeft: return PadSwitchUp | PadSwitchLeft;
    default: return 0;
    }
}

PadSnapshot ReadRawSnapshot(const RawGameController& raw) {
    // The input thread polls every 4 ms: these only allocate the first time. Bytes, because
    // std::vector<bool> is packed and array_view needs real bools.
    thread_local std::vector<std::uint8_t> buttons;
    thread_local std::vector<GameControllerSwitchPosition> switches;
    thread_local std::vector<double> axes;
    buttons.resize(raw.ButtonCount());
    switches.resize(raw.SwitchCount());
    axes.resize(raw.AxisCount());
    auto* const button_data = reinterpret_cast<bool*>(buttons.data());
    raw.GetCurrentReading(winrt::array_view<bool>(button_data, button_data + buttons.size()),
                          winrt::array_view<GameControllerSwitchPosition>{switches},
                          winrt::array_view<double>{axes});
    PadSnapshot snapshot;
    for (std::size_t i = 0; i < std::min(buttons.size(), PadMaxButtons); ++i) {
        if (buttons[i]) snapshot.buttons |= 1ULL << i;
    }
    for (std::size_t i = 0; i < std::min(axes.size(), PadMaxAxes); ++i) {
        snapshot.axes[i] = static_cast<float>(axes[i] * 2.0 - 1.0); // Windows reports 0..1.
    }
    for (std::size_t i = 0; i < std::min(switches.size(), PadMaxSwitches); ++i) {
        snapshot.switches[i] = SwitchBits(switches[i]);
    }
    return snapshot;
}

PadSnapshot StandardSnapshot(const ControllerReading& reading) {
    static constexpr std::array<GamepadButtons, StdHome> Order{
        GamepadButtons::A, GamepadButtons::B, GamepadButtons::X, GamepadButtons::Y,
        GamepadButtons::LeftShoulder, GamepadButtons::RightShoulder,
        GamepadButtons::LeftThumbstick, GamepadButtons::RightThumbstick,
        GamepadButtons::Menu, GamepadButtons::View,
        GamepadButtons::DPadUp, GamepadButtons::DPadDown, GamepadButtons::DPadLeft, GamepadButtons::DPadRight,
    };
    const auto& pad = reading.gamepad;
    PadSnapshot snapshot;
    for (std::size_t i = 0; i < Order.size(); ++i) {
        if ((pad.Buttons & Order[i]) == Order[i]) snapshot.buttons |= 1ULL << i;
    }
    if (reading.home) snapshot.buttons |= 1ULL << StdHome;
    if (reading.capture) snapshot.buttons |= 1ULL << StdCapture;
    snapshot.axes[StdLeftX] = static_cast<float>(pad.LeftThumbstickX);
    snapshot.axes[StdLeftY] = static_cast<float>(pad.LeftThumbstickY);
    snapshot.axes[StdRightX] = static_cast<float>(pad.RightThumbstickX);
    snapshot.axes[StdRightY] = static_cast<float>(pad.RightThumbstickY);
    snapshot.axes[StdLeftTrigger] = static_cast<float>(pad.LeftTrigger);
    snapshot.axes[StdRightTrigger] = static_cast<float>(pad.RightTrigger);
    return snapshot;
}

/// A mapped controller, as the rest of the frontend reads any other.
ControllerReading MappedReading(const PadOutput& output) {
    static constexpr std::array<GamepadButtons, 16> ByAction{
        GamepadButtons::A, GamepadButtons::B, GamepadButtons::X, GamepadButtons::Y,
        GamepadButtons::LeftThumbstick, GamepadButtons::RightThumbstick,
        GamepadButtons::LeftShoulder, GamepadButtons::RightShoulder,
        GamepadButtons::None, GamepadButtons::None, // ZL and ZR are the triggers below.
        GamepadButtons::Menu, GamepadButtons::View,
        GamepadButtons::DPadLeft, GamepadButtons::DPadUp, GamepadButtons::DPadRight, GamepadButtons::DPadDown,
    };
    ControllerReading reading;
    for (std::size_t action = 0; action < ByAction.size(); ++action) {
        if ((output.buttons >> action) & 1) reading.gamepad.Buttons |= ByAction[action];
    }
    reading.gamepad.LeftTrigger = (output.buttons >> 8) & 1;
    reading.gamepad.RightTrigger = (output.buttons >> 9) & 1;
    reading.gamepad.LeftThumbstickX = output.left_x;
    reading.gamepad.LeftThumbstickY = output.left_y;
    reading.gamepad.RightThumbstickX = output.right_x;
    reading.gamepad.RightThumbstickY = output.right_y;
    reading.home = (output.buttons >> 18) & 1;
    reading.capture = (output.buttons >> 19) & 1;
    return reading;
}

std::wstring PadModel(unsigned vendor, unsigned product) {
    wchar_t text[16];
    std::swprintf(text, 16, L"%04x_%04x", vendor, product);
    return text;
}

winrt::hstring PadBindingsKey(const std::wstring& key) {
    return winrt::hstring{L"pad_bindings_v1_" + key};
}

/// The first guess for a generic pad; a trigger's axis rests off-center and stays off the sticks.
std::shared_ptr<const PadBindings> GenericBindingsFor(const RawGameController& raw) {
    const auto snapshot = ReadRawSnapshot(raw);
    const std::size_t axes = std::min<std::size_t>(raw.AxisCount(), PadMaxAxes);
    // Every axis exactly at the bottom means no report arrived yet: nothing is known about them.
    const bool reported = std::any_of(snapshot.axes.begin(), snapshot.axes.begin() + axes,
                                      [](float value) { return value != -1.0f; });
    std::array<float, PadMaxAxes> rest{};
    if (reported) rest = snapshot.axes;
    return std::make_shared<const PadBindings>(GenericPadBindings(
        raw.ButtonCount(), std::span<const float>{rest.data(), axes}, raw.SwitchCount()));
}

/// EnumerateControllers runs twice a second on two threads: settings are read once per controller,
/// and the pointer stays the same until the mapping changes, which is how the catalog notices.
struct PadBindingStore {
    std::mutex mutex;
    std::map<std::wstring, std::shared_ptr<const PadBindings>> pads;
    std::map<std::wstring, std::shared_ptr<const PadMenuShortcut>> menu_shortcuts;
    std::set<std::wstring> saved; // Pads whose bindings came from the player.
};
PadBindingStore& PadBindingsStore() { static PadBindingStore store; return store; }

winrt::hstring PadMenuShortcutKey(const std::wstring& key) {
    return winrt::hstring{L"pad_menu_shortcut_v1_" + key};
}

std::shared_ptr<const PadMenuShortcut> PadMenuShortcutFor(const std::wstring& mapping_key) {
    auto& store = PadBindingsStore();
    std::scoped_lock lock{store.mutex};
    if (const auto found = store.menu_shortcuts.find(mapping_key); found != store.menu_shortcuts.end())
        return found->second;
    std::shared_ptr<const PadMenuShortcut> shortcut;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto key = PadMenuShortcutKey(mapping_key);
        if (values.HasKey(key)) {
            const auto stored = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(key)));
            if (const auto parsed = ParsePadMenuShortcut(stored))
                shortcut = std::make_shared<const PadMenuShortcut>(*parsed);
        }
    } catch (const winrt::hresult_error&) {}
    store.menu_shortcuts[mapping_key] = shortcut;
    return shortcut;
}

std::shared_ptr<const PadBindings> PadBindingsFor(const std::wstring& mapping_key, const RawGameController& raw) {
    auto& store = PadBindingsStore();
    std::scoped_lock lock{store.mutex};
    if (const auto found = store.pads.find(mapping_key); found != store.pads.end()) return found->second;
    std::shared_ptr<const PadBindings> bindings;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto key = PadBindingsKey(mapping_key);
        if (values.HasKey(key)) {
            const auto stored = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(key)));
            if (const auto parsed = ParsePadBindings(stored)) {
                bindings = std::make_shared<const PadBindings>(*parsed);
                store.saved.insert(mapping_key);
            }
        }
    } catch (const winrt::hresult_error&) {}
    if (!bindings && raw) bindings = GenericBindingsFor(raw);
    store.pads[mapping_key] = bindings;
    return bindings;
}

/// Gives a listed controller its identity and whatever was saved for it.
void AttachMapping(ControllerDevice& device, unsigned vendor, unsigned product) {
    device.mapping_key = PadMappingKey(PadModel(vendor, product), device.id);
    device.bindings = PadBindingsFor(device.mapping_key, device.raw);
    device.menu_shortcut = PadMenuShortcutFor(device.mapping_key);
    auto& store = PadBindingsStore();
    std::scoped_lock lock{store.mutex};
    device.custom_mapping = device.menu_shortcut || store.saved.contains(device.mapping_key);
}
}

ControllerReading ReadController(const ControllerDevice& device) {
    if (!device.bindings) return ReadStandardController(device);
    return MappedReading(ApplyPadBindings(*device.bindings, ReadPadSnapshot(device)));
}
ControllerReading ReadMenuController(const ControllerDevice& device) {
    return device.raw ? ReadController(device) : ReadStandardController(device);
}
PadSnapshot ReadPadSnapshot(const ControllerDevice& device) {
    return device.raw ? ReadRawSnapshot(device.raw) : StandardSnapshot(ReadStandardController(device));
}
ControllerReading ReadingFromSnapshot(const ControllerDevice& device, const PadSnapshot& snapshot) {
    return MappedReading(ApplyPadBindings(CurrentPadBindings(device), snapshot));
}

bool SavePadBindings(ControllerDevice& device, const PadBindings& bindings) {
    if (device.mapping_key.empty()) return false;
    // A standard pad back on its own layout returns to the path with no mapping at all.
    if (!device.raw && bindings == StandardPadBindings) return ResetPadBindings(device);
    try {
        auto saved = std::make_shared<const PadBindings>(bindings);
        winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values().Insert(
            PadBindingsKey(device.mapping_key), winrt::box_value(winrt::to_hstring(SerializePadBindings(bindings))));
        auto& store = PadBindingsStore();
        std::scoped_lock lock{store.mutex};
        store.pads[device.mapping_key] = saved;
        store.saved.insert(device.mapping_key);
        device.bindings = std::move(saved);
        return true;
    } catch (const winrt::hresult_error&) { return false; }
}

bool ResetPadBindings(ControllerDevice& device) {
    if (device.mapping_key.empty()) return false;
    try {
        auto bindings = device.raw ? GenericBindingsFor(device.raw) : std::shared_ptr<const PadBindings>{};
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (const auto key = PadBindingsKey(device.mapping_key); values.HasKey(key)) values.Remove(key);
        auto& store = PadBindingsStore();
        std::scoped_lock lock{store.mutex};
        store.pads[device.mapping_key] = bindings;
        store.saved.erase(device.mapping_key);
        device.bindings = std::move(bindings);
        return true;
    } catch (const winrt::hresult_error&) { return false; }
}

bool SavePadMenuShortcut(ControllerDevice& device, const std::optional<PadMenuShortcut>& shortcut) {
    if (device.mapping_key.empty()) return false;
    try {
        const auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        const auto key = PadMenuShortcutKey(device.mapping_key);
        std::shared_ptr<const PadMenuShortcut> saved;
        if (shortcut) {
            values.Insert(key, winrt::box_value(winrt::to_hstring(SerializePadMenuShortcut(*shortcut))));
            saved = std::make_shared<const PadMenuShortcut>(*shortcut);
        } else if (values.HasKey(key)) {
            values.Remove(key);
        }
        auto& store = PadBindingsStore();
        std::scoped_lock lock{store.mutex};
        store.menu_shortcuts[device.mapping_key] = saved;
        device.menu_shortcut = std::move(saved);
        return true;
    } catch (const winrt::hresult_error&) { return false; }
}

std::wstring PadMenuShortcutName(const std::optional<PadMenuShortcut>& shortcut, const PadBindings& bindings,
                                 bool standard) {
    if (!shortcut) return PadSourceName(bindings[11], standard) + L" + " + PadSourceName(bindings[10], standard);
    std::wstring name;
    for (std::size_t i = 0; i < shortcut->count; ++i) {
        if (i) name += L" + ";
        name += PadSourceName(shortcut->sources[i], standard);
    }
    return name;
}

std::wstring PadSourceName(PadSource source, bool standard) {
    using K = PadSourceKind;
    const std::wstring number = std::to_wstring(source.index + 1);
    switch (source.kind) {
    case K::None: return std::wstring{I18n::Tr(I18n::Text::KeyUnassigned)};
    case K::Button: {
        static constexpr std::array<const wchar_t*, StdButtonCount> Names{
            L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LS", L"RS", L"Menu", L"View",
            L"D-Pad ^", L"D-Pad v", L"D-Pad <", L"D-Pad >", L"Home", L"Capture"};
        if (standard && source.index < Names.size()) return Names[source.index];
        return I18n::Format(I18n::Text::PadSourceButton, {number});
    }
    case K::AxisPositive:
    case K::AxisNegative: {
        const bool positive = source.kind == K::AxisPositive;
        static constexpr std::array<const wchar_t*, StdAxisCount> Names{
            L"LS X", L"LS Y", L"RS X", L"RS Y", L"LT", L"RT"};
        if (standard && source.index >= StdLeftTrigger && source.index < Names.size() && positive)
            return Names[source.index];
        if (standard && source.index < Names.size()) return std::wstring{Names[source.index]} + (positive ? L"+" : L"-");
        return I18n::Format(I18n::Text::PadSourceAxis, {number}) + (positive ? L"+" : L"-");
    }
    case K::SwitchUp: return L"Hat " + number + L" ^";
    case K::SwitchDown: return L"Hat " + number + L" v";
    case K::SwitchLeft: return L"Hat " + number + L" <";
    case K::SwitchRight: return L"Hat " + number + L" >";
    }
    return {};
}

void LogRawControllers() {
    try {
        using winrt::Windows::Gaming::Input::Gamepad;
        const auto list = RawGameController::RawGameControllers();
        LOG_INFO(Input, "Controllers: {} reported by Windows, {} gamepads", list.Size(), Gamepad::Gamepads().Size());
        for (const auto& raw : list) {
            LOG_INFO(Input, "Controller: {:04x}:{:04x} buttons={} axes={} switches={} gamepad={} wireless={}",
                     raw.HardwareVendorId(), raw.HardwareProductId(), raw.ButtonCount(), raw.AxisCount(),
                     raw.SwitchCount(), bool(Gamepad::FromGameController(raw)), raw.IsWireless());
        }
    } catch (const winrt::hresult_error& e) {
        LOG_WARNING(Input, "Controllers: can't list them, HRESULT {:08x}", static_cast<u32>(e.code()));
    }
}

namespace {
winrt::fire_and_forget ProbeHid(std::function<void(std::string)> diagnostic) {
    using namespace winrt::Windows::Devices::HumanInterfaceDevice;
    using winrt::Windows::Devices::Enumeration::DeviceInformation;
    // Bluetooth pads announce themselves as gamepads (usage 5), USB ones as joysticks (usage 4).
    for (const u16 usage : {u16{4}, u16{5}}) try {
        const auto devices = co_await DeviceInformation::FindAllAsync(
            HidDevice::GetDeviceSelector(1, usage, 0x057e, 0x2009));
        diagnostic("Pro HID gate: usage " + std::to_string(usage) + " devices=" +
                   std::to_string(devices.Size()));
        for (const auto& info : devices) {
            diagnostic("Pro HID gate: access=" + std::to_string(static_cast<int>(
                winrt::Windows::Devices::Enumeration::DeviceAccessInformation::CreateFromId(info.Id()).CurrentStatus())));
            auto hid = co_await HidDevice::FromIdAsync(info.Id(),
                winrt::Windows::Storage::FileAccessMode::ReadWrite);
            diagnostic("Pro HID gate: open=" + std::to_string(bool(hid)));
            if (!hid) {
                hid = co_await HidDevice::FromIdAsync(info.Id(),
                    winrt::Windows::Storage::FileAccessMode::Read);
                diagnostic("Pro HID gate: read-only=" + std::to_string(bool(hid)));
                if (!hid) continue;
            }
            // Every report whose contents change (a 0x30 report's timer byte aside), up to 80,
            // for 20 s: press buttons and move the sticks meanwhile.
            struct Dump {
                std::atomic<unsigned> count{};
                std::atomic<unsigned> logged{};
                std::mutex mutex;
                std::vector<u8> last;
            };
            auto dump = std::make_shared<Dump>();
            const auto token = hid.InputReportReceived([diagnostic, dump](auto&&, const auto& e) {
                dump->count.fetch_add(1);
                try {
                    const auto report = e.Report();
                    const auto data = report.Data();
                    std::vector<u8> bytes(data.data(), data.data() + data.Length());
                    if (bytes.size() > 1 && bytes[0] == 0x30) bytes[1] = 0;
                    std::scoped_lock lock{dump->mutex};
                    if (bytes == dump->last || dump->logged.load() >= 80) return;
                    dump->last = bytes;
                    dump->logged.fetch_add(1);
                    std::string hex;
                    for (size_t i = 0; i < std::min<size_t>(bytes.size(), 16); ++i) {
                        static constexpr char digits[] = "0123456789abcdef";
                        hex += digits[bytes[i] >> 4];
                        hex += digits[bytes[i] & 15];
                        hex += ' ';
                    }
                    diagnostic("Pro HID gate: report=" + std::to_string(report.Id()) + " bytes=" +
                               std::to_string(bytes.size()) + " | " + hex);
                } catch (const winrt::hresult_error&) {}
            });
            co_await winrt::resume_after(std::chrono::seconds{20});
            hid.InputReportReceived(token);
            hid.Close();
            diagnostic("Pro HID gate: received=" + std::to_string(dump->count.load()));
            for (const auto& device : ProControllers()) {
                const auto reading = ReadController(device);
                diagnostic("Pro HID decoder: ready=" + std::to_string(bool(device)) +
                    " buttons=" + std::to_string(static_cast<unsigned>(reading.gamepad.Buttons)) +
                    " left=" + std::to_string(reading.gamepad.LeftThumbstickX) + "," + std::to_string(reading.gamepad.LeftThumbstickY) +
                    " right=" + std::to_string(reading.gamepad.RightThumbstickX) + "," + std::to_string(reading.gamepad.RightThumbstickY));
            }
        }
    } catch (const winrt::hresult_error& e) {
        diagnostic("Pro HID gate: HRESULT=" + std::to_string(static_cast<unsigned>(e.code())));
    }
    // Windows' own paths (Bluetooth pads never reach the HID reader): what each controller
    // reports while the user presses buttons, logged only when a reading changes.
    try {
        using namespace winrt::Windows::Gaming::Input;
        for (const auto& raw : RawGameController::RawGameControllers()) {
            diagnostic("Pad probe: raw " + winrt::to_string(raw.DisplayName()) + " vid=" +
                       std::to_string(raw.HardwareVendorId()) + " pid=" +
                       std::to_string(raw.HardwareProductId()) + " wireless=" +
                       std::to_string(raw.IsWireless()) + " buttons=" +
                       std::to_string(raw.ButtonCount()) + " axes=" +
                       std::to_string(raw.AxisCount()) + " switches=" +
                       std::to_string(raw.SwitchCount()) + " gamepad=" +
                       std::to_string(bool(Gamepad::FromGameController(raw))));
        }
        std::vector<std::string> last;
        std::string last_raw;
        for (int sample = 0; sample < 600; ++sample) { // 60 s
            // The raw reading of every Nintendo controller: buttons pressed, axes, hat.
            std::string raw_line;
            for (const auto& raw : RawGameController::RawGameControllers()) {
                if (raw.HardwareVendorId() != 0x057e) continue;
                // std::vector<bool> is packed: array_view needs real bools.
                const size_t button_count = raw.ButtonCount();
                std::unique_ptr<bool[]> buttons{new bool[button_count]{}};
                std::vector<GameControllerSwitchPosition> switches(raw.SwitchCount());
                std::vector<double> axes(raw.AxisCount());
                raw.GetCurrentReading(
                    winrt::array_view<bool>(buttons.get(), buttons.get() + button_count),
                    winrt::array_view<GameControllerSwitchPosition>{switches},
                    winrt::array_view<double>{axes});
                raw_line += "raw buttons=";
                for (size_t b = 0; b < button_count; ++b)
                    if (buttons[b]) raw_line += std::to_string(b) + " ";
                raw_line += "axes=";
                for (const double axis : axes) raw_line += std::to_string(static_cast<int>(axis * 100)) + " ";
                raw_line += "hat=";
                for (const auto position : switches) raw_line += std::to_string(static_cast<int>(position)) + " ";
            }
            if (raw_line != last_raw) {
                diagnostic("Pad probe: " + raw_line);
                last_raw = raw_line;
            }
            const auto devices = EnumerateControllers();
            last.resize(devices.size());
            for (size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                const auto r = ReadController(device).gamepad;
                std::string line = winrt::to_string(device.name) + (device.pad ? " gamepad" : " hid") +
                    " ready=" + std::to_string(bool(device)) +
                    " buttons=" + std::to_string(static_cast<unsigned>(r.Buttons)) +
                    " left=" + std::to_string(r.LeftThumbstickX) + "," + std::to_string(r.LeftThumbstickY);
                if (line != last[i]) {
                    diagnostic("Pad probe: " + line);
                    last[i] = std::move(line);
                }
            }
            co_await winrt::resume_after(std::chrono::milliseconds{100});
        }
        diagnostic("Pad probe: done");
    } catch (const winrt::hresult_error& e) {
        diagnostic("Pad probe: HRESULT=" + std::to_string(static_cast<unsigned>(e.code())));
    }
}
}
void ProbeProControllerHid(std::function<void(std::string)> diagnostic) {
    ProbeHid(std::move(diagnostic));
}

std::vector<ControllerDevice> EnumerateControllers() {
    using namespace winrt::Windows::Gaming::Input;
    std::vector<ControllerDevice> devices;
    auto pro = ProControllers();
    std::wstring seen; // What Windows reports now, usable or not.
    try {
        for (const auto& raw : RawGameController::RawGameControllers()) {
            ControllerDevice device;
            device.id = raw.NonRoamableId().c_str();
            seen += device.id + L'|';
            // Over Bluetooth, or over USB before the HID reader has one, Windows itself maps a
            // Nintendo pad as a Gamepad, and by position: its B (bottom) arrives as Xbox A.
            device.nintendo = raw.HardwareVendorId() == 0x057e;
            const bool pro_controller = device.nintendo && raw.HardwareProductId() == 0x2009;
            // The HID reader wins whenever it has the pad, over USB or Bluetooth: Windows' Gamepad
            // for it reads nothing over Bluetooth, and the HID identity is stable across
            // library and gameplay.
            if (pro_controller && !pro.empty()) continue;
            device.name = pro_controller ? L"Nintendo Switch Pro Controller" : raw.DisplayName().c_str();
            // The console can report the name pseudo-localized: then the maker names it.
            if (PseudoLocalized(device.name))
                device.name = raw.HardwareVendorId() == 0x045e ? I18n::Tr(I18n::Text::ControllerXbox) : L"";
            if (device.name.empty()) device.name = I18n::Tr(I18n::Text::ControllerGeneric);
            device.wireless = raw.IsWireless();
            device.pad = Gamepad::FromGameController(raw);
            if (!device.pad) {
                if (!EnoughToPlay(raw)) continue;
                device.raw = raw;
            }
            AttachMapping(device, raw.HardwareVendorId(), raw.HardwareProductId());
            if (!device.id.empty()) devices.push_back(std::move(device));
        }
    } catch (const winrt::hresult_error&) {
        // Older Xbox contracts may lack raw names/IDs. Retain the existing
        // Gamepad path with explicitly session-scoped fallback identities.
        devices.clear();
    }
    try {
        const auto pads = Gamepad::Gamepads();
        for (unsigned i = 0; i < pads.Size(); ++i) {
            const auto pad = pads.GetAt(i);
            if (std::any_of(devices.begin(), devices.end(), [&](const auto& d) { return d.pad == pad; }))
                continue;
            devices.push_back({L"session:" + std::to_wstring(i),
                               I18n::Format(I18n::Text::ControllerNumbered, {std::to_wstring(i + 1)}),
                               pad.IsWireless(), pad});
            seen += devices.back().id + L'|';
            // No hardware id (an injected pad may arrive like this): they share one mapping.
            AttachMapping(devices.back(), 0, 0);
        }
    } catch (const winrt::hresult_error&) {}
    for (auto& device : pro) AttachMapping(device, 0x057e, 0x2009);
    devices.insert(devices.end(), pro.begin(), pro.end());
    // Automatic picks in order: a generic pad never takes player 1 from a standard one.
    std::stable_partition(devices.begin(), devices.end(), [](const auto& device) { return !device.raw; });
    static std::mutex logged_mutex;
    static std::wstring logged;
    bool changed = false;
    {
        std::scoped_lock lock{logged_mutex};
        if (seen != logged) { logged = seen; changed = true; }
    }
    if (changed) LogRawControllers();
    return devices;
}
std::optional<size_t> SelectController(const std::vector<ControllerDevice>& devices,
                                     std::wstring_view preferred) {
    return SelectControllerDevice(devices, preferred);
}
std::wstring ControllerLabel(const std::vector<ControllerDevice>& devices,
                             std::wstring_view preferred) {
    if (preferred == KeyboardControllerId) return std::wstring{I18n::Tr(I18n::Text::Keyboard)};
    if (const auto selected = SelectController(devices, preferred)) return devices[*selected].name;
    return std::wstring{I18n::Tr(preferred.empty() ? I18n::Text::NoController
                                                   : I18n::Text::ChosenControllerMissing)};
}
bool SameControllerList(const std::vector<ControllerDevice>& a,
                        const std::vector<ControllerDevice>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
        return x.id == y.id && x.name == y.name && x.wireless == y.wireless && x.pad == y.pad && x.raw == y.raw &&
               x.pro == y.pro && x.bindings == y.bindings && x.menu_shortcut == y.menu_shortcut &&
               bool(x) == bool(y);
    });
}
}
