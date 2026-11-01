// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

namespace Jadyx {

/// Copies the data a local test package carries into the app's writable folders.
void SeedUserData(const std::filesystem::path& install, const std::filesystem::path& local);

} // namespace Jadyx
