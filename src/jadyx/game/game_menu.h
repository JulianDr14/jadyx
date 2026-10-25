// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "jadyx/input/controller_style.h"
#include "jadyx/ui/ui_actions.h"
#include "jadyx/ui/ui_icon.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_text.h"
#include "jadyx/settings/upscaling.h"

namespace Jadyx {
// In-game menu: View + Menu held for a second (Escape on a keyboard) pauses the game and opens it.
// The guide button belongs to the system, so the shortcut has to come from game buttons: Switch -
// and +, which almost no game holds together.

/// Holds back a lone + or - for a moment, so pressing View and Menu together never reaches the
/// game as a pause press; both held for HoldMs fire Toggle. A lone press is delivered after
/// PendingMs (or at once on release, so taps still arrive).
class MenuComboFilter {
public:
    static constexpr std::uint32_t PendingMs = 100;
    static constexpr std::uint32_t HoldMs = 1000;

    /// Filters this poll's + / - and returns true when the combo fires.
    bool Filter(bool& plus, bool& minus, std::uint32_t now_ms) {
        const bool any = plus || minus;
        if (plus && minus && state != State::Combo) {
            state = State::Combo;
            if (pending_since == NotPending) pending_since = now_ms;
            fired = false;
        }
        switch (state) {
        case State::Idle:
            if (!any) return false;
            state = State::Pending;
            pending_since = now_ms;
            held_plus = plus;
            held_minus = minus;
            plus = minus = false;
            return false;
        case State::Pending:
            if (!any) {
                // Released before the window closed: a tap, delivered now and released next poll.
                state = State::Idle;
                pending_since = NotPending;
                plus = held_plus;
                minus = held_minus;
                return false;
            }
            held_plus = plus;
            held_minus = minus;
            if (now_ms - pending_since < PendingMs) {
                plus = minus = false;
                return false;
            }
            state = State::Passing;
            pending_since = NotPending;
            return false;
        case State::Passing:
            if (!any) state = State::Idle;
            return false;
        case State::Combo: {
            const bool fire = !fired && plus && minus && now_ms - pending_since >= HoldMs;
            if (fire) fired = true;
            if (!any) {
                state = State::Idle;
                pending_since = NotPending;
            }
            plus = minus = false;
            return fire;
        }
        }
        return false;
    }

private:
    static constexpr std::uint32_t NotPending = ~0U;
    enum class State : std::uint8_t { Idle, Pending, Passing, Combo };
    State state{State::Idle};
    std::uint32_t pending_since{NotPending};
    bool held_plus{}, held_minus{}, fired{};
};

struct GameMenuSettings {
    ConsoleControllerStyle style{ConsoleControllerStyle::Auto};
    bool swap_face_buttons{};
    float deadzone{0.12f};
    /// PC only: the window can switch to full screen from the menu (the Series always is).
    bool can_fullscreen{};
    bool fullscreen{};
    bool show_performance_stats{};
    /// Pulse sessions are on: leaving for home keeps the game where it is, unless the player
    /// turns that off on the row (pulse_sessions.h).
    bool pulse_sessions{};
    /// Shown live: the paused frame is presented again with each change.
    Upscaling upscaling{Upscaling::Bilinear};
    /// Opt-in diagnostic package: capture from a controller without a keyboard.
    bool material_capture{};
    /// Opt-in diagnostic package ("hunt=1"): bottleneck capture of the next frames.
    bool performance_capture{};
    /// The paused frame was read back: the menu can save it as a screenshot.
    bool screenshot{};
    /// Opt-in diagnostic package ("diag_menu=1"): the display page can switch the suspects of
    /// missing geometry without another package.
    bool diag_toggles{};
    /// How draws and dispatches order their UAV writes (D3D12::DiagSync, seven modes); applies
    /// at once.
    std::uint8_t gpu_sync{};
    /// Occlusion results reach the game as never zero (VideoCommon::force_occlusion_visible):
    /// whether objects flicker because the game culls them on its queries; applies at once.
    bool occlusion_visible{};
};

/// Library: back to home, keeping a Pulse session when they are on. LibraryWithoutSession: the
/// player chose to leave without one.
enum class GameMenuResult : std::uint8_t {
    None, Resume, Library, SettingsChanged, FullScreen, Cheats, PerformanceStatsChanged, UpscalingChanged,
    LibraryWithoutSession,
    CaptureGraphics,
    CapturePerformance,
    Screenshot,
    GpuSyncChanged,
    OcclusionChanged,
};

/// One row as drawn: a label, a line under it, and the value of a setting (changed with
/// left/right) or of a page.
struct GameMenuRow {
    /// Info: it only tells something (no button does anything on it).
    enum class Kind : std::uint8_t { Action, Choice, Page, Info };
    std::wstring label;
    std::wstring value;
    Kind kind{Kind::Action};
    std::wstring detail;            ///< Under the label: what the row holds or did.
    std::optional<UiIcon> icon{};   ///< The quick menu's rows have one; a page's settings none.
};

/// The menu's pages, items and cursor, with texts in the interface language. The quick menu
/// lists actions and opens the controller's and the display's settings as pages of their own.
class GameMenu {
public:
    enum Item : std::size_t {
        Continue, Cheats, Mods, Style, FaceButtons, Deadzone, FullScreen, ImageUpscaling, PerformanceStats, Library,
        CaptureGraphics, CapturePerformance, Screenshot, ControllerPage, DisplayPage,
        GpuSync, OcclusionVisible,
    };
    enum class Page : std::uint8_t { Quick, Controller, Display };

