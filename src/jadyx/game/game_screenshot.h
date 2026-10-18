// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Screenshots from the in-game menu: the paused frame as a PNG in the app's LocalState.

#include <filesystem>
#include <optional>
#include <string>

#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace Jadyx {

/// Saves `frame` (RGBA rows) as "jadyx_YYYY-MM-DD_HH-MM-SS.png" in `folder`, created if needed,
/// never over an earlier file. Returns the file's name, or nothing when it could not be written.
std::optional<std::wstring> SaveScreenshot(const D3D12::MenuImage& frame, const std::filesystem::path& folder);

} // namespace Jadyx
