// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <stop_token>
#include <string>
namespace Jadyx {
/// Structural/CNMT/header validation, not a full payload hash scan. Does not import tickets.
std::string ValidateUploadedNsp(const std::filesystem::path& path, std::stop_token stop);
}
