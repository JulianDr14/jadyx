// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jadyx/settings/system_settings.h"

namespace Jadyx {

// Separate LocalSettings keys preserve existing global settings and library files.
inline std::wstring GameLanguageOverrideKey(std::uint64_t program_id) {
    constexpr std::wstring_view digits = L"0123456789ABCDEF";
    std::wstring key = L"game_language_";
    for (int shift = 60; shift >= 0; shift -= 4)
        key += digits[(program_id >> shift) & 15];
    return key;
}

inline std::optional<Settings::Language> DecodeGameLanguage(std::int32_t raw) {
    for (const auto language : SelectableGameLanguages())
        if (static_cast<std::int32_t>(language) == raw)
            return language;
    return std::nullopt;
}

// The broker supplies typed reads/writes; reset removes the key rather than pinning the global
// language. Invalid data inherits too. Title zero is not a persistent identity.
template <typename Read>
std::optional<Settings::Language> ReadGameLanguage(std::uint64_t program_id, Read&& read) {
    if (!program_id)
        return std::nullopt;
    const auto raw = read(GameLanguageOverrideKey(program_id));
    return raw ? DecodeGameLanguage(*raw) : std::nullopt;
}

template <typename Write>
bool WriteGameLanguage(std::uint64_t program_id, std::optional<Settings::Language> language,
                       Write&& write) {
    if (!program_id || (language && !DecodeGameLanguage(static_cast<std::int32_t>(*language))))
        return false;
    return write(GameLanguageOverrideKey(program_id),
                 language ? std::optional{static_cast<std::int32_t>(*language)} : std::nullopt);
}

inline Settings::Language PreferredGameLanguage(std::optional<Settings::Language> language,
                                                Settings::Language global) {
    return language.value_or(global);
}

inline std::optional<Settings::Language> StepGameLanguage(std::optional<Settings::Language> current,
                                                          bool back) {
    const auto choices = SelectableGameLanguages();
    std::size_t index = 0;
    for (std::size_t i = 0; i < choices.size(); ++i)
        if (current == choices[i])
            index = i + 1;
    index =
        back ? (index + choices.size()) % (choices.size() + 1) : (index + 1) % (choices.size() + 1);
    return index ? std::optional{choices[index - 1]} : std::nullopt;
}
} // namespace Jadyx
