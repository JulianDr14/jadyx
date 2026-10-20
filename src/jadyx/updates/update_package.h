// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace Jadyx::Updates {
struct UpdatePackage {
    std::filesystem::path path;
    std::string version, sha256;
    std::uint64_t size{};
    bool operator==(const UpdatePackage&) const = default;
    std::string FileName() const {
        return "Jadyx_" + version + "_x64.appx";
    }
};
} // namespace Jadyx::Updates
