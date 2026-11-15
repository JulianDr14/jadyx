// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// DSU controllers in words, the same in the in-game menu and in Settings > Controller.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "jadyx/i18n/localization.h"
#include "jadyx/input/dsu_protocol.h"
#include "jadyx/input/dsu_status.h"

namespace Jadyx {

/// The address part of a pad's key, as the server rows show it.
[[nodiscard]] inline std::wstring DsuKeyAddress(std::uint64_t key) {
    return Dsu::FormatAddress({static_cast<std::uint32_t>(key >> 24), static_cast<std::uint16_t>((key >> 8) & 0xFFFF)});
}

/// The Search row's value: searching, or what the last search found.
[[nodiscard]] inline std::wstring DsuSearchValue(const DsuStatus& status) {
    using I18n::Text;
    if (status.searching) return std::wstring{I18n::Tr(Text::DsuSearching)};
    if (!status.last_found) return {};
    if (*status.last_found == 0) return std::wstring{I18n::Tr(Text::DsuFoundNone)};
    return I18n::Format(Text::DsuFound, {std::to_wstring(*status.last_found)});
}
[[nodiscard]] inline std::wstring DsuSearchHint(const DsuStatus& status) {
    return I18n::Format(I18n::Text::DsuSearchHint, {status.console_address.empty() ? L"?" : status.console_address});
}

[[nodiscard]] inline std::wstring DsuServerStateText(const DsuServerStatus& server) {
    using I18n::Text;
    switch (server.state) {
    case DsuServerState::Connected: return std::wstring{I18n::Tr(Text::DsuConnected)};
    case DsuServerState::Lost: return std::wstring{I18n::Tr(Text::DsuLost)};
    case DsuServerState::Connecting: break;
    }
    return std::wstring{I18n::Tr(Text::DsuConnecting)};
}
[[nodiscard]] inline std::wstring DsuServerHint(const DsuServerStatus& server) {
    using I18n::Text;
    switch (server.state) {
    case DsuServerState::Connected:
        return I18n::Format(Text::DsuServerHint, {std::to_wstring(server.pads.size()), std::to_wstring(server.rate_hz)});
    case DsuServerState::Lost: return std::wstring{I18n::Tr(Text::DsuLostHint)};
    case DsuServerState::Connecting: break;
    }
    return std::wstring{I18n::Tr(Text::DsuConnectingHint)};
}

/// "Controller 1".."Controller 4", by the server's slot.
[[nodiscard]] inline std::wstring DsuPadName(const DsuPadStatus& pad) {
    return I18n::Format(I18n::Text::DsuPadRow, {std::to_wstring(pad.slot + 1)});
}
/// Who the pad plays, and for Auto which player it took (or that it waits for a button).
[[nodiscard]] inline std::wstring DsuAssignText(const DsuPadStatus& pad) {
    using I18n::Text;
    switch (pad.assign) {
    case DsuAssign::Auto:
        return pad.player != 0 ? I18n::Format(Text::DsuAssignAutoPlayer, {std::to_wstring(pad.player)})
                               : std::wstring{I18n::Tr(Text::DsuAssignAutoWaiting)};
    case DsuAssign::GyroOnly: return std::wstring{I18n::Tr(Text::DsuAssignGyro)};
    case DsuAssign::Off: return std::wstring{I18n::Tr(Text::DsuAssignOff)};
    default: return I18n::Format(Text::DsuAssignPlayer, {std::to_wstring(DsuPinnedPlayer(pad.assign))});
    }
}
[[nodiscard]] inline std::wstring DsuPadHint(const DsuPadStatus& pad) {
    return std::wstring{I18n::Tr(pad.motion ? I18n::Text::DsuPadMotionHint : I18n::Text::DsuPadNoMotionHint)};
}

/// The DSU page's row on the Controller page: how many servers and pads.
[[nodiscard]] inline std::wstring DsuSummaryText(const DsuStatus& status) {
    if (status.servers.empty()) return std::wstring{I18n::Tr(I18n::Text::DsuNone)};
    return I18n::Format(I18n::Text::DsuSummary,
                        {std::to_wstring(status.servers.size()), std::to_wstring(status.PadCount())});
}

/// The gyro's pad: Automatic, or "address · Controller n".
[[nodiscard]] inline std::wstring GyroDsuPadText(const DsuStatus& status) {
    if (status.gyro_pad == 0) return std::wstring{I18n::Tr(I18n::Text::GyroDsuPadAuto)};
    return DsuKeyAddress(status.gyro_pad) + L" · " +
           I18n::Format(I18n::Text::DsuPadRow, {std::to_wstring(DsuPadSlot(status.gyro_pad) + 1)});
}
[[nodiscard]] inline std::wstring GyroDsuPadHint(const DsuStatus& status) {
    return std::wstring{
        I18n::Tr(status.MotionPads().empty() ? I18n::Text::GyroDsuNoPad : I18n::Text::GyroDsuPadHint)};
}
/// The next gyro pad for left and right: Automatic, then each pad that sends motion.
[[nodiscard]] inline std::uint64_t StepGyroDsuPad(const DsuStatus& status, bool back) {
    std::vector<std::uint64_t> choices{0};
    for (const std::uint64_t key : status.MotionPads()) choices.push_back(key);
    const auto it = std::find(choices.begin(), choices.end(), status.gyro_pad);
    const std::size_t at = it == choices.end() ? 0 : static_cast<std::size_t>(it - choices.begin());
    return choices[(at + (back ? choices.size() - 1 : 1)) % choices.size()];
}

} // namespace Jadyx
