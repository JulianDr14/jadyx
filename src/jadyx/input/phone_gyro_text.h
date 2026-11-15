// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The phone gyro's link in words, the same in the in-game menu and in Settings > Controller.

#pragma once

#include <string>

#include "jadyx/i18n/localization.h"
#include "jadyx/input/phone_gyro.h"

namespace Jadyx {

/// The link in a word or two.
[[nodiscard]] inline std::wstring PhoneStateText(const PhoneGyroStatus& phone) {
    using I18n::Text;
    const auto tr = [](Text id) { return std::wstring{I18n::Tr(id)}; };
    switch (phone.state) {
    case PhoneGyroState::Off:
    case PhoneGyroState::Searching: return tr(Text::PhoneSearching);
    case PhoneGyroState::NotFound: return tr(Text::PhoneNotFound);
    case PhoneGyroState::Connecting: return tr(Text::PhoneConnecting);
    case PhoneGyroState::Connected: return tr(Text::PhoneConnected);
    case PhoneGyroState::Lost: return tr(Text::PhoneLost);
    }
    return {};
}

/// What to do now: open the app on the console's network, or which phone it is and how fast.
[[nodiscard]] inline std::wstring PhoneHintText(const PhoneGyroStatus& phone) {
    using I18n::Text;
    if (phone.state == PhoneGyroState::Connected)
        return I18n::Format(Text::PhoneConnectedHint, {phone.address, std::to_wstring(phone.rate_hz)});
    if (phone.state == PhoneGyroState::Lost) return I18n::Format(Text::PhoneLostHint, {phone.address});
    return I18n::Format(Text::PhoneSearchHint, {phone.console_address.empty() ? L"?" : phone.console_address});
}

/// The address row's value: the typed one, or automatic (with the phone found).
[[nodiscard]] inline std::wstring PhoneAddressText(const PhoneGyroStatus& phone) {
    if (phone.typed && !phone.address.empty()) return phone.address;
    std::wstring value{I18n::Tr(I18n::Text::PhoneAddressAuto)};
    if (phone.state == PhoneGyroState::Connected) value += L" · " + phone.address;
    return value;
}

/// Under the address row: why it was refused, or what A does.
[[nodiscard]] inline I18n::Text PhoneAddressHint(const PhoneGyroStatus& phone, bool invalid) {
    using I18n::Text;
    if (invalid) return Text::PhoneAddressInvalid;
    return phone.typed && !phone.address.empty() ? Text::PhoneAddressTypedHint : Text::PhoneAddressHint;
}

} // namespace Jadyx
