// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string_view>
#include <cstddef>

namespace Jadyx {
inline constexpr std::wstring_view KeyboardControllerId = L"keyboard";
// Shared pure policy: automatic picks a compatible pad; explicit selection
// never silently redirects player input when the chosen device disconnects.
template <typename Devices>
std::optional<size_t> SelectControllerDevice(const Devices& devices, std::wstring_view preferred) {
    if (preferred == KeyboardControllerId) return {};
    for (size_t i = 0; i < devices.size(); ++i) {
        if (static_cast<bool>(devices[i]) && (preferred.empty() || devices[i].id == preferred)) return i;
    }
    return {};
}
/// A name in Windows' pseudo-locale, as the console can report a pad's: plain letters mixed
/// with accented look-alikes from the extended Latin, Greek and Cyrillic blocks. A name written
/// in one of those scripts alone is a real one.
inline bool PseudoLocalized(std::wstring_view name) {
    bool plain = false, extended = false;
    for (const wchar_t c : name) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) plain = true;
        else if (c >= 0x0100 && c <= 0x052F) extended = true;
    }
    return plain && extended;
}
}
