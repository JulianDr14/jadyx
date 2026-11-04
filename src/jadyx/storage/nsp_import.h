// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <stop_token>
#include <string>
namespace Jadyx {
/// Structural/CNMT/header validation, not a full payload hash scan. Does not import tickets.
std::string ValidateUploadedNsp(const std::filesystem::path& path, std::stop_token stop);
/// A base game uploaded as NSP, XCI or NRO, told apart by its header. `name` is the file name it
/// is published under: its extension must name the same format.
std::string ValidateUploadedGame(const std::filesystem::path& path, const std::string& name, std::stop_token stop);
}
