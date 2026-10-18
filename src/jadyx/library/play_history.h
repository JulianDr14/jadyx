// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Play history: when each game file was last played and, while "Play time" is on (Settings >
// System), how long. The library is ordered by it: the last game played always comes first, then
// the most played (or, with play time off, the most recently played), then the games never played.
// Kept in LocalState/jadyx/play_history.txt and saved during play, so closing the app from the
// dashboard loses at most a minute. Pure logic up to the persistence block at the end.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "jadyx/library/game_library.h"
#include "jadyx/i18n/localization.h"

namespace Jadyx {

struct PlayRecord {
    std::uint64_t seconds{};    ///< Time played while tracking was on.
    std::int64_t last_played{}; ///< Unix seconds of the last launch; 0 = never.
    std::uint32_t sessions{};   ///< Launches.
    std::uint64_t program_id{}; ///< The game's, as booted; 0 for homebrew. Feeds Eden's playtime.bin.
};

/// Keyed by the library entry's launch_path.
using PlayHistory = std::map<std::string, PlayRecord, std::less<>>;

namespace PlayHistoryDetail {
inline constexpr std::string_view Header = "jadyx-play-history 1";

template <typename T>
bool ParseNumber(std::string_view text, T& out, int base = 10) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out, base);
    return error == std::errc{} && end == text.data() + text.size();
}

/// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
struct Date {
    std::int64_t year;
    unsigned month, day;
};
inline Date CivilFromDays(std::int64_t days) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2), month, day};
}
inline std::int64_t LocalDay(std::int64_t unix_seconds, std::int64_t utc_offset) {
    const std::int64_t local = unix_seconds + utc_offset;
    return local >= 0 ? local / 86400 : (local - 86399) / 86400;
}
} // namespace PlayHistoryDetail

/// One line per game: last played, seconds, sessions, program id (hex) and the launch path,
/// separated by tabs, which no path can hold. Unreadable lines are skipped, never fatal.
inline PlayHistory ParsePlayHistory(std::string_view text) {
    using namespace PlayHistoryDetail;
    PlayHistory history;
    bool header = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!header) {
            if (line != Header) return {}; // Unknown format: start over rather than misread it.
            header = true;
            continue;
        }
        std::string_view fields[5];
        std::size_t count = 0;
        for (; count < 4; ++count) {
            const auto tab = line.find('\t');
            if (tab == std::string_view::npos) break;
            fields[count] = line.substr(0, tab);
            line.remove_prefix(tab + 1);
        }
        if (count != 4 || line.empty()) continue;
        fields[4] = line;
        PlayRecord record;
        if (!ParseNumber(fields[0], record.last_played) || !ParseNumber(fields[1], record.seconds) ||
            !ParseNumber(fields[2], record.sessions) || !ParseNumber(fields[3], record.program_id, 16))
            continue;
        history.insert_or_assign(std::string{fields[4]}, record);
    }
    return history;
}

inline std::string SerializePlayHistory(const PlayHistory& history) {
    std::string out{PlayHistoryDetail::Header};
    out += '\n';
    for (const auto& [path, record] : history) {
        if (path.empty() || path.find_first_of("\t\r\n") != std::string::npos) continue;
        char pid[17];
        const auto end = std::to_chars(pid, pid + sizeof(pid), record.program_id, 16).ptr;
        out += std::to_string(record.last_played) + '\t' + std::to_string(record.seconds) + '\t' +
               std::to_string(record.sessions) + '\t' + std::string(pid, end) + '\t' + path + '\n';
    }
    return out;
}

/// Library order. The last game played first; then, with play time on, the most played (ties by
/// the most recent), or with it off the most recently played; games never played last, by path.
inline void OrderLibrary(std::vector<LibraryEntry>& entries, const PlayHistory& history,
                         bool by_play_time) {
    const auto record = [&](const LibraryEntry& entry) -> PlayRecord {
        const auto it = history.find(entry.launch_path);
        return it == history.end() ? PlayRecord{} : it->second;
    };
    std::int64_t newest = 0;
    for (const auto& entry : entries) newest = std::max(newest, record(entry).last_played);
    std::stable_sort(entries.begin(), entries.end(), [&](const LibraryEntry& a, const LibraryEntry& b) {
        const auto ra = record(a), rb = record(b);
        const bool played_a = ra.last_played > 0, played_b = rb.last_played > 0;
        if (played_a != played_b) return played_a;
        if (!played_a) return a.launch_path < b.launch_path;
        const bool first_a = ra.last_played == newest, first_b = rb.last_played == newest;
        if (first_a != first_b) return first_a;
        if (by_play_time && ra.seconds != rb.seconds) return ra.seconds > rb.seconds;
        if (ra.last_played != rb.last_played) return ra.last_played > rb.last_played;
        return a.launch_path < b.launch_path;
    });
}

