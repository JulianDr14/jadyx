// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <string_view>

namespace Jadyx {
// The boot.cfg alias exists because a console package has no shell environment.
inline bool move_to_usb_boot_option{};
inline bool MoveToUsbEnabled() {
    const char* value = std::getenv("NXBOX_MOVE_TO_USB");
    return move_to_usb_boot_option || (value && std::string_view{value} == "1");
}
} // namespace Jadyx
