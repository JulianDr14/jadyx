// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include "common/settings_enums.h"
#include "jadyx/i18n/localization.h"

namespace Jadyx {
inline std::span<const Settings::ResolutionSetup> SelectableResolutions() {
    using R = Settings::ResolutionSetup;
    static constexpr std::array choices{R::Res1_2X, R::Res3_4X, R::Res1X,
                                        R::Res3_2X, R::Res2X,   R::Res3X};
    return choices;
}
inline std::optional<Settings::ResolutionSetup> ResolutionFromValue(std::int32_t value) {
    for (const auto choice : SelectableResolutions())
        if (static_cast<std::int32_t>(choice) == value)
            return choice;
    return std::nullopt;
}
inline Settings::ResolutionSetup EffectiveResolution(Settings::ResolutionSetup requested,
                                                     bool enabled) {
    return enabled && ResolutionFromValue(static_cast<std::int32_t>(requested))
               ? requested
               : Settings::ResolutionSetup::Res1X;
}
inline Settings::ResolutionSetup StepResolution(Settings::ResolutionSetup current, bool back) {
    const auto choices = SelectableResolutions();
    const auto found = std::ranges::find(choices, current);
    const auto index =
        found == choices.end() ? std::size_t{2} : static_cast<std::size_t>(found - choices.begin());
    return choices[(index + (back ? choices.size() - 1 : 1)) % choices.size()];
}
// LocalSettings keys, scoped by application id. Zero is the default for all games.
inline std::wstring ResolutionKey(std::uint64_t application) {
    if (!application)
        return L"system_resolution";
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring key = L"game_resolution_";
    for (int shift = 60; shift >= 0; shift -= 4)
        key += digits[(application >> shift) & 15];
    return key;
}
// The same reader is used by the UWP store and the host tests. Invalid overrides inherit.
template <typename Read>
Settings::ResolutionSetup ReadResolution(std::uint64_t application, Read&& read) {
    for (const auto id : {application, std::uint64_t{0}}) {
        if (const auto raw = read(ResolutionKey(id)))
            if (const auto choice = ResolutionFromValue(*raw))
                return *choice;
    }
    return Settings::ResolutionSetup::Res1X;
}
template <typename Write>
bool StoreResolution(std::uint64_t application, Settings::ResolutionSetup choice, Write&& write) {
    const auto value = static_cast<std::int32_t>(choice);
    if (!ResolutionFromValue(value))
        return false;
    try {
        write(ResolutionKey(application), value);
        return true;
    } catch (...) {
        return false;
    }
}
inline I18n::Text ResolutionText(Settings::ResolutionSetup choice) {
    using R = Settings::ResolutionSetup;
    using I18n::Text;
    switch (choice) {
    case R::Res1_2X:
        return Text::ResolutionHalf;
    case R::Res3_4X:
        return Text::ResolutionThreeQuarters;
    case R::Res3_2X:
        return Text::ResolutionOneHalf;
    case R::Res2X:
        return Text::ResolutionDouble;
    case R::Res3X:
        return Text::ResolutionTriple;
    default:
        return Text::ResolutionNative;
    }
}
} // namespace Jadyx
