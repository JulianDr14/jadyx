// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace Jadyx {

/// Dev Home runs an app as an App (about 1 GiB of memory on the Series) or as a Game (about
/// 5 GiB). Anything under 3 GiB is the App limit; 0 (the limit could not be read) is not.
inline bool IsAppModeMemory(std::uint64_t limit_bytes) {
    return limit_bytes != 0 && limit_bytes < (std::uint64_t{3} << 30);
}

} // namespace Jadyx
