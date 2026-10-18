// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/play_history.h"

#include <ctime>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "frontend_common/play_time_manager.h"
#include "jadyx/storage/atomic_file.h"
#include "jadyx/settings/uwp_system_settings.h"

namespace Jadyx {
namespace {
constexpr auto SaveEvery = std::chrono::seconds{60};

std::filesystem::path HistoryPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "play_history.txt";
}
} // namespace

PlayHistory LoadPlayHistory() {
    const auto text = ReadWholeFile(HistoryPath());
    return text ? ParsePlayHistory(*text) : PlayHistory{};
}

bool SavePlayHistory(const PlayHistory& history) {
    if (WriteFileAtomically(HistoryPath(), SerializePlayHistory(history))) return true;
    LOG_ERROR(Frontend, "Play history: could not save {}", Common::FS::PathToUTF8String(HistoryPath()));
    return false;
}

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t LocalUtcOffset() {
    const std::time_t now = std::time(nullptr);
    std::tm local{}, utc{};
    if (localtime_s(&local, &now) != 0 || gmtime_s(&utc, &now) != 0) return 0;
    // mktime reads both as local time, so their difference is the offset (DST included).
    local.tm_isdst = utc.tm_isdst = 0;
    return static_cast<std::int64_t>(std::difftime(std::mktime(&local), std::mktime(&utc)));
}

PlaySession::PlaySession(std::string launch_path_, std::uint64_t program_id_)
    : launch_path{std::move(launch_path_)}, program_id{program_id_},
      tracking{LoadSystemSettings().track_play_time}, clock{PlayClock::Clock::now()},
      last_save{PlayClock::Clock::now()} {
    auto history = LoadPlayHistory();
    auto& record = history[launch_path];
    record.last_played = UnixNow();
    ++record.sessions;
    if (program_id != 0) record.program_id = program_id;
    SavePlayHistory(history);
    LOG_INFO(Frontend, "Play history: launch {} of {:016X}, {} s played so far{}", record.sessions,
             program_id, record.seconds, tracking ? "" : " (play time off)");
}

PlaySession::~PlaySession() {
    Finish();
}

void PlaySession::Tick(bool running) {
    if (finished) return;
    const auto now = PlayClock::Clock::now();
    unsaved += clock.Step(now, tracking && running);
    if (unsaved > 0 && now - last_save >= SaveEvery) Flush();
}

void PlaySession::Finish() {
    if (finished) return;
    Flush();
    finished = true;
}

void PlaySession::Flush() {
    last_save = PlayClock::Clock::now();
    if (unsaved == 0) return;
    const auto seconds = std::exchange(unsaved, 0);
    auto history = LoadPlayHistory();
    history[launch_path].seconds += seconds;
    SavePlayHistory(history);
    if (program_id != 0) {
        PlayTime::PlayTimeManager eden;
        eden.SetPlayTime(program_id, eden.GetPlayTime(program_id) + seconds);
    }
}

} // namespace Jadyx
