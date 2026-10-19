// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The game language a title gets from its NACP, shared by the boot (the language the game runs
// in) and the library (the language its title is read in), so both always agree.

#include "common/common_types.h"
#include "core/file_sys/control_metadata.h"
#include "jadyx/settings/system_settings.h"

namespace Jadyx {

/// The NACP's supported-language bit for a system language (as NACP::GetLanguageEntry maps them).
inline u32 NacpLanguageBit(Settings::Language language) {
    using FileSys::Language;
    const auto bit = [](Language nacp) { return u32{1} << static_cast<u32>(nacp); };
    switch (language) {
    case Settings::Language::Japanese:            return bit(Language::Japanese);
    case Settings::Language::EnglishAmerican:     return bit(Language::AmericanEnglish);
    case Settings::Language::EnglishBritish:      return bit(Language::BritishEnglish);
    case Settings::Language::French:              return bit(Language::French);
    case Settings::Language::FrenchCanadian:      return bit(Language::CanadianFrench);
    case Settings::Language::German:              return bit(Language::German);
    case Settings::Language::Italian:             return bit(Language::Italian);
    case Settings::Language::Spanish:             return bit(Language::Spanish);
    case Settings::Language::SpanishLatin:        return bit(Language::LatinAmericanSpanish);
    case Settings::Language::Dutch:               return bit(Language::Dutch);
    case Settings::Language::Portuguese:          return bit(Language::Portuguese);
    case Settings::Language::PortugueseBrazilian: return bit(Language::BrazilianPortuguese);
    case Settings::Language::Russian:             return bit(Language::Russian);
    case Settings::Language::Korean:              return bit(Language::Korean);
    case Settings::Language::Chinese:
    case Settings::Language::ChineseSimplified:   return bit(Language::SimplifiedChinese);
    case Settings::Language::Taiwanese:
    case Settings::Language::ChineseTraditional:  return bit(Language::TraditionalChinese);
    case Settings::Language::Polish:              return bit(Language::Polish);
    case Settings::Language::Thai:                return bit(Language::Thai);
    }
    return 0;
}

/// The closest language to `preferred` that the game declares (PickGameLanguage).
inline Settings::Language PickNacpLanguage(Settings::Language preferred, const FileSys::NACP& nacp) {
    const u32 supported = nacp.GetSupportedLanguages();
    return PickGameLanguage(preferred, [supported](Settings::Language language) {
        return (supported & NacpLanguageBit(language)) != 0;
    });
}

} // namespace Jadyx
