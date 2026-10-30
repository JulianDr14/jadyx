// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include "jadyx/settings/graphics_settings.h"
#include "jadyx/settings/system_settings.h"

namespace Service::Account {
class ProfileManager;
}

namespace Jadyx {
/// Saved in the app's LocalSettings, like the controller options.
SystemSettings LoadSystemSettings();
bool SaveSystemSettings(const SystemSettings& settings);

/// A game's own offline mode, set from the in-game menu; nullopt follows SystemSettings.
std::optional<bool> LoadGameOfflineMode(std::uint64_t program_id);
bool SaveGameOfflineMode(std::uint64_t program_id, std::optional<bool> offline);

/// Settings > Graphics, saved in LocalSettings. Keys of their own: an install that saved the old
/// in-game upscaling starts with these defaults (FSR on).
GraphicsSettings LoadGraphicsSettings();
bool SaveGraphicsSettings(const GraphicsSettings& settings);

/// A game's own graphics, set from its options in the library; nullopt fields follow
/// Settings > Graphics.
GameGraphics LoadGameGraphics(std::uint64_t program_id);
bool SaveGameGraphics(std::uint64_t program_id, const GameGraphics& graphics);

/// Windows' preferred languages and home region (the console's, on Xbox). Never throws.
SystemLocale DetectSystemLocale();

/// Resolves the saved settings against the system and makes the result current: the interface
/// language for the frontend, Eden's system language and region for the next game, and the upscaling
/// of Settings > Graphics (a game's own is applied once its title id is known).
ResolvedSystem ApplySystemSettings();

/// Makes upscaling Eden's scaling settings, which the D3D12 present reads every frame. On the GPU
/// thread while a game runs (GPU::RunOnGpuThread), the thread that reads them.
void ApplyUpscaling(Upscaling upscaling);

/// Renames the active profile to the saved user name (the default one when none is saved), before
/// a game starts. Returns false when the profile could not be written.
bool ApplyUserName(Service::Account::ProfileManager& profiles, std::wstring_view name);
} // namespace Jadyx
