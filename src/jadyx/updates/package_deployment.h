// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <stop_token>
namespace Jadyx::Updates {
struct DeploymentResult {
    bool registered{};
    std::uint32_t error{};
    bool submitted{};
};
// Windows owns replacement and reopening once the request has been submitted.
DeploymentResult DeployPackage(const std::filesystem::path& path, std::stop_token stop);
} // namespace Jadyx::Updates
