// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/i18n/localization.cpp and src/jadyx/i18n/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include "jadyx/library/play_history.h"

int main() {
    using namespace Jadyx;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));

    // The file: a round trip, and lines that cannot be read are skipped.
    PlayHistory history;
    history["C:\\games\\a.nsp"] = {7200, 1'760'000'000, 3, 0x0100ABCD00000000};
    history["ext:token/b.xci"] = {0, 1'760'100'000, 1, 0};
    const auto text = SerializePlayHistory(history);
    const auto back = ParsePlayHistory(text);
    check(back.size() == 2);
    const auto& a = back.at("C:\\games\\a.nsp");
    check(a.seconds == 7200 && a.last_played == 1'760'000'000 && a.sessions == 3 &&
          a.program_id == 0x0100ABCD00000000);
    check(ParsePlayHistory(text + "garbage\n1\t2\n").size() == 2);
    check(ParsePlayHistory("old format\n1\t2\t3\t4\tpath\n").empty());
    check(ParsePlayHistory("").empty());
    PlayHistory odd;
    odd["bad\tpath"] = {1, 1, 1, 0};
    check(ParsePlayHistory(SerializePlayHistory(odd)).empty());

    // Order with play time on: the last played first, then the most played, then never played.
    const auto entry = [](const char* path) {
        LibraryEntry e;
        e.launch_path = path;
        return e;
    };
    const auto order = [&](const PlayHistory& h, bool by_play_time) {
        std::vector<LibraryEntry> entries{entry("a"), entry("b"), entry("c"), entry("d"), entry("e")};
        OrderLibrary(entries, h, by_play_time);
        std::string out;
        for (const auto& e : entries) out += e.launch_path;
        return out;
    };
    PlayHistory played;
    played["b"] = {600, 300, 1, 0};   // little time, played last
    played["c"] = {9000, 100, 5, 0};  // most played
    played["d"] = {3000, 200, 2, 0};
    played["e"] = {3000, 250, 2, 0};  // same time as d, but more recent
    check(order(played, true) == "bceda");
    // With play time off: by last play only.
    check(order(played, false) == "bedca");
    // Nothing played: the scan's order by path.
    check(order({}, true) == "abcde" && order({}, false) == "abcde");

    // Play time text.
    check(FormatPlayTime(30) == L"Menos de un minuto");
    check(FormatPlayTime(45 * 60) == L"45 min");
    check(FormatPlayTime(3600) == L"1 h");
    check(FormatPlayTime(12 * 3600 + 30 * 60 + 59) == L"12 h 30 min");
    check(FormatPlayTime(150 * 3600 + 60) == L"150 h");
    check(FormatPlayTimeShort(59 * 60) == L"59 min" && FormatPlayTimeShort(12 * 3600 + 1800) == L"12 h");

    // Last play in local calendar days (UTC-5 here): 23:00 local yesterday is "Ayer".
    constexpr std::int64_t offset = -5 * 3600;
    constexpr std::int64_t now = 1'760'659'200 + 15 * 3600; // 2025-10-17 10:00 local
    check(FormatLastPlayed(now - 60, now, offset) == L"Hoy");
    check(FormatLastPlayed(now - 11 * 3600, now, offset) == L"Ayer");
    check(FormatLastPlayed(now - 3 * 86400, now, offset) == L"Hace 3 días");
    check(FormatLastPlayed(now - 30 * 86400, now, offset) == L"17/9/2025");

    // The clock: paused and stalled steps do not count; fractions carry over.
    using namespace std::chrono_literals;
    const auto t0 = PlayClock::Clock::time_point{} + 1h;
    PlayClock clock{t0};
    check(clock.Step(t0 + 600ms, true) == 0);
    check(clock.Step(t0 + 1200ms, true) == 1);
    check(clock.Step(t0 + 2min, false) == 0);
    check(clock.Step(t0 + 10min, true) == 1); // a suspended console counts at most a second
    std::uint64_t total = 0;
    for (int i = 1; i <= 500; ++i) total += clock.Step(t0 + 10min + i * 20ms, true);
    check(total == 10);

    std::puts("play history: file, library order, play time and last play text, clock PASS");
}
