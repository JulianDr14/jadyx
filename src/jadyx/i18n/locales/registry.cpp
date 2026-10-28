// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/i18n/locales/catalogs.h"

namespace Jadyx::I18n {
namespace {
// CLDR: English and Spanish use "one" for exactly 1 and "other" for everything else.
PluralForm OneOrOther(std::uint64_t count) {
    return count == 1 ? PluralForm::One : PluralForm::Other;
}

// CLDR: Brazilian Portuguese uses "one" for 0 and 1 ("0 jogo", "1 jogo", "2 jogos").
PluralForm ZeroOrOneIsOne(std::uint64_t count) {
    return count <= 1 ? PluralForm::One : PluralForm::Other;
}

// English first: it is the fallback for any language and any missing text.
const std::array<UiLanguage, 4> languages{{
    {L"en", L"English", &EnglishCatalog, &OneOrOther},
    {L"es", L"Español", &SpanishCatalog, &OneOrOther},
    {L"pt-BR", L"Português (Brasil)", &PortugueseBrazilCatalog, &ZeroOrOneIsOne},
    {L"zh-CN", L"简体中文", &SimplifiedChineseCatalog,
     [](std::uint64_t) { return PluralForm::Other; }},
}};
} // namespace

std::span<const UiLanguage> UiLanguages() {
    return languages;
}
} // namespace Jadyx::I18n
