// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
namespace Jadyx::Updates::Storage {
std::optional<std::string> ReadBounded(const std::filesystem::path& path,
                                       std::size_t limit = 1u << 20);
bool Persist(const std::filesystem::path& path, std::string_view body);
} // namespace Jadyx::Updates::Storage
