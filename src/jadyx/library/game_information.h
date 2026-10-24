// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "jadyx/content/content_model.h"
#include "jadyx/library/compatibility.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/play_history.h"
#include "jadyx/storage/storage_path.h"

namespace Jadyx {
struct GameContentCounts {
    std::size_t updates{}, dlc{};
};
/// Count retained update versions and distinct DLC titles, including disabled installations.
/// Merely inspecting information never adopts embedded packages or changes activation.
inline GameContentCounts CountGameContent(std::span<const Content::Revision> revisions,
                                          std::uint64_t application) {
    GameContentCounts counts;
    std::vector<std::uint64_t> titles;
    if (!application)
        return counts;
    for (const auto& revision : revisions) {
        if (revision.application != application)
            continue;
        if (revision.kind == Content::Kind::Update)
            ++counts.updates;
        else if (std::find(titles.begin(), titles.end(), revision.title) == titles.end()) {
            titles.push_back(revision.title);
            ++counts.dlc;
        }
    }
    return counts;
}

/// Read once on a worker when the information page opens; absent values mean unavailable.
struct GameInformationFacts {
    std::optional<std::uint64_t> bytes;
    std::optional<GameContentCounts> content;
};
struct GameInformationData {
    std::wstring program_id, version, format, size, storage, path, updates, dlc;
    std::wstring last_played, play_time;
    I18n::Text compatibility{I18n::Text::CompatibilityUntested};
    bool reported{};
    Compatibility::Report report;
};

inline bool CanRemoveGame(const LibraryEntry& game) {
    return game.program_id != 0 && !IsStoragePath(game.launch_path);
}

/// IEC units, one truncated decimal, entirely integer arithmetic (including UINT64_MAX).
inline std::wstring FormatGameSize(std::uint64_t bytes) {
    constexpr std::wstring_view units[]{L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
    std::size_t unit = 0;
    std::uint64_t divisor = 1;
    while (unit + 1 < std::size(units) && bytes / divisor >= 1024) {
        divisor *= 1024;
        ++unit;
    }
    auto text = std::to_wstring(bytes / divisor);
    if (unit)
        text += L"." + std::to_wstring((bytes % divisor) * 10 / divisor);
    return text + L" " + std::wstring{units[unit]};
}

/// No I/O, title matching or compatibility inference in presentation. The caller supplies the
/// existing catalog's selected report. Untracked history stays absent instead of inventing it.
inline GameInformationData BuildGameInformation(const LibraryEntry& game,
                                                const GameInformationFacts& facts,
                                                const Compatibility::Report& report) {
    using I18n::Text;
    GameInformationData data;
    const std::wstring unknown{I18n::Tr(Text::GameInfoUnknown)};
    if (game.program_id) {
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (int shift = 60; shift >= 0; shift -= 4)
            data.program_id += hex[(game.program_id >> shift) & 15];
    } else
        data.program_id = unknown;
    data.version = game.display_version.empty() ? unknown : game.display_version;
    data.format = game.relative_path.extension().wstring();
    if (!data.format.empty())
        data.format.erase(0, 1);
    for (auto& character : data.format)
        if (character >= L'a' && character <= L'z')
            character = static_cast<wchar_t>(character - (L'a' - L'A'));
    if (data.format.empty())
        data.format = unknown;
    data.size = facts.bytes ? FormatGameSize(*facts.bytes) : unknown;
    const bool external = IsStoragePath(game.launch_path);
    data.storage = external ? I18n::Format(Text::GameInfoExternal, {game.source_name})
                            : std::wstring{I18n::Tr(Text::SourceInternal)};
    // Token-scoped identities are not physical drive paths. Show the authorized source and
    // its relative path, exactly as the library sees them, rather than exposing a token.
    data.path = external ? game.relative_path.wstring()
                         : std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(
                                                                   game.launch_path.data()),
                                                               game.launch_path.size()}}
                               .wstring();
    data.updates = facts.content ? std::to_wstring(facts.content->updates) : unknown;
    data.dlc = facts.content ? std::to_wstring(facts.content->dlc) : unknown;
    if (game.play_seconds)
        data.play_time = FormatPlayTime(game.play_seconds);
    if (game.last_played > 0) {
        const auto date = PlayHistoryDetail::CivilFromDays(game.last_played / 86400);
        data.last_played = I18n::Format(
            Text::PlayedOnDate,
            {std::to_wstring(date.day), std::to_wstring(date.month), std::to_wstring(date.year)});
    }
    data.reported = report.product == "Jadyx" && !report.console.empty() &&
                    report.status != Compatibility::Status::Untested;
    if (data.reported) {
        data.report = report;
        switch (report.status) {
        case Compatibility::Status::Playable:
            data.compatibility = Text::CompatibilityPlayable;
            break;
        case Compatibility::Status::Issues:
            data.compatibility = Text::CompatibilityIssues;
            break;
        case Compatibility::Status::Broken:
            data.compatibility = Text::CompatibilityBroken;
            break;
        default:
            break;
        }
    }
    return data;
}
} // namespace Jadyx
