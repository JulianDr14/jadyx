// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"

namespace Jadyx {
using namespace winrt::Windows::UI::Core;
using I18n::Tr;

void LibrarySession::SaveKeyboard() {
    const bool saved = SaveKeyboardBindings(panel.keyboard.bindings);
    panel.keyboard.notice = Tr(saved ? T::KeyboardSaved : T::KeyboardSaveFailed);
    keyboard_notice_until = saved ? std::chrono::steady_clock::now() + std::chrono::seconds{3} :
                                   std::chrono::steady_clock::time_point::max();
    dirty = true;
}

void LibrarySession::BeginCapture() {
    panel.keyboard.notice.clear();
    panel.keyboard.capturing = true;
    capture_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    dirty = true;
}

void LibrarySession::OnKeyDown(KeyEventArgs const& e) {
    if (e.KeyStatus().WasKeyDown && e.VirtualKey() != VirtualKey::Up &&
        e.VirtualKey() != VirtualKey::Down && e.VirtualKey() != VirtualKey::Left &&
        e.VirtualKey() != VirtualKey::Right) return;
    if (system_page.name_editor) {
        using Command = TextEntry::Command;
        std::optional<Command> command;
        switch (e.VirtualKey()) {
        case VirtualKey::Escape: command = Command::Cancel; break;
        case VirtualKey::Enter: command = Command::Done; break;
        case VirtualKey::Back: command = Command::Delete; break;
        case VirtualKey::Up: command = Command::Up; break;
        case VirtualKey::Down: command = Command::Down; break;
        case VirtualKey::Left: command = Command::Left; break;
        case VirtualKey::Right: command = Command::Right; break;
        default: break; // Characters arrive through CharacterReceived.
        }
        if (command) {
            const auto result = system_page.name_editor->Apply(*command);
            if (result != TextEntry::Result::None) finish_name_editor = result == TextEntry::Result::Done;
        }
        dirty = true;
        e.Handled(true);
        return;
    }
    auto& editor = panel.keyboard;
    if (editor.open) {
        const auto key = e.VirtualKey();
        if (editor.capturing) {
            if (key != VirtualKey::Escape) {
                if (AssignKeyboardKey(editor.bindings, editor.selected, static_cast<unsigned>(key))) SaveKeyboard();
                else {
                    editor.notice = Tr(T::KeyUnsupported);
                    keyboard_notice_until = std::chrono::steady_clock::time_point::max();
                }
            }
            editor.capturing = false;
            dirty = true;
        } else if (key == VirtualKey::Escape) { editor.open = false; dirty = true; }
        else if (key == VirtualKey::Tab) {
            editor.page = (editor.page + 1) % 3; editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
        } else if (key == VirtualKey::Enter) BeginCapture();
        else if (key == VirtualKey::Delete) {
            AssignKeyboardKey(editor.bindings, editor.selected, 0); SaveKeyboard();
        } else if (key == VirtualKey::Up || key == VirtualKey::Down ||
                   key == VirtualKey::Left || key == VirtualKey::Right) {
            const auto items = KeyboardPageActions(editor.page);
            const size_t count = items.size();
            const size_t index = static_cast<size_t>(std::find(items.begin(), items.end(), editor.selected) - items.begin());
            const size_t step = key == VirtualKey::Up || key == VirtualKey::Down ? 2 : 1;
            const bool reverse = key == VirtualKey::Up || key == VirtualKey::Left;
            editor.selected = items[(index + (reverse ? count - step : step)) % count];
            dirty = true;
        }
        e.Handled(true);
        return;
    }
    switch (e.VirtualKey()) {
    case VirtualKey::Up: actions |= Up; break;
    case VirtualKey::Down: actions |= Down; break;
    case VirtualKey::Left: actions |= Left; break;
    case VirtualKey::Right: actions |= Right; break;
    case VirtualKey::F1: actions |= Panel; break;
    case VirtualKey::Escape: actions |= Quit; break;
    case VirtualKey::Enter: actions |= Play; break;
    case VirtualKey::X: actions |= Swap; break;
    case VirtualKey::R: actions |= Refresh; break;
    case VirtualKey::F: actions |= Pin; break;
    case VirtualKey::O: actions |= AddFolder; break;
    case VirtualKey::F2: actions |= Options; break;
    case VirtualKey::PageUp: actions |= PreviousManagerTab; break;
    case VirtualKey::PageDown: actions |= NextManagerTab; break;
    case VirtualKey::Q: actions |= PreviousTab; break;
    case VirtualKey::E: actions |= NextTab; break;
    default: break;
    }
    e.Handled(true);
}

void LibrarySession::OnPointerPressed(PointerEventArgs const& e) {
    const auto p = e.CurrentPoint().Position();
    const auto dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel() * oversize;
    const auto scale = std::max(0.01f, std::min(width / 1280.0f, height / 720.0f));
    const float x = static_cast<float>((p.X * dpi - (width - 1280 * scale) / 2) / scale);
    const float y = static_cast<float>((p.Y * dpi - (height - 720 * scale) / 2) / scale);
    const bool handled = PressDialog(x, y) || PressGameOptions(x, y) || PressUpdatePage(x, y) || PressHeader(x, y) ||
                         PressConfiguration(x, y) || PressKeyboardEditor(x, y);
    if (!handled) {
        if (settings) PressControllerPanel(x, y);
        else if (!loading) PressLibrary(x, y);
    }
    e.Handled(true);
}

bool LibrarySession::PressDialog(float x, float y) {
    if (configuration.network_screen || configuration.confirm_import) {
        const auto hit = canvas->HitAt(x, y);
        if (hit.hit == LibraryHit::NetworkAction && hit.index < 3) {
            configuration.network_action = hit.index;
            actions |= Play;
            dirty = true;
        }
        return true;
    }
    if (system_page.name_editor) {
        using L = SystemPageLayout;
        if (const auto key = TextEntryPanelLayout::KeyAt(x - L::EditorX, y - L::EditorY)) {
            auto& editor = *system_page.name_editor;
            editor.Focus(key->first, key->second);
            if (editor.Press() == TextEntry::Result::Done) finish_name_editor = true;
            system_page.editor_notice.clear();
            dirty = true;
        }
        return true;
    }
    return false;
}

bool LibrarySession::PressGameOptions(float x, float y) {
    if (game_options.open && y < 94) {
        const auto target = canvas->HitAt(x, y);
        if (target.hit == LibraryHit::Tab) {
            pressed_tab = static_cast<LibraryTab>(target.index);
            dirty = true;
        } else if (target.hit == LibraryHit::Controller && !game_options.page && !content_session) {
            game_options = {};
            actions |= Panel;
            dirty = true;
        }
        return true;
    }
    if (game_options.open && canvas->HitAt(x, y).hit == LibraryHit::GameOptionSelect) {
        const auto target = canvas->HitAt(x, y);
        if (target.hit == LibraryHit::GameOptionSelect && target.index < GameOptions().size()) {
            requested_game_option = target.index;
            dirty = true;
        }
        return true;
    }
    if (game_options.open) {
        const auto target = canvas->HitAt(x, y);
        if (target.hit == LibraryHit::ManagerTab) {
            requested_manager_tab = target.index;
            game_options.focus = GameOptionsFocus::ManagerTabs;
            dirty = true;
        } else if (target.hit == LibraryHit::ManagerRow) {
            if (game_options.page == GameOption::Mods && mod_session) {
                const auto count = mods_page.tab == Mods::ModsTab::Installed ?
                    mod_session->View().mods.size() : mod_session->View().files.size();
                if (target.index < count) { mods_page.selected = target.index; actions |= Play; }
            } else if (game_options.page == GameOption::Cheats && cheat_session && target.index < cheat_session->Catalog().Cheats(cheats_page.category).size()) {
                cheats_page.selected = target.index; actions |= Play;
            }
            game_options.focus = GameOptionsFocus::Content;
            dirty = true;
        } else if (target.hit == LibraryHit::ManagerCard && mod_session &&
                   target.index < mod_session->Store().cards.size()) {
            mods_page.card = target.index;
            game_options.focus = GameOptionsFocus::Content;
            actions |= Play; dirty = true;
        }
        return true;
    }
    return false;
}

bool LibrarySession::PressUpdatePage(float x, float y) {
    if (system_page.updates_open) {
        const auto hit = canvas->HitAt(x, y);
        if (hit.hit == LibraryHit::UpdateAction) {
            if (hit.index == UpdateBackHit) actions |= Quit;
            else { system_page.update_row = hit.index; actions |= Play; }
            dirty = true;
        }
        return true;
    }
    // The new-version notice: its prompts do what Y and X do while it shows.
    if (const auto hit = canvas->HitAt(x, y); hit.hit == LibraryHit::UpdateNotice) {
        actions |= hit.index == 0 ? Pin : Refresh;
        dirty = true;
        return true;
    }
    return false;
}

