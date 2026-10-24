// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Versioned transfer metadata only. Payloads are copied by NXbox to Jadyx's normal storage.
#include "jadyx/settings/resolution_options.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace Jadyx::NxboxImport {
inline constexpr std::size_t MaxFileBytes = 1024 * 1024;
using Value = std::variant<std::string, bool, std::int32_t>;
struct Setting {
    std::wstring key;
    Value value;
};
struct Game {
    std::uint64_t program_id{};
    bool pinned{};
    std::uint64_t seconds{};
    std::int64_t last_played{};
    std::uint32_t sessions{};
};
struct Receipt {
    bool keys{};
    std::string firmware;
    std::uint32_t saves{}, mods{};
};
using Json = nlohmann::json;
inline std::optional<Json> Document(std::string_view text) {
    if (text.size() > MaxFileBytes)
        return std::nullopt;
    try {
        auto doc = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32)
                throw std::runtime_error("NXbox metadata nesting limit");
            return true;
        });
        if (!doc.is_object() || !doc.contains("version") || !doc["version"].is_number_integer() ||
            doc["version"] != 1)
            return std::nullopt;
        return doc;
    } catch (...) {
        return std::nullopt;
    }
}
inline std::optional<std::uint64_t> ProgramId(std::string_view text) {
    if (text.size() != 16)
        return std::nullopt;
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
        return std::nullopt;
    return value;
}
inline std::string ProgramIdText(std::uint64_t id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t i = 0; i < 16; ++i)
        text[i] = digits[(id >> ((15 - i) * 4)) & 15];
    return text;
}
inline std::optional<std::int32_t> Resolution(const Json& raw) {
    if (!raw.is_number())
        return std::nullopt;
    using R = Settings::ResolutionSetup;
    const std::pair<double, R> choices[]{{0.5, R::Res1_2X}, {0.75, R::Res3_4X}, {1, R::Res1X},
                                         {1.5, R::Res3_2X}, {2, R::Res2X},      {3, R::Res3X}};
    for (const auto& [scale, choice] : choices)
        if (raw == scale)
            return static_cast<std::int32_t>(choice);
    return std::nullopt;
}
inline std::optional<std::vector<Setting>> ParseSettings(std::string_view text) {
    const auto doc = Document(text);
    if (!doc)
        return std::nullopt;
    std::vector<Setting> settings;
    if (doc->contains("language") && (*doc)["language"].is_string()) {
        auto language = (*doc)["language"].get<std::string>();
        if (language == "pt_br" || language == "pt-BR")
            language = "pt-BR";
        if (language == "en" || language == "es" || language == "pt-BR")
            settings.push_back({.key = L"system_ui_language", .value = language});
    }
    if (doc->contains("resolution"))
        if (const auto choice = Resolution((*doc)["resolution"]))
            settings.push_back({.key = ResolutionKey(0), .value = *choice});
    // Layout describes physical face-button mapping. The UI confirm button has no equivalent:
    // Jadyx navigation always confirms with A, so confirm_button is deliberately ignored.
    if (doc->contains("controller_layout") && (*doc)["controller_layout"].is_string()) {
        const auto layout = (*doc)["controller_layout"].get<std::string>();
        if (layout == "xbox" || layout == "switch")
            settings.push_back({.key = L"controller_swap_face", .value = layout == "switch"});
    }
    if (doc->contains("per_game") && (*doc)["per_game"].is_object()) {
        for (const auto& [id, options] : (*doc)["per_game"].items()) {
            const auto application = ProgramId(id);
            if (!application || !options.is_object() || !options.contains("resolution"))
                continue;
            if (const auto choice = Resolution(options["resolution"]))
                settings.push_back({.key = ResolutionKey(*application), .value = *choice});
        }
    }
    return settings;
}
// Presence, rather than equality with a default, protects explicitly saved defaults too.
template <typename Has, typename Write>
std::size_t ApplySettings(const std::vector<Setting>& settings, Has&& has, Write&& write) {
    std::size_t count{};
    for (const auto& setting : settings) {
        if (has(setting.key))
            continue;
        write(setting.key, setting.value);
        ++count;
    }
    return count;
}
inline bool Unsigned(const Json& doc, const char* key, std::uint64_t maximum) {
    if (!doc.contains(key))
        return true;
    const auto& raw = doc[key];
    return raw.is_number_integer() && (raw.is_number_unsigned() || raw.get<std::int64_t>() >= 0) &&
           raw.get<std::uint64_t>() <= maximum;
}
inline std::optional<std::vector<Game>> ParseLibrary(std::string_view text) {
    const auto doc = Document(text);
    if (!doc || !doc->contains("games") || !(*doc)["games"].is_array() ||
        (*doc)["games"].size() > 4096)
        return std::nullopt;
    std::vector<Game> games;
    for (const auto& item : (*doc)["games"]) {
        if (!item.is_object() || !item.contains("program_id") || !item["program_id"].is_string())
            continue;
        const auto id = ProgramId(item["program_id"].get<std::string>());
        if (!id || !Unsigned(item, "seconds", UINT64_MAX) ||
            !Unsigned(item, "sessions", UINT32_MAX) || !Unsigned(item, "last_played", INT64_MAX) ||
            (item.contains("pinned") && !item["pinned"].is_boolean()))
            continue;
        Game game;
        game.program_id = *id;
        game.pinned = item.value("pinned", false);
        game.seconds = item.value("seconds", std::uint64_t{});
        game.sessions = item.value("sessions", std::uint32_t{});
        game.last_played = item.value("last_played", std::int64_t{});
        if (std::ranges::none_of(games, [&](const auto& old) { return old.program_id == *id; }))
            games.push_back(game);
    }
    return games;
}
inline std::string SerializeLibrary(const std::vector<Game>& games) {
    Json doc = {{"version", 1}, {"games", Json::array()}};
    for (const auto& game : games)
        doc["games"].push_back({{"program_id", ProgramIdText(game.program_id)},
                                {"pinned", game.pinned},
                                {"seconds", game.seconds},
                                {"last_played", game.last_played},
                                {"sessions", game.sessions}});
    return doc.dump();
}
inline void MergeCounters(const Game& game, std::uint64_t& seconds) {
    seconds = std::max(seconds, game.seconds);
}
inline std::vector<std::string> ParseUsbFolders(std::string_view text) {
    std::vector<std::string> folders;
    if (text.size() > MaxFileBytes)
        return folders;
    while (!text.empty() && folders.size() < 64) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.size() > 1024 ||
            std::ranges::any_of(line, [](unsigned char c) { return c < 32 || c == 127; }))
            continue;
        if (std::ranges::find(folders, line) == folders.end())
            folders.emplace_back(line);
    }
    return folders;
}
inline std::optional<Receipt> ParseReceipt(std::string_view text) {
    const auto doc = Document(text);
    if (!doc || !Unsigned(*doc, "saves", UINT32_MAX) || !Unsigned(*doc, "mods", UINT32_MAX) ||
        (doc->contains("keys") && !(*doc)["keys"].is_boolean()) ||
        (doc->contains("firmware") && !(*doc)["firmware"].is_string()))
        return std::nullopt;
    Receipt receipt;
    receipt.keys = doc->value("keys", false);
    receipt.firmware = doc->value("firmware", std::string{});
    if (receipt.firmware.size() > 64 ||
        std::ranges::any_of(receipt.firmware, [](unsigned char c) { return c < 32 || c == 127; }))
        return std::nullopt;
    receipt.saves = doc->value("saves", std::uint32_t{});
    receipt.mods = doc->value("mods", std::uint32_t{});
    return receipt;
}
} // namespace Jadyx::NxboxImport
