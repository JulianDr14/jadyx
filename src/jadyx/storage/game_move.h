// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <winrt/Windows.Storage.h>
#include "jadyx/library/game_library.h"
#include "jadyx/storage/move_option.h"

namespace Jadyx {
// Clean interrupted copies before the startup scan, using the same serial worker ownership.
void CleanupGameMoveStaging(const winrt::Windows::Storage::StorageFolder& destination,
                            std::stop_token stop);

// Called only by the library's serial MTA storage worker, with portal mutations blocked.
std::string MoveInternalGame(const std::filesystem::path& games, const LibraryEntry& game,
                             const winrt::Windows::Storage::StorageFolder& destination,
                             std::stop_token stop,
                             const std::function<void(std::uint64_t, std::uint64_t)>& progress);
} // namespace Jadyx
