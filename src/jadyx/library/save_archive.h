// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace Jadyx {

/// Canonicalizes a relative ZIP path and rejects names unsafe or unimportable on Windows.
std::optional<std::string> NormalizeSaveArchivePath(std::string path);

/// Writes a bounded, uncompressed ZIP that the portal's save importer can read.
std::string ExportSaveArchive(
    const std::filesystem::path& source, const std::filesystem::path& output, std::stop_token stop,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});

} // namespace Jadyx