/// "45 min", "12 h 30 min", "120 h"; "Under a minute" below one. Hours drop minutes from 100 h.
inline std::wstring FormatPlayTime(std::uint64_t seconds) {
    using I18n::Text;
    const auto hours = seconds / 3600, minutes = seconds / 60 % 60;
    if (seconds < 60) return std::wstring{I18n::Tr(Text::PlayTimeUnderMinute)};
    if (hours == 0) return I18n::Format(Text::PlayTimeMinutes, {std::to_wstring(minutes)});
    if (minutes == 0 || hours >= 100) return I18n::Format(Text::PlayTimeHours, {std::to_wstring(hours)});
    return I18n::Format(Text::PlayTimeHoursMinutes, {std::to_wstring(hours), std::to_wstring(minutes)});
}

/// For a tile: "45 min" under an hour, else whole hours ("12 h").
inline std::wstring FormatPlayTimeShort(std::uint64_t seconds) {
    using I18n::Text;
    if (seconds < 3600) return I18n::Format(Text::PlayTimeMinutes, {std::to_wstring(seconds / 60)});
    return I18n::Format(Text::PlayTimeHours, {std::to_wstring(seconds / 3600)});
}

/// "Today", "Yesterday", "3 days ago" within a week, else the date. Days are local calendar days.
inline std::wstring FormatLastPlayed(std::int64_t last_played, std::int64_t now, std::int64_t utc_offset) {
    using namespace PlayHistoryDetail;
    using I18n::Text;
    const std::int64_t days = LocalDay(now, utc_offset) - LocalDay(last_played, utc_offset);
    if (days <= 0) return std::wstring{I18n::Tr(Text::PlayedToday)};
    if (days == 1) return std::wstring{I18n::Tr(Text::PlayedYesterday)};
    if (days < 7) return I18n::Format(Text::PlayedDaysAgo, {std::to_wstring(days)});
    const auto date = CivilFromDays(LocalDay(last_played, utc_offset));
    return I18n::Format(Text::PlayedOnDate,
                        {std::to_wstring(date.day), std::to_wstring(date.month), std::to_wstring(date.year)});
}

/// Counts the time a game runs. Each step adds the time since the previous one, capped so that a
/// suspended console or a stalled frame never counts as play; paused steps add nothing.
class PlayClock {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto MaxStep = std::chrono::seconds{1};

    explicit PlayClock(Clock::time_point now) : last{now} {}

    /// Whole seconds to add now.
    std::uint64_t Step(Clock::time_point now, bool counting) {
        const auto elapsed = now - last;
        last = now;
        if (!counting || elapsed <= Clock::duration::zero()) return 0;
        pending += std::min<Clock::duration>(elapsed, MaxStep);
        const auto whole = std::chrono::duration_cast<std::chrono::seconds>(pending);
        pending -= whole;
        return static_cast<std::uint64_t>(whole.count());
    }

private:
    Clock::time_point last;
    Clock::duration pending{};
};

// ---- Persistence (play_history.cpp) ----

PlayHistory LoadPlayHistory();
bool SavePlayHistory(const PlayHistory& history);
/// Seconds east of UTC right now, for FormatLastPlayed.
std::int64_t LocalUtcOffset();
std::int64_t UnixNow();

/// One play of a game, driven by the boot loop. Records the launch at once (the order needs it
/// even with play time off), then counts play time while the game runs, saving it every minute
/// and at the end. Play time also reaches Eden's playtime.bin, which the pdm:qry service reports
/// to games.
class PlaySession {
public:
    PlaySession(std::string launch_path, std::uint64_t program_id);
    ~PlaySession(); ///< Saves what is left.
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    /// Once per loop iteration; `running` is false while the game is paused (menu open).
    void Tick(bool running);
    void Finish();

private:
    void Flush();

    std::string launch_path;
    std::uint64_t program_id;
    bool tracking;
    PlayClock clock;
    std::uint64_t unsaved{};
    PlayClock::Clock::time_point last_save;
    bool finished{};
};

} // namespace Jadyx
