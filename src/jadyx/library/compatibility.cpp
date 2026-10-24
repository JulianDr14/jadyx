// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/compatibility.h"

#include <charconv>
#include <chrono>
#include <nlohmann/json.hpp>

namespace Jadyx::Compatibility {
namespace {
// ISO 8601 UTC, with optional fractional seconds (up to nanosecond precision).
// Normalize fractions so lexical order also handles whole seconds and milliseconds.
std::optional<std::string> Timestamp(std::string_view text) {
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text.back() != 'Z')
        return std::nullopt;
    const auto number = [text](std::size_t offset, std::size_t length) {
        if (text.substr(offset, length).find_first_not_of("0123456789") != std::string_view::npos)
            return -1;
        int value{};
        const auto result =
            std::from_chars(text.data() + offset, text.data() + offset + length, value);
        return result.ec == std::errc{} && result.ptr == text.data() + offset + length ? value : -1;
    };
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    const auto date = std::chrono::year_month_day{std::chrono::year{year},
                                                  std::chrono::month{static_cast<unsigned>(month)},
                                                  std::chrono::day{static_cast<unsigned>(day)}};
    if (year < 1 || !date.ok() || number(11, 2) < 0 || number(11, 2) > 23 || number(14, 2) < 0 ||
        number(14, 2) > 59 || number(17, 2) < 0 || number(17, 2) > 59)
        return std::nullopt;
    std::string fraction;
    if (text.size() != 20) {
        if (text.size() < 22 || text.size() > 30 || text[19] != '.')
            return std::nullopt;
        fraction = text.substr(20, text.size() - 21);
        if (fraction.find_first_not_of("0123456789") != std::string::npos)
            return std::nullopt;
    }
    fraction.resize(9, '0');
    return std::string{text.substr(0, 19)} + "." + fraction + "Z";
}
} // namespace
std::optional<Table> Parse(std::string_view bytes) {
    if (bytes.size() > MaxBytes)
        return std::nullopt;
    const auto root = nlohmann::json::parse(bytes, nullptr, false);
    if (!root.is_object() || !root.contains("schema_version") || root["schema_version"] != 1 ||
        !root.contains("titles") || !root["titles"].is_object())
        return std::nullopt;
    if (root.contains("generated") &&
        (!root["generated"].is_string() || !Timestamp(root["generated"].get<std::string>())))
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
            for (const auto* field :
                 {"title_id", "name", "status", "console", "fps_note", "note", "product"}) {
                if (!row.contains(field) || !row[field].is_string() ||
                    row[field].get_ref<const std::string&>().size() > 1024)
                    return std::nullopt;
            }
            for (const auto* field : {"jadyx_version", "report_url", "date", "source"}) {
                if (row.contains(field) && (!row[field].is_string() ||
                                            row[field].get_ref<const std::string&>().size() > 1024))
                    return std::nullopt;
            }
            if (row["title_id"] != key || (row["console"] != "X" && row["console"] != "S") ||
                (row["product"] != "Jadyx" && row["product"] != "NXbox") || row["name"] == "")
                return std::nullopt;
            Report report;
            report.name = row["name"];
            report.console = row["console"];
            report.fps_note = row["fps_note"];
            report.jadyx_version = row.value("jadyx_version", "");
            report.report_url = row.value("report_url", "");
            report.date = row.value("date", "");
            report.note = row["note"];
            report.source = row.value("source", "");
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

std::optional<std::string_view> Newest(std::string_view remote, std::string_view cached,
                                       std::string_view bundled) {
    std::optional<std::string_view> selected;
    std::string newest;
    // Stable ties: downloaded, cached, bundled. Undated legacy documents rank below dated ones.
    for (const auto bytes : {remote, cached, bundled}) {
        if (!Parse(bytes))
            continue;
        const auto root = nlohmann::json::parse(bytes);
        const auto generated = root.value("generated", "");
        const auto timestamp = generated.empty() ? std::string{} : *Timestamp(generated);
        if (!selected || timestamp > newest) {
            selected = bytes;
            newest = timestamp;
        }
    }
    return selected;
}

Table Select(std::string_view remote, std::string_view cached, std::string_view bundled) {
    const auto selected = Newest(remote, cached, bundled);
    return selected ? std::move(*Parse(*selected)) : Table{};
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
