// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx::Compatibility {
inline constexpr std::size_t MaxBytes = 256 * 1024;
enum class Status { Untested, Playable, Issues, Broken };
struct Report {
    std::string name, console, fps_note, jadyx_version, note, source, product, report_url, date;
    Status status{Status::Untested};
    std::string date; ///< Optional report date; old catalogs leave it unknown.
};
using Table = std::map<std::uint64_t, std::vector<Report>>;
/// Invalid files fail as a whole, so they cannot hide the last valid cached or bundled list.
std::optional<Table> Parse(std::string_view bytes);
/// Views borrow the input storage. Invalid documents are ignored; undated files rank oldest.
std::optional<std::string_view> Newest(std::string_view remote, std::string_view cached,
                                       std::string_view bundled);
Table Select(std::string_view remote, std::string_view cached, std::string_view bundled);
/// Only the update bit is normalized; DLC and unrelated program IDs stay distinct.
const Report& Lookup(const Table& table, std::uint64_t title_id, std::string_view console = {});
} // namespace Jadyx::Compatibility
