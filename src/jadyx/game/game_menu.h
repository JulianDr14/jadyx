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
#include "jadyx/input/motion_emu.h"
#include "jadyx/input/right_stick_settings.h"
#include "jadyx/input/rumble_settings.h"
#include "jadyx/settings/graphics_page.h"
#include "jadyx/ui/ui_actions.h"
#include "jadyx/ui/ui_icon.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_text.h"

namespace Jadyx {
// In-game menu (Jadyx Hub to the player): View + Menu held for a second (Escape on a keyboard) pauses
// the game and opens it. The guide button belongs to the system, so the shortcut has to come from game
// buttons: Switch - and +, which almost no game holds together. A controller's mapping can replace
// the combination (PadMenuShortcut); this one stays the default.

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

/// The A/B switches of the geometry glitch hunt page, each a live renderer toggle.
struct GlitchToggles {
    bool a2c_shader{true}; ///< single-sample alpha-to-coverage dithered in the shader
    bool precise_writes{true}; ///< CPU writes beside GPU bytes apply at once
    bool array_own_memory{}; ///< large compressed arrays moved to memory of their own
    bool array_rewrite{}; ///< large compressed arrays copied out and back in place
    int astc_arrays{}; ///< ASTC arrays: 0 auto (probed BC), 1 RGBA8, 2 BC (AstcArrayMode)
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
    /// Opt-in diagnostic package: capture from a controller without a keyboard.
    bool material_capture{};
    /// Opt-in diagnostic package ("hunt=1"): bottleneck capture of the next frames.
    bool performance_capture{};
    /// The paused frame was read back: the menu can save it as a screenshot.
    bool screenshot{};
    /// This game's own offline mode; nullopt follows Settings > System (`offline_default`).
    std::optional<bool> offline_mode;
    bool offline_default{};
    /// Settings > Graphics and this game's own, for the Graphics page.
    GraphicsSettings graphics_global{};
    GameGraphics graphics{};
    /// The game's memory is one Memory Edit can look through.
    bool memory_edit{};
    /// Settings > Controller's right stick and this game's own. Without `right_stick_per_game`
    /// (no title id) the rows change Settings' values instead.
    RightStickSettings right_stick_global{};
    GameRightStick right_stick{};
    bool right_stick_per_game{};
    /// This game's emulated gyro and when its controller shows on screen (only with
    /// `right_stick_per_game`: they are always a game's own).
    MotionMode motion{MotionMode::Off};
    MotionViewMode motion_view{MotionViewMode::Always};
    /// Opt-in diagnostic package ("glitch_hunt_menu=1"): the geometry glitch hunt page.
    bool glitch_hunt{};
    GlitchToggles glitch{};
    /// Opt-in diagnostic package ("geo_hunt=1"): missed-write hunt for deformed meshes. A row
    /// marks the moment a deformation shows.
    bool geo_hunt{};
    /// Opt-in diagnostic package ("pso_menu=1"): the priority a pipeline build gets while the
    /// game waits for it (D3D12::SetPipelineBuildPriority's boost), 0 off, 2 above normal,
    /// 3 highest; changed at run time to compare in one session.
    bool pso_menu{};
    std::uint32_t pso_boost{};
    /// Settings > Controller's gyro controls (every game's).
    MotionStick motion_stick{MotionStick::Right};
    MotionShakeButton motion_shake{MotionShakeButton::StickClick};
    /// Settings > Controller's rumble and this game's own, as the right stick's
    /// (`right_stick_per_game` says whether there is a game).
    RumbleSettings rumble_global{};
    GameRumble rumble{};
    /// Geometry hunt package: drop the triangles whose indices point past the vertex buffers
    /// (they would stretch to the model origin). Shown with the geo_hunt rows.
    bool geo_oob_fix{true};
};

/// Library: back to home, keeping a Pulse session when they are on. LibraryWithoutSession: the
/// player chose to leave without one.
enum class GameMenuResult : std::uint8_t {
    None, Resume, Library, SettingsChanged, FullScreen, Cheats, PerformanceStatsChanged,
    LibraryWithoutSession,
    CaptureGraphics,
    CapturePerformance,
    /// Geometry glitch hunt: capture the next frames, or Settings().glitch changed.
    CaptureGlitches,
    GlitchTogglesChanged,
    Screenshot,
    OfflineModeChanged,
    GraphicsChanged,
    Amiibo,
    MemoryEdit,
    /// The right stick's rows: Settings() holds the new values (the game's own or Settings').
    RightStickChanged,
    /// Geometry hunt: mark the moment in its log, or one of its fixes changed.
    GeoMark,
    GeoOobFixChanged,
    /// Settings().pso_boost changed.
    PsoBoostChanged,
    /// The room chat's page.
    Chat,
    /// Settings().motion or motion_view changed.
    MotionChanged,
    /// Settings().motion_stick or motion_shake changed (Settings > Controller's).
    GyroControlsChanged,
    /// The rumble rows: Settings() holds the new values (the game's own or Settings').
    RumbleChanged,
};

/// The console's tag reader as the game left it, for the Amiibo row.
enum class AmiiboReader : std::uint8_t { Off, Looking, Placed };

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
        Continue, Cheats, Mods, Style, FaceButtons, Deadzone, FullScreen, PerformanceStats, Library,
        CaptureGraphics, CapturePerformance, Screenshot, ControllerPage, DisplayPage, OfflineMode,
        GraphicsMenu, GfxUpscaling, GfxFrameGen, GfxShaders, GfxGpuFence, GfxGuestClock, GfxExactGpuReads, GfxTextureMemory,
        GfxGpuAccuracy, GfxConsoleMode, Amiibo, MemoryEdit,
        RStickInvertX, RStickInvertY, RStickSensitivity, RStickMouse, RStickMouseSensitivity,
        GlitchPage, GlitchCapture, GlitchA2cShader, GlitchPreciseWrites, GlitchArrayOwnMemory,
        GlitchArrayRewrite, GlitchAstcArraysRgba,
        GeoMark, GeoOobFix, PsoBoost, Chat, GfxFixedCoreMap, Motion, MotionView,
        GyroStick, GyroShake, Rumble, RumbleStrength,
    };
    enum class Page : std::uint8_t { Quick, Controller, Display, Graphics, Glitch };

    /// `cheats`: the game has a title id, so the menu offers its cheats, its own offline mode and
    /// its graphics that can change while it runs. The rest wait for a boot: the library has them.
    explicit GameMenu(GameMenuSettings settings_, bool cheats = false) : settings{settings_} {
        quick = {Continue};
        if (settings.screenshot) quick.push_back(Screenshot);
        if (cheats) quick.push_back(Cheats);
        if (cheats && settings.memory_edit) quick.push_back(MemoryEdit);
        quick.insert(quick.end(), {ControllerPage, DisplayPage});
        if (cheats) quick.push_back(GraphicsMenu);
        if (cheats) quick.push_back(OfflineMode);
        if (settings.material_capture) quick.push_back(CaptureGraphics);
        if (settings.performance_capture) quick.push_back(CapturePerformance);
        if (settings.glitch_hunt) quick.push_back(GlitchPage);
        if (settings.geo_hunt) quick.insert(quick.end(), {GeoMark, GeoOobFix});
        if (settings.pso_menu) quick.push_back(PsoBoost);
        quick.push_back(Library);
        glitch = {GlitchCapture, GlitchA2cShader, GlitchPreciseWrites, GlitchArrayOwnMemory,
                  GlitchArrayRewrite, GlitchAstcArraysRgba};
        controller = {Style, FaceButtons, Deadzone, RStickInvertX, RStickInvertY, RStickSensitivity,
                      RStickMouse, RStickMouseSensitivity, Rumble, RumbleStrength};
        if (settings.right_stick_per_game)
            controller.insert(controller.end(), {Motion, GyroStick, GyroShake, MotionView});
        display = {PerformanceStats};
        if (settings.can_fullscreen) display.push_back(FullScreen);
        UpdateGraphicsItems();
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

    /// Adds the Amiibo row, after the game's cheats and mods. `name`: the tag placed, if any.
    /// While the game isn't reading tags the row only says so: placing one would do nothing.
    void SetAmiibo(AmiiboReader reader, std::wstring name = {}) {
        amiibo_reader = reader;
        amiibo_name = std::move(name);
        if (std::find(quick.begin(), quick.end(), Amiibo) != quick.end()) return;
        auto after = std::find(quick.begin(), quick.end(), Mods);
        if (after == quick.end()) after = std::find(quick.begin(), quick.end(), Cheats);
        if (after == quick.end()) after = std::find(quick.begin(), quick.end(), ControllerPage) - 1;
        quick.insert(after + 1, Amiibo);
    }

    /// In an online room: the chat's row goes first after Continue, with the unread count.
    void SetChat(std::size_t unread) {
        chat_unread = unread;
        if (std::find(quick.begin(), quick.end(), Chat) != quick.end()) return;
        quick.insert(quick.begin() + 1, Chat);
    }
    /// Puts the cursor on the chat's row (the menu opened from a message's notice).
    void SelectChat() {
        page = Page::Quick;
        const auto found = std::find(quick.begin(), quick.end(), Chat);
        if (found != quick.end()) selected = static_cast<std::size_t>(found - quick.begin());
    }

    /// What the last screenshot did, under its row (empty: where they go).
    void SetScreenshotNote(std::wstring note) { screenshot_note = std::move(note); }
    /// The results of the search Memory Edit keeps, on its row.
    void SetMemoryEditHits(std::uint64_t hits) { memory_edit_hits = hits; }

    GameMenuResult Apply(MenuAction action) {
        const auto& items = Items();
        switch (action) {
        case MenuAction::Toggle:
            return GameMenuResult::Resume;
        case MenuAction::Back:
            if (page == Page::Quick) return GameMenuResult::Resume;
            // Back on the page's own row.
            selected = static_cast<std::size_t>(
                std::find(quick.begin(), quick.end(),
                          page == Page::Controller ? ControllerPage
                          : page == Page::Display  ? DisplayPage
                          : page == Page::Glitch   ? GlitchPage
                                                   : GraphicsMenu) -
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
        case MenuAction::Menu:
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
        return Tr(page == Page::Controller ? Text::MenuController
                  : page == Page::Display  ? Text::MenuDisplay
                  : page == Page::Graphics ? Text::Graphics
                  : page == Page::Glitch   ? Text::MenuGlitchPage
                                           : Text::MenuQuick);
    }
    /// Row of the cursor, as drawn.
    [[nodiscard]] std::size_t Selected() const { return selected; }
    [[nodiscard]] Item SelectedItem() const { return Items()[selected]; }
    [[nodiscard]] const GameMenuSettings& Settings() const { return settings; }

private:
    static std::wstring Tr(I18n::Text id) { return std::wstring{I18n::Tr(id)}; }

    [[nodiscard]] const std::vector<Item>& Items() const {
        return page == Page::Controller ? controller : page == Page::Display ? display : page == Page::Graphics ? graphics : page == Page::Glitch ? glitch : quick;
    }

    /// The Handheld style forces the console's mode (uwp_input.cpp): there is nothing to choose.
    void UpdateGraphicsItems() {
        graphics = {GfxUpscaling,     GfxFrameGen,      GfxShaders,     GfxGpuFence,    GfxGuestClock,
                    GfxExactGpuReads, GfxTextureMemory, GfxGpuAccuracy, GfxFixedCoreMap};
        if (settings.style != ConsoleControllerStyle::Handheld) graphics.push_back(GfxConsoleMode);
    }

    static GraphicsRow RowOf(Item item) {
        switch (item) {
        case GfxFrameGen: return GraphicsRow::FrameGen;
        case GfxShaders: return GraphicsRow::Shaders;
        case GfxGpuFence: return GraphicsRow::GpuFence;
        case GfxGuestClock: return GraphicsRow::GuestClock;
        case GfxExactGpuReads: return GraphicsRow::ExactGpuReads;
        case GfxTextureMemory: return GraphicsRow::TextureMemory;
        case GfxGpuAccuracy: return GraphicsRow::GpuAccuracy;
        case GfxConsoleMode: return GraphicsRow::ConsoleMode;
        case GfxFixedCoreMap: return GraphicsRow::FixedCoreMap;
        default: return GraphicsRow::Upscaling;
        }
    }

    GameMenuRow Row(Item item) const {
        using I18n::Text;
        constexpr auto Choice = GameMenuRow::Kind::Choice;
        constexpr auto Page = GameMenuRow::Kind::Page;
        constexpr auto Action = GameMenuRow::Kind::Action;
        switch (item) {
        case Continue:
            return {Tr(Text::MenuResume), {}, Action, {}, UiIcon::Play};
        case Chat:
            return {Tr(Text::ChatTitle), {}, Page,
                    chat_unread != 0 ? I18n::Plural(Text::ChatUnreadOne, Text::ChatUnreadOther, chat_unread)
                                     : std::wstring{},
                    UiIcon::Globe};
        case Screenshot:
            return {Tr(Text::MenuScreenshot), {}, Action,
                    screenshot_note.empty() ? Tr(Text::MenuScreenshotWhere) : screenshot_note, UiIcon::Camera};
        case CaptureGraphics:
            return {Tr(Text::MenuCaptureGraphics), {}, Action, {}, UiIcon::Archive};
        case CapturePerformance:
            return {Tr(Text::MenuCapturePerformance), {}, Action, {}, UiIcon::Archive};
        case GlitchPage:
            return {Tr(Text::MenuGlitchPage), {}, Page, Tr(Text::MenuGlitchPageDetail), UiIcon::Archive};
        case GeoMark:
            return {Tr(Text::MenuGeoMark), {}, Action,
                    geo_marks != 0 ? Tr(Text::MenuGeoMarked) + L" " + std::to_wstring(geo_marks)
                                   : Tr(Text::MenuGeoMarkDetail),
                    UiIcon::Archive};
        case GeoOobFix:
            return {Tr(Text::MenuGeoOobFix), Tr(settings.geo_oob_fix ? Text::Yes : Text::No),
                    Choice, Tr(Text::MenuGeoOobFixDetail), UiIcon::Settings};
        case PsoBoost:
            return {Tr(Text::MenuPsoBoost),
                    Tr(settings.pso_boost == 0   ? Text::No
                       : settings.pso_boost == 2 ? Text::MenuPsoBoostAbove
                                                 : Text::MenuPsoBoostHighest),
                    Choice, Tr(Text::MenuPsoBoostDetail), UiIcon::Settings};
        case GlitchCapture:
            return {Tr(Text::MenuCaptureGlitches), {}, Action, Tr(Text::MenuCaptureGlitchesDetail)};
        case GlitchA2cShader:
            // Renderer switch names, the same in every language: the log and boot.cfg use them.
            return {L"a2c_shader", Tr(settings.glitch.a2c_shader ? Text::Yes : Text::No), Choice,
                    L"Alpha-to-coverage without MSAA dithered in the shader (off: 0.5 cut)"};
        case GlitchPreciseWrites:
            return {L"precise_writes", Tr(settings.glitch.precise_writes ? Text::Yes : Text::No),
                    Choice, L"CPU writes beside GPU-written bytes apply at once"};
        case GlitchArrayOwnMemory:
            return {L"array_own_memory", Tr(settings.glitch.array_own_memory ? Text::Yes : Text::No),
                    Choice, L"Large compressed texture arrays move to memory of their own"};
        case GlitchArrayRewrite:
            return {L"array_rewrite", Tr(settings.glitch.array_rewrite ? Text::Yes : Text::No),
                    Choice, L"Large compressed texture arrays are copied out and back in place"};
        case GlitchAstcArraysRgba:
            return {L"astc_arrays",
                    settings.glitch.astc_arrays == 1   ? L"RGBA8"
                    : settings.glitch.astc_arrays == 2 ? L"BC"
                                                       : L"Auto",
                    Choice, L"ASTC texture arrays: Auto probes each as BC, or all RGBA8, or all BC (reloads them)"};
        case MemoryEdit:
            return {Tr(Text::MemoryEditTitle), {}, Page,
                    memory_edit_hits != 0
                        ? I18n::Plural(Text::MemoryEditResultsOne, Text::MemoryEditResultsOther, memory_edit_hits)
                        : Tr(Text::MemoryEditRowIdle),
                    UiIcon::Cache};
        case Cheats:
            return {Tr(Text::CheatsTitle), {}, Page, I18n::Plural(Text::CheatsOnOne, Text::CheatsOnOther, cheats_on),
                    UiIcon::Cheats};
        case Mods:
            return {Tr(Text::ModsTitle), {}, GameMenuRow::Kind::Info,
                    I18n::Plural(Text::ModsActiveOne, Text::ModsActiveOther, mods_on) + L" · " +
                        Tr(Text::MenuModsAtBoot),
                    UiIcon::Mods};
        case Amiibo:
            if (amiibo_reader == AmiiboReader::Off) {
                return {Tr(Text::AmiiboTitle), {}, GameMenuRow::Kind::Info, Tr(Text::AmiiboNotLooking), UiIcon::User};
            }
            return {Tr(Text::AmiiboTitle), {}, Page,
                    amiibo_reader == AmiiboReader::Placed ? amiibo_name : Tr(Text::AmiiboLooking), UiIcon::User};
        case ControllerPage:
            return {Tr(Text::MenuController), {}, Page,
                    Tr(ControllerStyleText(settings.style)) + L" · " +
                        Tr(settings.swap_face_buttons ? Text::ByPosition : Text::ByLetter) + L" · " + DeadzoneText(),
                    UiIcon::Controller};
        case DisplayPage: {
            std::wstring detail;
            if (settings.show_performance_stats) detail = Tr(Text::MenuStatsShort);
            return {Tr(Text::MenuDisplay), {}, Page, detail, UiIcon::Display};
        }
        case GraphicsMenu: {
            const GraphicsSettings resolved = ResolveGraphics(settings.graphics_global, settings.graphics);
            return {Tr(Text::Graphics), {}, Page,
                    resolved.frame_gen ? Tr(Text::FrameGen) : GraphicsRowValue(resolved, GraphicsRow::Upscaling),
                    UiIcon::Settings};
        }
        case GfxUpscaling:
        case GfxFrameGen:
        case GfxShaders:
        case GfxGpuFence:
        case GfxFixedCoreMap: {
            // Whose value it is goes under the label: with the value it would not fit the row.
            const GraphicsRow row = RowOf(item);
            return {Tr(GraphicsRowTitle(row)),
                    GraphicsRowValue(ResolveGraphics(settings.graphics_global, settings.graphics), row), Choice,
                    Tr(HasGameOverride(settings.graphics, row) ? Text::MenuGraphicsOwn : Text::MenuGraphicsFollowsSettings)};
        }
        case GfxGuestClock:
        case GfxExactGpuReads:
        case GfxTextureMemory:
        case GfxGpuAccuracy:
        case GfxConsoleMode:
            return {Tr(GraphicsRowTitle(RowOf(item))),
                    GameGraphicsRowValue(settings.graphics_global, settings.graphics, RowOf(item)), Choice};
        case Style:
            return {Tr(Text::ControllerSeenAs), Tr(ControllerStyleText(settings.style)), Choice};
        case FaceButtons:
            return {Tr(Text::FaceButtons), Tr(settings.swap_face_buttons ? Text::ByPosition : Text::ByLetter),
                    Choice};
        case Deadzone:
            return {Tr(Text::DeadZone), DeadzoneText(), Choice};
        case RStickInvertX:
        case RStickInvertY:
        case RStickSensitivity:
        case RStickMouse:
        case RStickMouseSensitivity: {
            const RightStickSettings resolved = ResolveRightStick(settings.right_stick_global, settings.right_stick);
            const auto& game = settings.right_stick;
            const auto yes_no = [](bool on) { return Tr(on ? Text::Yes : Text::No); };
            const auto percent = [](int value) { return std::to_wstring(value) + L"%"; };
            Text label{};
            std::wstring value;
            bool own{};
            switch (item) {
            case RStickInvertX:
                label = Text::RightStickInvertX, value = yes_no(resolved.invert_x), own = game.invert_x.has_value();
                break;
            case RStickInvertY:
                label = Text::RightStickInvertY, value = yes_no(resolved.invert_y), own = game.invert_y.has_value();
                break;
            case RStickSensitivity:
                label = Text::RightStickSensitivity, value = percent(resolved.stick_sensitivity);
                own = game.stick_sensitivity.has_value();
                break;
            case RStickMouse:
                label = Text::RightStickMouse, value = yes_no(resolved.mouse), own = game.mouse.has_value();
                break;
            default:
                label = Text::MouseSensitivity, value = percent(resolved.mouse_sensitivity);
                own = game.mouse_sensitivity.has_value();
                break;
            }
            std::wstring detail;
            if (settings.right_stick_per_game)
                detail = Tr(own ? Text::MenuGraphicsOwn : Text::MenuGraphicsFollowsSettings);
            return {Tr(label), value, Choice, detail};
        }
        case Motion: {
            const bool on = settings.motion != MotionMode::Off;
            return {Tr(Text::MotionRow), Tr(on ? Text::Yes : Text::No), Choice,
                    Tr(on ? Text::MotionOnHint : Text::MotionOffHint)};
        }
        case GyroStick:
            return {Tr(Text::GyroStickRow),
                    Tr(settings.motion_stick == MotionStick::Left ? Text::GyroStickLeft : Text::GyroStickRight),
                    Choice, Tr(Text::GyroStickHint)};
        case GyroShake:
            return {Tr(Text::GyroShakeRow), GyroShakeText(settings.motion_shake), Choice, Tr(Text::GyroShakeHint)};
        case Rumble:
        case RumbleStrength: {
            const RumbleSettings resolved = ResolveRumble(settings.rumble_global, settings.rumble);
            const bool own = item == Rumble ? settings.rumble.on.has_value() : settings.rumble.strength.has_value();
            std::wstring detail = item == Rumble ? Tr(Text::RumbleHint) : std::wstring{};
            if (settings.right_stick_per_game)
                detail = Tr(own ? Text::MenuGraphicsOwn : Text::MenuGraphicsFollowsSettings);
            if (item == Rumble) return {Tr(Text::RumbleRow), Tr(resolved.on ? Text::Yes : Text::No), Choice, detail};
            return {Tr(Text::RumbleStrengthRow), std::to_wstring(resolved.strength) + L"%", Choice, detail};
        }
        case MotionView: {
            static constexpr Text values[]{Text::MotionViewAlways, Text::MotionViewMoving, Text::No};
            return {Tr(Text::MotionViewRow), Tr(values[static_cast<std::size_t>(settings.motion_view)]), Choice,
                    Tr(Text::MotionViewHint)};
        }
        case PerformanceStats:
            return {Tr(Text::PerformanceStats), Tr(settings.show_performance_stats ? Text::Yes : Text::No), Choice};
        case FullScreen:
            return {Tr(Text::MenuFullScreen), Tr(settings.fullscreen ? Text::Yes : Text::No), Choice};
        case OfflineMode: {
            const auto yes_no = [](bool on) { return Tr(on ? Text::Yes : Text::No); };
            return {Tr(Text::OfflineMode),
                    settings.offline_mode ? yes_no(*settings.offline_mode)
                                          : I18n::Format(Text::MenuOfflineSystem, {yes_no(settings.offline_default)}),
                    Choice, Tr(Text::MenuOfflineThisGame), UiIcon::Airplane};
        }
        case Library:
            if (!settings.pulse_sessions) return {Tr(Text::MenuBackToHome), {}, Action, {}, UiIcon::Exit};
            return {Tr(Text::MenuBackToHome), {}, Choice,
                    Tr(keep_session ? Text::MenuKeepSession : Text::MenuSkipSession), UiIcon::Exit};
        }
        return {};
    }

    /// The shake button's name: the Xbox pad's, which every controller page uses.
    static std::wstring GyroShakeText(MotionShakeButton button) {
        using I18n::Text;
        switch (button) {
        case MotionShakeButton::LB: return L"LB";
        case MotionShakeButton::RB: return L"RB";
        case MotionShakeButton::LT: return L"LT";
        case MotionShakeButton::RT: return L"RT";
        case MotionShakeButton::StickClick: break;
        }
        return Tr(Text::GyroShakeStickClick);
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
        case Chat:
            return confirm || action == MenuAction::Right ? GameMenuResult::Chat : GameMenuResult::None;
        case Screenshot:
            return confirm ? GameMenuResult::Screenshot : GameMenuResult::None;
        case CaptureGraphics:
            return confirm ? GameMenuResult::CaptureGraphics : GameMenuResult::None;
        case CapturePerformance:
            return confirm ? GameMenuResult::CapturePerformance : GameMenuResult::None;
        case MemoryEdit:
            return confirm ? GameMenuResult::MemoryEdit : GameMenuResult::None;
        case Cheats:
            return confirm ? GameMenuResult::Cheats : GameMenuResult::None;
        case Mods:
            return GameMenuResult::None;
        case Amiibo:
            return confirm && amiibo_reader != AmiiboReader::Off ? GameMenuResult::Amiibo : GameMenuResult::None;
        case ControllerPage:
        case DisplayPage:
        case GraphicsMenu:
        case GlitchPage:
            // A and Right open the page, as a row with ">" does elsewhere.
            if (back) return GameMenuResult::None;
            page = SelectedItem() == ControllerPage ? Page::Controller
                   : SelectedItem() == DisplayPage  ? Page::Display
                   : SelectedItem() == GlitchPage   ? Page::Glitch
                                                    : Page::Graphics;
            selected = 0;
            return GameMenuResult::None;
        case GfxUpscaling:
        case GfxFrameGen:
        case GfxShaders:
        case GfxGpuFence:
        case GfxGuestClock:
        case GfxExactGpuReads:
        case GfxTextureMemory:
        case GfxGpuAccuracy:
        case GfxConsoleMode:
        case GfxFixedCoreMap: {
            auto row = static_cast<std::size_t>(RowOf(SelectedItem()));
            const bool changed = HandleGameGraphicsInput(
                settings.graphics, row,
                back ? GraphicsInput::Left : confirm ? GraphicsInput::Confirm : GraphicsInput::Right);
            return changed ? GameMenuResult::GraphicsChanged : GameMenuResult::None;
        }
        case Library:
            if (!confirm) {
                if (settings.pulse_sessions) keep_session = !keep_session;
                return GameMenuResult::None;
            }
            return !settings.pulse_sessions || keep_session ? GameMenuResult::Library
                                                            : GameMenuResult::LibraryWithoutSession;
        case Style:
            settings.style = StepConsoleControllerStyle(settings.style, back);
            UpdateGraphicsItems();
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
        case RStickInvertX:
        case RStickInvertY:
        case RStickSensitivity:
        case RStickMouse:
        case RStickMouseSensitivity:
            ChangeRightStick(SelectedItem(), back);
            return GameMenuResult::RightStickChanged;
        case Motion:
            settings.motion = settings.motion == MotionMode::Off ? MotionMode::On : MotionMode::Off;
            return GameMenuResult::MotionChanged;
        case GyroStick:
            settings.motion_stick = settings.motion_stick == MotionStick::Right ? MotionStick::Left : MotionStick::Right;
            return GameMenuResult::GyroControlsChanged;
        case GyroShake: {
            const auto count = MotionShakeButtonCount;
            const auto index = static_cast<std::uint8_t>(settings.motion_shake);
            settings.motion_shake =
                static_cast<MotionShakeButton>(back ? (index + count - 1) % count : (index + 1) % count);
            return GameMenuResult::GyroControlsChanged;
        }
        case Rumble:
        case RumbleStrength: {
            const RumbleSettings& global = settings.rumble_global;
            RumbleSettings stepped = ResolveRumble(global, settings.rumble);
            if (SelectedItem() == Rumble) stepped.on = !stepped.on;
            else stepped.strength = StepSensitivity(RumbleStrengthSteps, stepped.strength, back);
            if (!settings.right_stick_per_game) {
                settings.rumble_global = stepped;
            } else if (SelectedItem() == Rumble) {
                settings.rumble.on = GameOverride(stepped.on, global.on);
            } else {
                settings.rumble.strength = GameOverride(stepped.strength, global.strength);
            }
            return GameMenuResult::RumbleChanged;
        }
        case MotionView: {
            const auto count = MotionViewModeCount;
            const auto index = static_cast<std::uint8_t>(settings.motion_view);
            settings.motion_view =
                static_cast<MotionViewMode>(back ? (index + count - 1) % count : (index + 1) % count);
            return GameMenuResult::MotionChanged;
        }
        case PerformanceStats:
            settings.show_performance_stats = !settings.show_performance_stats;
            return GameMenuResult::PerformanceStatsChanged;
        case FullScreen:
            settings.fullscreen = !settings.fullscreen;
            return GameMenuResult::FullScreen;
        case GlitchCapture:
            return confirm ? GameMenuResult::CaptureGlitches : GameMenuResult::None;
        case GlitchA2cShader:
            settings.glitch.a2c_shader = !settings.glitch.a2c_shader;
            return GameMenuResult::GlitchTogglesChanged;
        case GlitchPreciseWrites:
            settings.glitch.precise_writes = !settings.glitch.precise_writes;
            return GameMenuResult::GlitchTogglesChanged;
        case GlitchArrayOwnMemory:
            settings.glitch.array_own_memory = !settings.glitch.array_own_memory;
            return GameMenuResult::GlitchTogglesChanged;
        case GlitchArrayRewrite:
            settings.glitch.array_rewrite = !settings.glitch.array_rewrite;
            return GameMenuResult::GlitchTogglesChanged;
        case GlitchAstcArraysRgba:
            settings.glitch.astc_arrays = (settings.glitch.astc_arrays + 1) % 3;
            return GameMenuResult::GlitchTogglesChanged;
        case GeoMark:
            if (!confirm) return GameMenuResult::None;
            ++geo_marks;
            return GameMenuResult::GeoMark;
        case GeoOobFix:
            settings.geo_oob_fix = !settings.geo_oob_fix;
            return GameMenuResult::GeoOobFixChanged;
        case PsoBoost:
            // Steps through off, above normal, highest.
            settings.pso_boost = settings.pso_boost == 0 ? 2 : settings.pso_boost == 2 ? 3 : 0;
            return GameMenuResult::PsoBoostChanged;
        case OfflineMode: {
            // Steps through: as in Settings > System, on, off.
            auto& offline = settings.offline_mode;
            if (!offline) offline = !back;
            else if (*offline == !back) offline = back;
            else offline.reset();
            return GameMenuResult::OfflineModeChanged;
        }
        }
        return GameMenuResult::None;
    }

    /// Steps what the row shows. For a game, the result is its own unless it lands on Settings'
    /// value, which follows Settings again; without a game, Settings' value itself changes.
    void ChangeRightStick(Item item, bool back) {
        const RightStickSettings& global = settings.right_stick_global;
        const RightStickSettings resolved = ResolveRightStick(global, settings.right_stick);
        RightStickSettings stepped = resolved;
        switch (item) {
        case RStickInvertX: stepped.invert_x = !resolved.invert_x; break;
        case RStickInvertY: stepped.invert_y = !resolved.invert_y; break;
        case RStickSensitivity:
            stepped.stick_sensitivity = StepSensitivity(StickSensitivitySteps, resolved.stick_sensitivity, back);
            break;
        case RStickMouse: stepped.mouse = !resolved.mouse; break;
        default:
            stepped.mouse_sensitivity = StepSensitivity(MouseSensitivitySteps, resolved.mouse_sensitivity, back);
            break;
        }
        if (!settings.right_stick_per_game) {
            settings.right_stick_global = stepped;
            return;
        }
        auto& game = settings.right_stick;
        switch (item) {
        case RStickInvertX: game.invert_x = GameOverride(stepped.invert_x, global.invert_x); break;
        case RStickInvertY: game.invert_y = GameOverride(stepped.invert_y, global.invert_y); break;
        case RStickSensitivity:
            game.stick_sensitivity = GameOverride(stepped.stick_sensitivity, global.stick_sensitivity);
            break;
        case RStickMouse: game.mouse = GameOverride(stepped.mouse, global.mouse); break;
        default:
            game.mouse_sensitivity = GameOverride(stepped.mouse_sensitivity, global.mouse_sensitivity);
            break;
        }
    }

    GameMenuSettings settings;
    std::vector<Item> quick, controller, display, graphics, glitch;
    Page page{Page::Quick};
    std::size_t selected{};
    std::size_t cheats_on{};
    std::size_t mods_on{};
    AmiiboReader amiibo_reader{};
    std::wstring amiibo_name;
    std::wstring screenshot_note;
    std::uint64_t memory_edit_hits{};
    std::size_t chat_unread{};
    std::uint32_t geo_marks{};
    bool keep_session{true};
};
} // namespace Jadyx
