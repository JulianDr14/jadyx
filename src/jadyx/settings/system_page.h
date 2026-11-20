// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings > System: the page's rows, what each shows and how input changes them. The library
// loop routes input here and acts on the returned effect (save, clear the cache, ...); the canvas
// draws from the same state and the layout below, which pointer hit tests share.

#include <algorithm>
#include <cstdint>
#include <optional>
#include "jadyx/updates/updater.h"
#include <string>

#include "jadyx/i18n/localization.h"
#include "jadyx/settings/system_settings.h"
#include "jadyx/ui/text_entry.h"
#include "jadyx/ui/text_entry_panel.h"

namespace Jadyx {

/// What the shared keyboard (SystemPage::name_editor) is typing. Settings > Online borrows it.
enum class EditorPurpose : std::uint8_t {
    UserName, OnlineNickname, OnlineAddress, OnlinePort, OnlinePassword, RoomPassword,
    HostName, HostSlots, HostPort, HostPassword, ChatMessage,
    DsuAddress,   ///< Settings > Controller > DSU controllers: a server's IP.
    NextendoServer, NextendoNat, ///< Online > Nextendo: the game and NAT servers' IPs.
};

/// Ports, counts and IP addresses take the numeric pad; names and server hosts the full keyboard.
constexpr bool NumericEditor(EditorPurpose purpose) {
    return purpose == EditorPurpose::OnlinePort || purpose == EditorPurpose::HostSlots ||
           purpose == EditorPurpose::HostPort || purpose == EditorPurpose::DsuAddress ||
           purpose == EditorPurpose::NextendoServer || purpose == EditorPurpose::NextendoNat;
}

struct SystemPage {
    enum Row : std::size_t { UserName, InterfaceLanguage, GameLanguage, Region, ClockFormat, GameDate, DateYear, DateMonth, DateDay, SurroundSound, AdultMods, PerformanceStats, PulseSessions, PlayTime, OfflineMode, MiiEditor, ClearCache, Updates, RowCount };