    // The header's tabs and controller work over every page that is not a dialog.
bool LibrarySession::PressHeader(float x, float y) {
    if ((!settings || configuration.open) && !panel.keyboard.open && !panel.expanded && !game_options.open &&
        !missing_content && !configuration.busy) {
        const auto target = canvas->HitAt(x, y);
        if (target.hit == LibraryHit::Tab) {
            pressed_tab = static_cast<LibraryTab>(target.index);
            return true;
        }
        if (target.hit == LibraryHit::Controller && !configuration.open) {
            actions |= Panel;
            return true;
        }
    }
    return false;
}

bool LibrarySession::PressConfiguration(float x, float y) {
    if (configuration.open && !content_session && !panel.keyboard.open && !panel.expanded) {
        const auto target = canvas->HitAt(x, y);
        const bool in_page = system_page.open || graphics_page.open || configuration.files || settings;
        const size_t shown = system_page.open ? HomeSystem : graphics_page.open ? HomeGraphics :
                             configuration.files ? HomeFiles : settings ? HomeControllers : configuration.selected;
        auto category = target.hit == LibraryHit::Category ? std::optional<size_t>{target.index} :
                        target.hit == LibraryHit::Controller ? std::optional<size_t>{HomeControllers} :
                        std::nullopt;
        // A row of the category the list has focused opens it, like A would.
        if (target.hit == LibraryHit::SettingRow && !in_page) category = shown;
        if (configuration.busy || picker != nullptr) {
        } else if (category && !(in_page && *category == shown)) {
            // Another category: leave the open page and enter it.
            system_page.open = graphics_page.open = configuration.files = settings = return_to_configuration = false;
            panel.notice.clear();
            configuration.selected = *category;
            actions |= Play;
            dirty = true;
        } else if (target.hit == LibraryHit::SettingRow) {
            if (system_page.open) system_page.selected = target.index;
            else if (graphics_page.open) graphics_page.selected = target.index;
            else if (configuration.files) configuration.selected = target.index;
            else setting_row = static_cast<unsigned>(target.index);
            actions |= Play;
            dirty = true;
        }
        return true;
    }
    return false;
}

bool LibrarySession::PressKeyboardEditor(float x, float y) {
    if (panel.keyboard.open) {
        auto& editor = panel.keyboard;
        if (Contains({1120, 112, 48, 40}, x, y)) {
            editor.open = editor.capturing = false; dirty = true;
        } else if (!editor.capturing) {
            if (Contains({635, 203, 514, 36}, x, y)) {
                editor.page = std::min(2U, static_cast<unsigned>((x - 635) / 174));
                editor.selected = KeyboardPageActions(editor.page).front(); dirty = true;
            } else if (Contains({635, 603, 155, 36}, x, y)) {
                AssignKeyboardKey(editor.bindings, editor.selected, 0); SaveKeyboard();
            } else if (Contains({806, 603, 170, 36}, x, y)) {
                editor.bindings = DefaultKeyboardBindings; SaveKeyboard();
            } else {
                for (size_t i = 0; i < KeyboardButtonCount; ++i) {
                    if (Contains(KeyboardDiagramTile(i), x, y)) {
                        editor.page = KeyboardPageFor(i); editor.selected = i; BeginCapture(); break;
                    }
                }
                for (const size_t i : KeyboardPageActions(editor.page)) {
                    if (!editor.capturing && Contains(KeyboardTileFor(i), x, y)) {
                        editor.selected = i; BeginCapture(); break;
                    }
                }
            }
        }
        return true;
    }
    return false;
}

void LibrarySession::PressControllerPanel(float x, float y) {
    if (panel.expanded) {
        const size_t first = ControllerChoiceFirst(panel);
        if (x >= 648 && x < 1178 && y >= ControllerChoiceTop) {
            const size_t i = first + static_cast<size_t>((y - ControllerChoiceTop) / ControllerChoiceHeight);
            if (i < panel.devices.size() + 2 && i < first + ControllerVisibleChoices) {
                panel.choice = i;
                actions |= Play;
            } else actions |= Quit;
        } else actions |= Quit;
    } else if (!panel.xbox && Contains({648, 510, 530, 34}, x, y)) {
        panel.keyboard.open = true; panel.keyboard.capturing = false; dirty = true;
    } else if (x < 620 || x > 1206 || y < 133 || y > 620) actions |= Panel;
    else if (x >= 648 && x < 1178 && y >= ControllerRowTop &&
             y < ControllerRowTop + (panel.xbox ? 3 : 4) * ControllerRowHeight) {
        setting_row = static_cast<unsigned>((y - ControllerRowTop) / ControllerRowHeight);
        actions |= Play;
    }
}

void LibrarySession::PressLibrary(float x, float y) {
    const auto target = canvas->HitAt(x, y);
    switch (target.hit) {
    case LibraryHit::Play: actions |= Play; break;
    case LibraryHit::Options: actions |= Options; break;
    case LibraryHit::Filter:
        filter = static_cast<Filter>(target.index);
        filter_focus = true;
        shelf = LibraryLayout::Shelf(scan.entries, filter);
        shelf_focus = 0;
        dirty = true;
        break;
    case LibraryHit::RailTile:
    case LibraryHit::Tile:
        if (target.index < scan.entries.size()) {
            selected = target.index;
            rail = target.hit == LibraryHit::RailTile;
            dirty = true;
        }
        break;
    case LibraryHit::ShelfTile:
        if (target.index < shelf.size()) {
            // A second press on the focused game plays it.
            if (!filter_focus && shelf_focus == target.index) actions |= Play;
            shelf_focus = target.index;
            selected = shelf[target.index];
            filter_focus = false;
            dirty = true;
        }
        break;
    case LibraryHit::Import:
        shelf_focus = shelf.size();
        filter_focus = false;
        actions |= Play;
        dirty = true;
        break;
    default: break;
    }
}

void LibrarySession::OnWheel(PointerEventArgs const& e) {
    if (configuration.network_screen) {
        const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
        if (delta) { actions |= delta > 0 ? Left : Right; dirty = true; }
        e.Handled(true);
        return;
    }
    if (game_options.open && !configuration.open) {
        const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
        if (delta) { actions |= delta > 0 ? Up : Down; dirty = true; }
        e.Handled(true);
        return;
    }

    if (settings && panel.expanded && !panel.keyboard.open) {
        const int delta = e.CurrentPoint().Properties().MouseWheelDelta();
        const size_t count = panel.devices.size() + 2;
        if (delta > 0 && panel.choice) --panel.choice;
        if (delta < 0 && panel.choice + 1 < count) ++panel.choice;
        dirty = true;
        e.Handled(true);
    }
}

void LibrarySession::ExpireNotices(TimePoint now) {
    if (!panel.keyboard.notice.empty() && now >= keyboard_notice_until) {
        panel.keyboard.notice.clear(); dirty = true;
    }
    if (!panel.notice.empty() && now >= controller_notice_until) {
        panel.notice.clear(); dirty = true;
    }
    if (!system_page.notice.empty() && now >= system_notice_until) {
        system_page.notice.clear(); dirty = true;
    }
    if (!options_notice.empty() && now >= options_notice_until) {
        options_notice.clear(); dirty = true;
    }
    SYSTEMTIME clock{};
    GetLocalTime(&clock);
    if (clock.wMinute != shown_minute) { shown_minute = clock.wMinute; dirty = true; }
    if (panel.keyboard.capturing && now >= capture_until) {
        panel.keyboard.capturing = false; dirty = true;
    }
}

void LibrarySession::UpdateControllers(TimePoint now) {
    const bool catalog_changed = controllers.Update(now, [] {
        RefreshProControllers();
        return EnumerateControllers();
    }, SameControllerList);
    if (catalog_changed) {
        panel.devices = controllers.Devices();
        panel.choice = std::min(panel.choice, panel.devices.size() + 1);
        // An open list must not apply a stale row after hotplug/reorder.
        if (panel.expanded) {
            panel.expanded = false;
            actions &= ~Play;
        }
        dirty = true;
    }
    // Selection follows settings immediately; discovery remains rate limited.
    const auto next = controllers.Select(options.controller_id);
    if (bool(pad) != bool(next) || (pad && next &&
        (pad->id != next->id || pad->pad != next->pad || pad->pro != next->pro))) {
        previous = {}; stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset();
        battery_read_at = {}; // Another controller: read its battery now.
    }
    if (catalog_changed || !pad || !next || pad->id != next->id)
        pad = next ? std::optional<ControllerDevice>{*next} : std::nullopt;
    const unsigned context = configuration.confirm_import ? 1001 : configuration.network_screen ? 1000 : game_options.open ? (7 + static_cast<unsigned>(game_options.selected) * 3 + static_cast<unsigned>(game_options.focus)) :
        graphics_page.open ? 1002 : system_page.name_editor ? 6 : system_page.open ? 5 :
        panel.keyboard.open ? 3 : panel.expanded ? 2 : settings ? 1 : configuration.open ? 4 : 0;
    if (context != navigation_context) {
        stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset(); navigation_context = context;
    }
}

void LibrarySession::ReadPad(TimePoint now) {
    if (pad) {
        try {
            const auto reading = ReadController(*pad).gamepad;
            const auto buttons = reading.Buttons;
            // Track physical edges before mapping: changing the option while holding a
            // button must not synthesize a second press of its new logical counterpart.
            const auto edges = MapFaceButtons(buttons & ~previous,
                SwapControllerFaceButtons(*pad, options.swap_face_buttons));
            previous = buttons;
            auto hit = [&](GamepadButtons b) { return (edges & b) == b; };
            // The d-pad takes precedence; do not combine opposite directions.
            constexpr auto dpad = GamepadButtons::DPadUp | GamepadButtons::DPadDown |
                                  GamepadButtons::DPadLeft | GamepadButtons::DPadRight;
            unsigned directions = 0;
            if (game_options.open) {
                const double dx = ((buttons & GamepadButtons::DPadRight) != GamepadButtons::None ? 1.0 : 0.0) -
                                  ((buttons & GamepadButtons::DPadLeft) != GamepadButtons::None ? 1.0 : 0.0);
                const double dy = ((buttons & GamepadButtons::DPadUp) != GamepadButtons::None ? 1.0 : 0.0) -
                                  ((buttons & GamepadButtons::DPadDown) != GamepadButtons::None ? 1.0 : 0.0);
                switch (manager_dpad.Update((buttons & dpad) != GamepadButtons::None ? dx : reading.LeftThumbstickX,
                                            (buttons & dpad) != GamepadButtons::None ? dy : reading.LeftThumbstickY, now)) {
                case StickDirection::Up: directions |= Up; break;
                case StickDirection::Down: directions |= Down; break;
                case StickDirection::Left: directions |= Left; break;
                case StickDirection::Right: directions |= Right; break;
                case StickDirection::None: break;
                }
                const auto triggers = trigger_tabs.Update(reading.LeftTrigger, reading.RightTrigger);
                if (triggers == 1) actions |= PreviousManagerTab;
                if (triggers == 2) actions |= NextManagerTab;
            } else {
                if ((buttons & dpad) != GamepadButtons::None) stick_navigation.Reset();
                else {
                    switch (stick_navigation.Update(reading.LeftThumbstickX, reading.LeftThumbstickY, now)) {
                    case StickDirection::Up: directions |= Up; break;
                    case StickDirection::Down: directions |= Down; break;
                    case StickDirection::Left: directions |= Left; break;
                    case StickDirection::Right: directions |= Right; break;
                    case StickDirection::None: break;
                    }
                }
                if (hit(GamepadButtons::DPadUp)) directions |= Up;
                if (hit(GamepadButtons::DPadDown)) directions |= Down;
                if (hit(GamepadButtons::DPadLeft)) directions |= Left;
                if (hit(GamepadButtons::DPadRight)) directions |= Right;
            }
            if (system_page.name_editor) {
                // Like the console's keyboard: A types, X deletes, Y adds a space, Menu (or RB)
                // is done, View (or LB) is Shift and B leaves without saving. The bumpers are
                // what a game's keyboard uses, where + and - belong to the game menu.
                auto& editor = *system_page.name_editor;
                const std::pair<bool, MenuAction> presses[] = {
                    {(directions & Up) != 0, MenuAction::Up},
                    {(directions & Down) != 0, MenuAction::Down},
                    {(directions & Left) != 0, MenuAction::Left},
                    {(directions & Right) != 0, MenuAction::Right},
                    {hit(GamepadButtons::A), MenuAction::Confirm},
                    {hit(GamepadButtons::X), MenuAction::Alternate},
                    {hit(GamepadButtons::Y), MenuAction::Refresh},
                    {hit(GamepadButtons::View) || hit(GamepadButtons::LeftShoulder), MenuAction::PreviousTab},
                    {hit(GamepadButtons::Menu) || hit(GamepadButtons::RightShoulder), MenuAction::NextTab},
                    {hit(GamepadButtons::B), MenuAction::Back},
                };
                for (const auto& [pressed, action] : presses) {
                    if (!pressed || finish_name_editor) continue;
                    const auto result = editor.Apply(*TextEntryCommand(action));
                    if (result != TextEntry::Result::None) finish_name_editor = result == TextEntry::Result::Done;
                }
                if (directions || edges != GamepadButtons::None) {
                    system_page.editor_notice.clear();
                    dirty = true;
                }
            } else {
                // Back must remain available while editing/capturing keyboard input.
                // Track all edges, but suppress other pad actions inside that modal.
                if (hit(GamepadButtons::B)) actions |= Quit;
                if (!panel.keyboard.open) {
                    actions |= directions;
                    if (hit(GamepadButtons::A)) actions |= Play;
                    // On the library X rescans and Y pins; the pages keep their own X and Y.
                    const bool library = !settings && !configuration.open && !game_options.open &&
                                         !missing_content;
                    if (hit(GamepadButtons::X)) {
                        if (system_page.updates_open) actions |= Quit;
                        else if (settings || content_session || missing_content) actions |= Swap;
                        else if (library) actions |= Refresh;
                        else if (!configuration.open) actions |= AddFolder;
                    }
                    if (hit(GamepadButtons::Y)) actions |= library ? Pin : Refresh;
                    if (hit(GamepadButtons::Menu)) actions |= Options;
                    if (hit(GamepadButtons::View)) actions |= Panel;
                    if (hit(GamepadButtons::LeftShoulder)) actions |= PreviousTab;
                    if (hit(GamepadButtons::RightShoulder)) actions |= NextTab;
                }
            }
        } catch (...) {
            controllers.ReadFailed();
            pad.reset(); previous = {}; stick_navigation.Reset(); manager_dpad.Reset(); trigger_tabs.Reset();
        }
    }
}

    // The battery changes slowly and its report is a driver call: a few times a minute.
void LibrarySession::ReadBattery(TimePoint now) {
    if (now >= battery_read_at) {
        battery_read_at = now + std::chrono::seconds{20};
        ControllerBattery read;
        try {
            if (pad && pad->pad) {
                const auto report = pad->pad.TryGetBatteryReport();
                using winrt::Windows::System::Power::BatteryStatus;
                if (report && report.Status() != BatteryStatus::NotPresent) {
                    const auto remaining = report.RemainingCapacityInMilliwattHours();
                    const auto full = report.FullChargeCapacityInMilliwattHours();
                    if (remaining && full && full.Value() > 0) {
                        read.known = true;
                        read.level = std::clamp(static_cast<float>(remaining.Value()) / full.Value(), 0.0f, 1.0f);
                        read.charging = report.Status() == BatteryStatus::Charging;
                    }
                }
            }
        } catch (...) {}
        if (!(read == battery)) { battery = read; dirty = true; }
    }
}

} // namespace Jadyx
