// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "jadyx/library/favorites.h"
#include "jadyx/library/game_library.h"
#include "jadyx/storage/nxbox_import.h"
namespace Jadyx {
struct NxboxWelcome {
    bool visible{};
    NxboxImport::Receipt receipt;
    std::vector<std::string> usb_folders;
};
NxboxWelcome ImportNxboxSetup();
bool DismissNxboxWelcome();
// Returns true when records were resolved and persisted.
// Resolves staged title IDs after metadata/USB authorization supplies launch paths.
bool ResolveNxboxLibrary(const std::vector<LibraryEntry>& entries, Favorites& favorites);
} // namespace Jadyx