    bool updates_open{};
    /// Performance statistics' own rows (StatsRows), opened from its row.
    bool stats_open{};
    std::size_t stats_selected{};
    std::size_t update_row{}, update_detail{}, update_scroll{};
    Updates::Snapshot update;
    bool updated{}; ///< This launch confirmed a new version: shown until the next input.
    bool open{};
    std::size_t selected{};
    SystemSettings settings; ///< As saved.
    SystemLocale locale;     ///< As Windows reports it, for the "Automatic (...)" values.
    std::wstring notice;
    std::optional<TextEntry> name_editor;
    EditorPurpose editor_purpose{EditorPurpose::UserName};
    std::wstring editor_notice;
};

enum class SystemPageEffect : std::uint8_t {
    None,
    Changed,    ///< Settings changed: save and apply them.
    EditName,   ///< The name editor opened.
    NameDone,   ///< The editor closed with a new name in settings: save and apply.
    OpenUpdates,
    ClearCache, ///< Clear the library cache.
    OpenMiiEditor, ///< Leave the library to run the firmware's Mii editor.
};

enum class SystemInput : std::uint8_t { Up, Down, Left, Right, Confirm, Back };

namespace SystemPageDetail {
inline std::optional<std::wstring> StepUiLanguage(const std::wstring& current, bool back) {
    const auto languages = I18n::UiLanguages();
    const std::size_t count = languages.size() + 1;
    std::size_t slot = 0;
    for (std::size_t i = 0; i < languages.size(); ++i)
        if (languages[i].tag == current) slot = i + 1;
    slot = (slot + (back ? count - 1 : 1)) % count;
    return slot == 0 ? std::wstring{} : std::wstring{languages[slot - 1].tag};
}
} // namespace SystemPageDetail

/// Performance statistics' rows: show, profile, online lines, text size and, in Custom, each line.
inline SystemPageEffect HandleStatsInput(SystemPage& page, SystemInput input) {
    auto& settings = page.settings;
    auto& stats = settings.stats;
    const auto rows = StatsRows(stats);
    page.stats_selected = std::min(page.stats_selected, rows.size() - 1);
    switch (input) {
    case SystemInput::Up:
        page.stats_selected = (page.stats_selected + rows.size() - 1) % rows.size();
        return SystemPageEffect::None;
    case SystemInput::Down:
        page.stats_selected = (page.stats_selected + 1) % rows.size();
        return SystemPageEffect::None;
    case SystemInput::Back:
        page.stats_open = false;
        return SystemPageEffect::None;
    default:
        break;
    }
    const bool back = input == SystemInput::Left;
    switch (const StatsRow row = rows[page.stats_selected]) {
    case StatsRow::Show:     settings.show_performance_stats = !settings.show_performance_stats; break;
    case StatsRow::Profile:  stats.profile = StepStatsEnum(stats.profile, back); break;
    case StatsRow::Network:  stats.network = !stats.network; break;
    case StatsRow::TextSize: stats.text_size = StepStatsEnum(stats.text_size, back); break;
    default:                 stats.custom_items ^= StatsItemBit(RowItem(row)); break;
    }
    // Leaving Custom drops its rows; the focus stays on the profile.
    page.stats_selected = std::min(page.stats_selected, StatsRows(stats).size() - 1);
    return SystemPageEffect::Changed;
}

/// Applies one input to the page (not to the name editor, which the library drives directly).
inline SystemPageEffect HandleSystemInput(SystemPage& page, SystemInput input) {
    using Row = SystemPage::Row;
    if (page.stats_open) return HandleStatsInput(page, input);
    switch (input) {
    case SystemInput::Up:
        page.selected = (page.selected + Row::RowCount - 1) % Row::RowCount;
        return SystemPageEffect::None;
    case SystemInput::Down:
        page.selected = (page.selected + 1) % Row::RowCount;
        return SystemPageEffect::None;
    case SystemInput::Back:
        page.open = false;
        return SystemPageEffect::None;
    case SystemInput::Left:
    case SystemInput::Right:
    case SystemInput::Confirm:
        break;
    }
    const bool back = input == SystemInput::Left;
    auto& settings = page.settings;
    switch (static_cast<Row>(page.selected)) {
    case Row::UserName:
        if (input != SystemInput::Confirm) return SystemPageEffect::None;
        page.name_editor.emplace(settings.user_name, MaxUserNameLength);
        page.editor_purpose = EditorPurpose::UserName;
        page.editor_notice.clear();
        return SystemPageEffect::EditName;
    case Row::InterfaceLanguage:
        settings.ui_language = *SystemPageDetail::StepUiLanguage(settings.ui_language, back);
        return SystemPageEffect::Changed;
    case Row::GameLanguage:
        settings.game_language = StepChoice(settings.game_language, SelectableGameLanguages(), back);
        return SystemPageEffect::Changed;
    case Row::Region:
        settings.region = StepChoice(settings.region, SelectableRegions(), back);
        return SystemPageEffect::Changed;
    case Row::ClockFormat:
        settings.clock_24h = !settings.clock_24h;
        return SystemPageEffect::Changed;
    case Row::GameDate:
        // Automatic <-> custom; a new custom date starts at today's.
        settings.game_date = settings.game_date ? std::nullopt : std::optional<GameDate>{TodayLocal()};
        return SystemPageEffect::Changed;
    case Row::DateYear:
    case Row::DateMonth:
    case Row::DateDay: {
        const auto field = page.selected == Row::DateYear    ? GameDateField::Year
                           : page.selected == Row::DateMonth ? GameDateField::Month
                                                             : GameDateField::Day;
        settings.game_date = StepGameDate(settings.game_date.value_or(TodayLocal()), field, back);
        return SystemPageEffect::Changed;
    }
    case Row::SurroundSound:
        settings.surround_sound = !settings.surround_sound;
        return SystemPageEffect::Changed;
    case Row::AdultMods:
        settings.show_adult_mods = !settings.show_adult_mods;
        return SystemPageEffect::Changed;
    case Row::PerformanceStats:
        if (input != SystemInput::Confirm) return SystemPageEffect::None;
        page.stats_open = true;
        page.stats_selected = 0;
        return SystemPageEffect::None;
    case Row::PulseSessions:
        settings.pulse_sessions = !settings.pulse_sessions;
        return SystemPageEffect::Changed;
    case Row::PlayTime:
        settings.track_play_time = !settings.track_play_time;
        return SystemPageEffect::Changed;
    case Row::OfflineMode:
        settings.offline_mode = !settings.offline_mode;
        return SystemPageEffect::Changed;
    case Row::Updates:
        return input == SystemInput::Confirm ? SystemPageEffect::OpenUpdates : SystemPageEffect::None;
    case Row::ClearCache:
        return input == SystemInput::Confirm ? SystemPageEffect::ClearCache : SystemPageEffect::None;
    case Row::MiiEditor:
        return input == SystemInput::Confirm ? SystemPageEffect::OpenMiiEditor : SystemPageEffect::None;
    case Row::RowCount:
        break;
    }
    return SystemPageEffect::None;
}

/// Closes the name editor; `accept` keeps its text. An empty name is refused with a notice.
inline SystemPageEffect FinishNameEditor(SystemPage& page, bool accept) {
    if (!page.name_editor) return SystemPageEffect::None;
    if (!accept) {
        page.name_editor.reset();
        return SystemPageEffect::None;
    }
    auto name = SanitizeUserName(page.name_editor->Value());
    if (name.empty()) {
        page.editor_notice = I18n::Tr(I18n::Text::NameEmpty);
        return SystemPageEffect::None;
    }
    page.name_editor.reset();
    // The default name is stored as "no name": it then follows any future default.
    page.settings.user_name = name == DefaultUserName ? std::wstring{} : std::move(name);
    return SystemPageEffect::NameDone;
}

inline I18n::Text StatsProfileText(StatsProfile profile) {
    using I18n::Text;
    switch (profile) {
    case StatsProfile::Standard: return Text::StatsProfileStandard;
    case StatsProfile::Minimal:  return Text::StatsProfileMinimal;
    case StatsProfile::Custom:   return Text::StatsProfileCustom;
    default:                     return Text::StatsProfileFull;
    }
}
inline I18n::Text StatsProfileHint(StatsProfile profile) {
    using I18n::Text;
    switch (profile) {
    case StatsProfile::Standard: return Text::StatsProfileStandardHint;
    case StatsProfile::Minimal:  return Text::StatsProfileMinimalHint;
    case StatsProfile::Custom:   return Text::StatsProfileCustomHint;
    default:                     return Text::StatsProfileFullHint;
    }
}
inline I18n::Text StatsTextSizeText(StatsTextSize size) {
    using I18n::Text;
    switch (size) {
    case StatsTextSize::Small: return Text::StatsSizeSmall;
    case StatsTextSize::Large: return Text::StatsSizeLarge;
    case StatsTextSize::Huge:  return Text::StatsSizeHuge;
    default:                   return Text::StatsSizeNormal;
    }
}
/// Toggle rows: whether each is on.
inline bool StatsRowOn(const SystemSettings& settings, StatsRow row) {
    switch (row) {
    case StatsRow::Show:    return settings.show_performance_stats;
    case StatsRow::Network: return settings.stats.network;
    case StatsRow::Profile:
    case StatsRow::TextSize: return false;
    default:                return (settings.stats.custom_items & StatsItemBit(RowItem(row))) != 0;
    }
}

inline I18n::Text SystemRowTitle(SystemPage::Row row) {
    using I18n::Text;
    switch (row) {
    case SystemPage::Updates:           return Text::UpdaterTitle;
    case SystemPage::UserName:          return Text::UserName;
    case SystemPage::InterfaceLanguage: return Text::InterfaceLanguage;
    case SystemPage::GameLanguage:      return Text::GameLanguage;
    case SystemPage::Region:            return Text::ConsoleRegion;
    case SystemPage::ClockFormat:       return Text::ClockFormat;
    case SystemPage::GameDate:          return Text::GameDate;
    case SystemPage::DateYear:          return Text::DateYear;
    case SystemPage::DateMonth:         return Text::DateMonth;
    case SystemPage::DateDay:           return Text::DateDay;
    case SystemPage::SurroundSound:     return Text::SurroundSound;
    case SystemPage::PerformanceStats:  return Text::PerformanceStats;
    case SystemPage::AdultMods:         return Text::ModsAdult;
    case SystemPage::PulseSessions:     return Text::PulseSessions;
    case SystemPage::PlayTime:          return Text::PlayTimeTracking;
    case SystemPage::OfflineMode:       return Text::OfflineMode;
    case SystemPage::MiiEditor:         return Text::MiiEditor;
    default:                            return Text::ClearLibraryCache;
    }
}

inline I18n::Text SystemRowHint(SystemPage::Row row) {
    using I18n::Text;
    switch (row) {
    case SystemPage::Updates:           return Text::UpdaterHint;
    case SystemPage::UserName:          return Text::UserNameHint;
    case SystemPage::InterfaceLanguage: return Text::InterfaceLanguageHint;
    case SystemPage::GameLanguage:      return Text::GameLanguageHint;
    case SystemPage::Region:            return Text::ConsoleRegionHint;
    case SystemPage::ClockFormat:       return Text::ClockFormatHint;
    case SystemPage::GameDate:          return Text::GameDateHint;
    case SystemPage::DateYear:
    case SystemPage::DateMonth:
    case SystemPage::DateDay:           return Text::DateFieldHint;
    case SystemPage::SurroundSound:     return Text::SurroundSoundHint;
    case SystemPage::PerformanceStats:  return Text::PerformanceStatsHint;
    case SystemPage::AdultMods:         return Text::ModsAdultHint;
    case SystemPage::PulseSessions:     return Text::PulseSessionsHint;
    case SystemPage::PlayTime:          return Text::PlayTimeTrackingHint;
    case SystemPage::OfflineMode:       return Text::OfflineModeHint;
    case SystemPage::MiiEditor:         return Text::MiiEditorHint;
    default:                            return Text::ClearLibraryCacheHint;
    }
}

/// The row's current value; choices that follow the system read "Automatic · <what it is>".
inline std::wstring SystemRowValue(const SystemPage& page, SystemPage::Row row) {
    using I18n::Text;
    const auto& settings = page.settings;
    const auto automatic = [](std::wstring_view value) {
        return I18n::Format(Text::AutomaticValue, {value});
    };
    switch (row) {
    case SystemPage::UserName:
        return settings.user_name.empty() ? std::wstring{DefaultUserName} : settings.user_name;
    case SystemPage::InterfaceLanguage:
        if (const auto* language = I18n::FindUiLanguage(settings.ui_language))
            return std::wstring{language->native_name};
        return automatic(I18n::NegotiateUiLanguage(I18n::UiLanguages(), page.locale.languages).native_name);
    case SystemPage::GameLanguage:
        if (settings.game_language) return std::wstring{GameLanguageName(*settings.game_language)};
        return automatic(GameLanguageName(GameLanguageForLocale(page.locale)));
    case SystemPage::Region:
        if (settings.region) return std::wstring{I18n::Tr(RegionText(*settings.region))};
        return automatic(I18n::Tr(RegionText(RegionForLocale(page.locale))));
    case SystemPage::ClockFormat:
        return std::wstring{I18n::Tr(settings.clock_24h ? Text::Clock24h : Text::Clock12h)};
    case SystemPage::GameDate:
        if (settings.game_date) return I18n::Format(Text::GameDateCustom, {FormatGameDate(*settings.game_date)});
        return automatic(FormatGameDate(TodayLocal()));
    case SystemPage::DateYear:
    case SystemPage::DateMonth:
    case SystemPage::DateDay: {
        // Automatic shows today's, which the first change starts from.
        const auto date = ClampGameDate(settings.game_date.value_or(TodayLocal()));
        const int value = row == SystemPage::DateYear ? date.year : row == SystemPage::DateMonth ? date.month : date.day;
        return std::to_wstring(value);
    }
    case SystemPage::SurroundSound:
        return std::wstring{I18n::Tr(settings.surround_sound ? Text::Yes : Text::No)};
    case SystemPage::PerformanceStats:
        return std::wstring{I18n::Tr(settings.show_performance_stats ? StatsProfileText(settings.stats.profile) : Text::No)};
    case SystemPage::AdultMods:
        return std::wstring{I18n::Tr(settings.show_adult_mods ? Text::ModsAdultShown : Text::ModsAdultHidden)};
    case SystemPage::PulseSessions:
        return std::wstring{I18n::Tr(settings.pulse_sessions ? Text::Yes : Text::No)};
    case SystemPage::PlayTime:
        return std::wstring{I18n::Tr(settings.track_play_time ? Text::Yes : Text::No)};
    case SystemPage::OfflineMode:
        return std::wstring{I18n::Tr(settings.offline_mode ? Text::Yes : Text::No)};
    default:
        return {};
    }
}

inline I18n::Text StatsRowTitle(StatsRow row) {
    using I18n::Text;
    switch (row) {
    case StatsRow::Show:      return Text::StatsShow;
    case StatsRow::Profile:   return Text::StatsProfile;
    case StatsRow::Network:   return Text::StatsNetwork;
    case StatsRow::TextSize:  return Text::StatsTextSize;
    case StatsRow::Fps:       return Text::StatsLineFps;
    case StatsRow::FrameTime: return Text::StatsLineFrameTime;
    case StatsRow::Cpu:       return Text::StatsLineCpu;
    case StatsRow::Gpu:       return Text::StatsLineGpu;
    case StatsRow::Memory:    return Text::StatsLineMemory;
    default:                  return Text::StatsLineHitch;
    }
}

/// The hint under a row; the profile's says which lines it shows.
inline std::wstring StatsRowHint(const SystemSettings& settings, StatsRow row) {
    using I18n::Text;
    switch (row) {
    case StatsRow::Show:     return std::wstring{I18n::Tr(Text::StatsShowHint)};
    case StatsRow::Profile:  return std::wstring{I18n::Tr(StatsProfileHint(settings.stats.profile))};
    case StatsRow::Network:  return std::wstring{I18n::Tr(Text::StatsNetworkHint)};
    case StatsRow::TextSize: return std::wstring{I18n::Tr(Text::StatsTextSizeHint)};
    default:                 return {};
    }
}

inline std::wstring StatsRowValue(const SystemSettings& settings, StatsRow row) {
    using I18n::Text;
    const auto yes_no = [](bool on) { return std::wstring{I18n::Tr(on ? Text::Yes : Text::No)}; };
    switch (row) {
    case StatsRow::Show:     return yes_no(settings.show_performance_stats);
    case StatsRow::Profile:  return std::wstring{I18n::Tr(StatsProfileText(settings.stats.profile))};
    case StatsRow::Network:  return yes_no(settings.stats.network);
    case StatsRow::TextSize: return std::wstring{I18n::Tr(StatsTextSizeText(settings.stats.text_size))};
    default:                 return yes_no(StatsRowOn(settings, row));
    }
}

/// Geometry shared by the canvas and pointer hit tests (1280x720 layout units).
struct SystemPageLayout {
    static constexpr float PanelX = 140, PanelY = 72, PanelWidth = 1000, PanelHeight = 612;
    static constexpr float RowX = 180, RowWidth = 920, RowTop = 166, RowStride = 47, RowHeight = 45;
    static constexpr float Notice = 594, Footer = 630;
    /// The name editor's keyboard panel (text_entry_panel.h), centered on the screen.
    static constexpr float EditorX = (1280 - TextEntryPanelLayout::Width) / 2;
    static constexpr float EditorY = (720 - TextEntryPanelLayout::Height) / 2;
};

} // namespace Jadyx
