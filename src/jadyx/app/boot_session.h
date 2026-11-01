// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "common/common_types.h"
#include "jadyx/app/boot_config.h"

namespace Jadyx {

/// RunHeadlessBoot's status when the player chose "back to the library" in the in-game menu.
constexpr int RETURN_TO_LIBRARY = 10;
/// A Pulse session failed to resume after the game was loaded into this System: boot it again,
/// fresh (BootConfig::pulse_resume off). The session stays where it was.
constexpr int PULSE_RESUME_FAILED = 18;
/// The library asked for the Mii editor and the firmware does not have it.
constexpr int MII_EDITOR_MISSING = 19;
/// The game's program has no ExeFS of its own (a sparse base) and no update brings one.
constexpr int UPDATE_REQUIRED = 20;

/// Where the renderer presents: the CoreWindow (as IUnknown*) and its size in physical pixels.
/// A null window selects the Null renderer.
struct BootSurface {
    void* core_window{};
    u32 width{1920};
    u32 height{1080};
    /// The sizes this game can change to from the in-game menu, by what its graphics ask for: the
    /// TV's resolution and the plain one (equal when boot.cfg forces one). Read on the UI thread.
    u32 screen_width{};
    u32 screen_height{};
    u32 plain_width{};
    u32 plain_height{};
};

/// Boots launch_path on `surface` and runs it until it ends. Returns a process exit-style status: 0
/// when the boot reached the run phase cleanly.
int RunHeadlessBoot(const std::string& launch_path, const BootSurface& surface,
                    const BootConfig& config);

} // namespace Jadyx
