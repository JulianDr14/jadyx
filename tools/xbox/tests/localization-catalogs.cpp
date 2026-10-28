// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/i18n/localization.cpp and src/jadyx/i18n/locales/*.cpp.
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "jadyx/i18n/localization.h"

int main() {
    using namespace Jadyx::I18n;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    const auto languages = UiLanguages();
    check(languages.size() >= 4 && languages.front().tag == L"en" &&
          FindUiLanguage(L"zh-CN") && FindUiLanguage(L"zh_CN"));

    // Negotiation: exact, shortened, case/underscore-insensitive, in preference order, English last.
    auto negotiate = [&](std::vector<std::wstring> preferences) {
        return NegotiateUiLanguage(languages, preferences).tag;
    };
    check(negotiate({L"es-CO"}) == L"es");
    check(negotiate({L"es-419"}) == L"es");
    check(negotiate({L"ES_mx"}) == L"es");
    check(negotiate({L"fr-FR", L"es-ES", L"en-US"}) == L"es");
    check(negotiate({L"pt-BR"}) == L"pt-BR");
    check(negotiate({L"pt"}) == L"pt-BR");
    check(negotiate({L"pt-PT", L"es-ES"}) == L"pt-BR"); // Another variant beats the next preference.
    check(negotiate({L"zh-SG"}) == L"zh-CN");
    check(negotiate({L"de-DE"}) == L"en");
    check(negotiate({}) == L"en");

    // Every catalog is complete (MakeCatalog enforces it at compile time; this guards the registry).
    for (const auto& language : languages)
        for (std::size_t i = 0; i < TextCount; ++i)
            if (language.tag != L"zh-CN") check(!(*language.catalog)[i].empty());

    // Every translation uses the same "{n}" placeholders as English, or Format leaves a hole.
    auto placeholders = [](std::wstring_view text) {
        std::bitset<10> found;
        for (std::size_t i = 0; i + 2 < text.size(); ++i)
            if (text[i] == L'{' && text[i + 1] >= L'0' && text[i + 1] <= L'9' && text[i + 2] == L'}')
                found.set(text[i + 1] - L'0');
        return found;
    };
    const auto& english = *languages.front().catalog;
    for (const auto& language : languages)
        for (std::size_t i = 0; i < TextCount; ++i) {
            if ((*language.catalog)[i].empty()) continue;
            if (placeholders((*language.catalog)[i]) == placeholders(english[i])) continue;
            std::fprintf(stderr, "placeholder mismatch: %ls text %zu\n", language.tag.data(), i);
            std::abort();
        }

    SetUiLanguage(*FindUiLanguage(L"es"));
    check(Tr(Text::Play) == L"Jugar");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 1) == L"1 juego");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 0) == L"0 juegos");
    check(Format(Text::Importing, {L"3", L"9"}) == L"Importando 3 / 9 archivos...");
    // Brazilian Portuguese: CLDR puts 0 with 1.
    SetUiLanguage(*FindUiLanguage(L"pt-BR"));
    check(Tr(Text::Play) == L"Jogar");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 0) == L"0 jogo");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 1) == L"1 jogo");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 2) == L"2 jogos");

    // A language with a "few" form uses its extra text, and Other where it has none.
    static constexpr PluralEntry few[]{{Text::GamesCountOther, PluralForm::Few, L"{0} gry"}};
    const UiLanguage polish_like{L"pl", L"Polski", languages.front().catalog,
        [](std::uint64_t n) { return n == 1 ? PluralForm::One : n % 10 >= 2 && n % 10 <= 4 ? PluralForm::Few : PluralForm::Other; },
        few};
    SetUiLanguage(polish_like);
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 3) == L"3 gry");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 5) == L"5 games");
    check(Plural(Text::TitlesCountOne, Text::TitlesCountOther, 3) == L"3 titles");

    SetUiLanguage(*FindUiLanguage(L"en"));
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 2) == L"2 games");
    SetUiLanguage(*FindUiLanguage(L"zh-CN"));
    check(Tr(Text::Play) == L"开始游戏");
    check(Tr(Text::GamesCountOther) == L"{0} 款游戏");
    check(Tr(Text::CheatsHint) == L"Turn them on before playing, or from the in-game menu while you play");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 3) == L"3 款游戏");
    SetUiLanguage(*FindUiLanguage(L"en"));
    check(ToUtf8(L"Ñ€😀") == "\xC3\x91\xE2\x82\xAC\xF0\x9F\x98\x80");

    std::puts("localization: negotiation, catalogs, plurals, placeholders and UTF-8 PASS");
}
