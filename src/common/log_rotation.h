// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace Common {

/// The name a rotated log takes: jadyx_diag.txt -> jadyx_diag.old.txt.
[[nodiscard]] inline std::filesystem::path RotatedLogPath(const std::filesystem::path& path) {
    auto old_path = path;
    old_path.replace_extension(".old" + path.extension().string());
    return old_path;
}

/// For logs that every launch appends to. Once `path` holds more than `max_bytes`, it replaces
/// its .old copy and the next write starts a new file: disk use stays under twice the cap and
/// the history up to the latest launch survives, a crash's last lines included. Returns true
/// when it rotated. Best effort: a failure leaves the log as it was.
inline bool RotateLogIfLarger(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size <= max_bytes) {
        return false;
    }
    const auto old_path = RotatedLogPath(path);
    std::filesystem::remove(old_path, ec);
    std::filesystem::rename(path, old_path, ec);
    return !ec;
}

} // namespace Common