    /// `cheats`: the game can have cheats (it has a title id), so the menu offers them.
    explicit GameMenu(GameMenuSettings settings_, bool cheats = false) : settings{settings_} {
        quick = {Continue};
        if (settings.screenshot) quick.push_back(Screenshot);
        if (cheats) quick.push_back(Cheats);
        quick.insert(quick.end(), {ControllerPage, DisplayPage});
        if (settings.material_capture) quick.push_back(CaptureGraphics);
        if (settings.performance_capture) quick.push_back(CapturePerformance);
        quick.push_back(Library);
        controller = {Style, FaceButtons, Deadzone};
        display = {ImageUpscaling, PerformanceStats};
        if (settings.can_fullscreen) display.push_back(FullScreen);
        if (settings.diag_toggles) display.insert(display.end(), {GpuSync, OcclusionVisible});
    }

    /// How many cheats are on, shown on the Cheats row.
    void SetCheatsOn(std::size_t count) { cheats_on = count; }

    /// How many mods are on: with any, a row says so. It only informs; mods change in the library,
    /// and a game reads them when it boots.
    void SetModsOn(std::size_t count) {
        mods_on = count;
        const auto found = std::find(quick.begin(), quick.end(), Mods);
        if (count == 0 || found != quick.end()) return;
        const auto cheats = std::find(quick.begin(), quick.end(), Cheats);
        const auto after = cheats != quick.end() ? cheats : std::find(quick.begin(), quick.end(), ControllerPage) - 1;
        quick.insert(after + 1, Mods);
    }

    /// What the last screenshot did, under its row (empty: where they go).
    void SetScreenshotNote(std::wstring note) { screenshot_note = std::move(note); }

    GameMenuResult Apply(MenuAction action) {
        const auto& items = Items();
        switch (action) {
        case MenuAction::Toggle:
            return GameMenuResult::Resume;
        case MenuAction::Back:
            if (page == Page::Quick) return GameMenuResult::Resume;
            // Back on the page's own row.
            selected = static_cast<std::size_t>(
                std::find(quick.begin(), quick.end(), page == Page::Controller ? ControllerPage : DisplayPage) -
                quick.begin());
            page = Page::Quick;
            return GameMenuResult::None;
        case MenuAction::Up:
            selected = (selected + items.size() - 1) % items.size();
            return GameMenuResult::None;
        case MenuAction::Down:
            selected = (selected + 1) % items.size();
            return GameMenuResult::None;
        case MenuAction::Left:
        case MenuAction::Right:
        case MenuAction::Confirm:
            return Change(action);
        case MenuAction::Alternate:
        case MenuAction::Refresh:
        case MenuAction::PreviousTab:
        case MenuAction::NextTab:
            break;
        }
        return GameMenuResult::None;
    }

