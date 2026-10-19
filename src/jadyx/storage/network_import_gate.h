// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <string>
namespace Jadyx {
// Opt-in real-file AppContainer diagnostic. Never alters the source library.
bool RunNetworkImportGate(const std::filesystem::path& local,
                          const std::function<void(std::string)>& diagnostic);
}
