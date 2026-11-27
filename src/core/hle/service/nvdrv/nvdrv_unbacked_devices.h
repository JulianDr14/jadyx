// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace Service::Nvidia {

/// Device nodes that exist on the console but have no backend here. Opening one answers
/// NotSupported, which callers treat as "feature off", instead of the FileOperationFailed that an
/// unknown name gets.
///  - nvhost-prof-gpu / nvhost-dbg-gpu: GPU profiler and debugger, closed on retail units.
///  - nverpt-ctrl (3.0.0+): telemetry for the console's error reports; nothing receives it.
inline constexpr std::array<std::string_view, 3> UnbackedDeviceNames{
    "/dev/nvhost-prof-gpu",
    "/dev/nvhost-dbg-gpu",
    "/dev/nverpt-ctrl",
};

[[nodiscard]] constexpr bool IsUnbackedDevice(std::string_view name) {
    return std::ranges::find(UnbackedDeviceNames, name) != UnbackedDeviceNames.end();
}

} // namespace Service::Nvidia