    /// The rows of the page shown.
    [[nodiscard]] std::vector<GameMenuRow> Rows() const {
        std::vector<GameMenuRow> rows;
        for (const Item item : Items()) rows.push_back(Row(item));
        return rows;
    }

    [[nodiscard]] Page CurrentPage() const { return page; }
    /// The page's title and icon, as its header shows them.
    [[nodiscard]] std::wstring PageTitle() const {
        using I18n::Text;
        return Tr(page == Page::Controller ? Text::MenuController : page == Page::Display ? Text::MenuDisplay
                                                                                          : Text::MenuQuick);
    }
    /// Row of the cursor, as drawn.
    [[nodiscard]] std::size_t Selected() const { return selected; }
    [[nodiscard]] Item SelectedItem() const { return Items()[selected]; }
    [[nodiscard]] const GameMenuSettings& Settings() const { return settings; }

private:
    static std::wstring Tr(I18n::Text id) { return std::wstring{I18n::Tr(id)}; }

    [[nodiscard]] const std::vector<Item>& Items() const {
        return page == Page::Controller ? controller : page == Page::Display ? display : quick;
    }

    GameMenuRow Row(Item item) const {
        using I18n::Text;
        constexpr auto Choice = GameMenuRow::Kind::Choice;
        constexpr auto Page = GameMenuRow::Kind::Page;
        constexpr auto Action = GameMenuRow::Kind::Action;
        switch (item) {
        case Continue:
            return {Tr(Text::MenuResume), {}, Action, {}, UiIcon::Play};
        case Screenshot:
            return {Tr(Text::MenuScreenshot), {}, Action,
                    screenshot_note.empty() ? Tr(Text::MenuScreenshotWhere) : screenshot_note, UiIcon::Camera};
        case CaptureGraphics:
            return {Tr(Text::MenuCaptureGraphics), {}, Action, {}, UiIcon::Archive};
        case CapturePerformance:
            return {Tr(Text::MenuCapturePerformance), {}, Action, {}, UiIcon::Archive};
        case Cheats:
            return {Tr(Text::CheatsTitle), {}, Page, I18n::Plural(Text::CheatsOnOne, Text::CheatsOnOther, cheats_on),
                    UiIcon::Cheats};
        case Mods:
            return {Tr(Text::ModsTitle), {}, GameMenuRow::Kind::Info,
                    I18n::Plural(Text::ModsActiveOne, Text::ModsActiveOther, mods_on) + L" · " +
                        Tr(Text::MenuModsAtBoot),
                    UiIcon::Mods};
        case ControllerPage:
            return {Tr(Text::MenuController), {}, Page,
                    Tr(ControllerStyleText(settings.style)) + L" · " +
                        Tr(settings.swap_face_buttons ? Text::ByPosition : Text::ByLetter) + L" · " + DeadzoneText(),
                    UiIcon::Controller};
        case DisplayPage: {
            std::wstring detail = Tr(UpscalingText(settings.upscaling));
            if (settings.show_performance_stats) detail += L" · " + Tr(Text::MenuStatsShort);
            return {Tr(Text::MenuDisplay), {}, Page, detail, UiIcon::Display};
        }
        case Style:
            return {Tr(Text::ControllerSeenAs), Tr(ControllerStyleText(settings.style)), Choice};
        case FaceButtons:
            return {Tr(Text::FaceButtons), Tr(settings.swap_face_buttons ? Text::ByPosition : Text::ByLetter),
                    Choice};
        case Deadzone:
            return {Tr(Text::DeadZone), DeadzoneText(), Choice};
        case ImageUpscaling:
            return {Tr(Text::Upscaling), Tr(UpscalingText(settings.upscaling)), Choice};
        case PerformanceStats:
            return {Tr(Text::PerformanceStats), Tr(settings.show_performance_stats ? Text::Yes : Text::No), Choice};
        case FullScreen:
            return {Tr(Text::MenuFullScreen), Tr(settings.fullscreen ? Text::Yes : Text::No), Choice};
        case GpuSync: {
            constexpr Text values[] = {Text::MenuGpuSyncNormal, Text::MenuGpuSyncLastBinding,
                                       Text::MenuGpuSyncWorkBarriers,
                                       Text::MenuGpuSyncDispatchBarriers,
                                       Text::MenuGpuSyncDrawBarriers,
                                       Text::MenuGpuSyncGlobalBarrier,
                                       Text::MenuGpuSyncReadsFirst};
            return {Tr(Text::MenuGpuSync), Tr(values[settings.gpu_sync % 7]), Choice};
        }
        case OcclusionVisible:
            return {Tr(Text::MenuOcclusionVisible), Tr(settings.occlusion_visible ? Text::Yes : Text::No),
                    Choice};
        case Library:
            if (!settings.pulse_sessions) return {Tr(Text::MenuBackToHome), {}, Action, {}, UiIcon::Exit};
            return {Tr(Text::MenuBackToHome), {}, Choice,
                    Tr(keep_session ? Text::MenuKeepSession : Text::MenuSkipSession), UiIcon::Exit};
        }
        return {};
    }

