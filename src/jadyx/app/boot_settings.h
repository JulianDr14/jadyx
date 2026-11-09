// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a session sets before it starts: boot.cfg over the settings the core and the renderer read,
// and later the game's own language and graphics.

#include "jadyx/app/boot_config.h"
#include "jadyx/app/boot_session.h"
#include "jadyx/settings/graphics_settings.h"

namespace Core {
class System;
}

namespace Jadyx {

struct GlitchToggles;

/// Everything boot.cfg decides, before Core::System exists. Sets the settings for this session
/// alone: they outlive it, so each session sets them all.
void ApplyBootConfig(const BootConfig& config, const BootSurface& surface, bool mii_editor);

void ApplyGameLanguage(Core::System& system);

/// The game's graphics (its own over Settings > Graphics) where boot.cfg sets none.
void ApplyGameGraphics(Core::System& system, const BootConfig& config, const GameGraphics& game);

/// The advanced graphics that can change while the game runs, those that differ from `before`.
void ApplyRunningAdvancedGraphics(Core::System& system, const GameAdvancedGraphics& before,
                                  const GameAdvancedGraphics& now);

/// The switches the geometry glitch hunt page starts with: boot.cfg's values.
GlitchToggles GlitchTogglesOf(const BootConfig& config);
/// The glitch hunt page's switches, live; logs each one that differs from `before`.
void ApplyGlitchToggles(const GlitchToggles& before, const GlitchToggles& now);

} // namespace Jadyx
