// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/compatibility.h"

#include <charconv>
#include <nlohmann/json.hpp>

namespace Jadyx::Compatibility {
std::optional<Table> Parse(std::string_view bytes) {
    if (bytes.size() > MaxBytes)
        return std::nullopt;
    const auto root = nlohmann::json::parse(bytes, nullptr, false);
    if (!root.is_object() || !root.contains("schema_version") || root["schema_version"] != 1 ||
        !root.contains("titles") || !root["titles"].is_object())
        return std::nullopt;
    Table table;
    for (const auto& [key, rows] : root["titles"].items()) {
        std::uint64_t title_id{};
        const auto result = std::from_chars(key.data(), key.data() + key.size(), title_id, 16);
        if (key.size() != 16 || result.ec != std::errc{} || result.ptr != key.data() + key.size() ||
            (title_id >> 56) != 1 || (title_id & 0x1FFF) != 0 || !rows.is_array() || rows.empty())
            return std::nullopt;
        if (table.contains(title_id))
            return std::nullopt;
        auto& reports = table[title_id];
        for (const auto& row : rows) {
            if (!row.is_object())
                return std::nullopt;
            for (const auto* field : {"title_id", "name", "status", "console", "fps_note",
                                      "jadyx_version", "note", "source", "product"}) {
                if (!row.contains(field) || !row[field].is_string() ||
                    row[field].get_ref<const std::string&>().size() > 1024)
                    return std::nullopt;
            }
            if (row["title_id"] != key || (row["console"] != "X" && row["console"] != "S") ||
                (row["product"] != "Jadyx" && row["product"] != "NXbox") || row["name"] == "" ||
                row["source"] == "")
                return std::nullopt;
            Report report;
            report.name = row["name"];
            report.console = row["console"];
            report.fps_note = row["fps_note"];
            report.jadyx_version = row["jadyx_version"];
            report.note = row["note"];
            report.source = row["source"];
            report.product = row["product"];
            const std::string status = row["status"];
            if (status == "playable")
                report.status = Status::Playable;
            else if (status == "issues")
                report.status = Status::Issues;
            else if (status == "broken")
                report.status = Status::Broken;
            reports.push_back(std::move(report));
        }
    }
    return table;
}

bool RefreshEnabled(std::string_view environment, std::string_view configuration) {
    if (!environment.empty())
        return environment == "1";
    bool enabled = false;
    while (!configuration.empty()) {
        const auto end = configuration.find('\n');
        auto line = configuration.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.starts_with("NXBOX_COMPAT_REFRESH="))
            enabled = line == "NXBOX_COMPAT_REFRESH=1";
        if (end == std::string_view::npos)
            break;
        configuration.remove_prefix(end + 1);
    }
    return enabled;
}

Table Select(std::string_view remote, std::string_view cached, std::string_view bundled) {
    for (const auto bytes : {remote, cached, bundled}) {
        if (auto table = Parse(bytes))
            return std::move(*table);
    }
    return {};
}

const Report& Lookup(const Table& table, std::uint64_t title_id, std::string_view console) {
    const auto found = table.find(title_id & ~std::uint64_t{0x800});
    static const Report untested;
    if (found == table.end())
        return untested;
    // Reports on another emulator/model are never a claim about this one. With no known model,
    // choose the most cautious Jadyx report and keep its model visible in the focus text.
    const Report* selected = nullptr;
    const auto rank = [](Status status) {
        switch (status) {
        case Status::Broken:
            return 3;
        case Status::Issues:
            return 2;
        case Status::Untested:
            return 1;
        default:
            return 0;
        }
    };
    for (const auto& report : found->second) {
        if (report.product != "Jadyx" || (!console.empty() && report.console != console))
            continue;
        if (!selected || rank(report.status) > rank(selected->status))
            selected = &report;
    }
    return selected ? *selected : untested;
}
} // namespace Jadyx::Compatibility
