// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <stop_token>
#include "jadyx/updates/update_policy.h"
namespace Jadyx::Updates {
bool ValidatePackage(const std::filesystem::path& path, const Release& release,
                     std::stop_token stop = {});
}