    std::wstring DeadzoneText() const {
        return std::to_wstring(static_cast<int>(settings.deadzone * 100 + 0.5f)) + L"%";
    }

    GameMenuResult Change(MenuAction action) {
        const bool back = action == MenuAction::Left;
        const bool confirm = action == MenuAction::Confirm;
        switch (SelectedItem()) {
        case Continue:
            return confirm ? GameMenuResult::Resume : GameMenuResult::None;
        case Screenshot:
            return confirm ? GameMenuResult::Screenshot : GameMenuResult::None;
        case CaptureGraphics:
            return confirm ? GameMenuResult::CaptureGraphics : GameMenuResult::None;
        case CapturePerformance:
            return confirm ? GameMenuResult::CapturePerformance : GameMenuResult::None;
        case Cheats:
            return confirm ? GameMenuResult::Cheats : GameMenuResult::None;
        case Mods:
            return GameMenuResult::None;
        case ControllerPage:
        case DisplayPage:
            // A and Right open the page, as a row with ">" does elsewhere.
            if (back) return GameMenuResult::None;
            page = SelectedItem() == ControllerPage ? Page::Controller : Page::Display;
            selected = 0;
            return GameMenuResult::None;
        case Library:
            if (!confirm) {
                if (settings.pulse_sessions) keep_session = !keep_session;
                return GameMenuResult::None;
            }
            return !settings.pulse_sessions || keep_session ? GameMenuResult::Library
                                                            : GameMenuResult::LibraryWithoutSession;
        case Style:
            settings.style = StepConsoleControllerStyle(settings.style, back);
            return GameMenuResult::SettingsChanged;
        case FaceButtons:
            settings.swap_face_buttons = !settings.swap_face_buttons;
            return GameMenuResult::SettingsChanged;
        case Deadzone: {
            // Same three steps as the library panel.
            constexpr float steps[] = {0.08f, 0.12f, 0.18f};
            std::size_t i = settings.deadzone < 0.10f ? 0 : settings.deadzone < 0.15f ? 1 : 2;
            i = back ? (i + 2) % 3 : (i + 1) % 3;
            settings.deadzone = steps[i];
            return GameMenuResult::SettingsChanged;
        }
        case ImageUpscaling:
            settings.upscaling = StepUpscaling(settings.upscaling, back);
            return GameMenuResult::UpscalingChanged;
        case PerformanceStats:
            settings.show_performance_stats = !settings.show_performance_stats;
            return GameMenuResult::PerformanceStatsChanged;
        case FullScreen:
            settings.fullscreen = !settings.fullscreen;
            return GameMenuResult::FullScreen;
        case GpuSync:
            settings.gpu_sync = static_cast<std::uint8_t>((settings.gpu_sync + (back ? 6 : 1)) % 7);
            return GameMenuResult::GpuSyncChanged;
        case OcclusionVisible:
            settings.occlusion_visible = !settings.occlusion_visible;
            return GameMenuResult::OcclusionChanged;
        }
        return GameMenuResult::None;
    }

    GameMenuSettings settings;
    std::vector<Item> quick, controller, display;
    Page page{Page::Quick};
    std::size_t selected{};
    std::size_t cheats_on{};
    std::size_t mods_on{};
    std::wstring screenshot_note;
    bool keep_session{true};
};
} // namespace Jadyx
