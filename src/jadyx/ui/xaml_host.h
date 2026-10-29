// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace Jadyx::XamlHost {

void Initialize();
void* RenderSurface() noexcept;
bool IsMediaPageOpen() noexcept;
void OpenBrowser();
void OpenVideoPlayer();
bool HandleMediaController(float right_x, float right_y, bool confirm, bool back, bool alternate,
                           bool play_pause, bool left, bool right, bool up, bool down);
void CloseMediaPage();
void AttachSwapChain(void* swap_chain);

} // namespace Jadyx::XamlHost
