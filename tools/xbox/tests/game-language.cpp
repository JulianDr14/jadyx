// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <map>
#include "jadyx/library/game_options_page.h"
#include "jadyx/settings/game_language.h"

int main() {
    using namespace Jadyx;
    using L = Settings::Language;
    std::map<std::wstring, std::int32_t> saved;
    const auto read = [&](const std::wstring& key) -> std::optional<std::int32_t> {
        const auto found = saved.find(key);
        return found == saved.end() ? std::nullopt : std::optional{found->second};
    };
    const auto write = [&](const std::wstring& key, std::optional<std::int32_t> value) {
        if (value)
            saved[key] = *value;
        else
            saved.erase(key);
        return true;
    };
    constexpr std::uint64_t first = 0x0100000000001000ULL;
    constexpr std::uint64_t second = first + 0x1000;
    assert(!ReadGameLanguage(first, read)); // Legacy settings have no key.
    assert(PreferredGameLanguage({}, L::Spanish) == L::Spanish);
    for (const auto language : SelectableGameLanguages()) {
        assert(WriteGameLanguage(first, language, write));
        assert(ReadGameLanguage(first, read) == language); // Reopen/persist.
        assert(!ReadGameLanguage(second, read));
        assert(PreferredGameLanguage(ReadGameLanguage(first, read), L::Spanish) == language);
    }
    assert(WriteGameLanguage(first, {},
                             write)); // Reset removes just this title's key.
    assert(saved.empty());
    saved[GameLanguageOverrideKey(first)] = 999;
    assert(!ReadGameLanguage(first, read));
    saved[GameLanguageOverrideKey(first)] = -1;
    assert(!ReadGameLanguage(first, read));
    assert(!WriteGameLanguage(first, static_cast<L>(999), write));
    assert(!WriteGameLanguage(0, L::French,
                              write)); // Unknown/homebrew title is never shared.
    assert(!ReadGameLanguage(0, read));
    assert(!WriteGameLanguage(first, L::French, [](const auto&, auto) { return false; }));
    assert(GameLanguageOverrideKey(first) == L"game_language_0100000000001000");
    assert(GameLanguageOverrideKey(first) != GameLanguageOverrideKey(second));
    auto mode = std::optional<L>{};
    for (std::size_t i = 0; i < SelectableGameLanguages().size() + 1; ++i)
        mode = StepGameLanguage(mode, false);
    assert(!mode);
    assert(StepGameLanguage({}, true) == SelectableGameLanguages().back());
    const auto preferred = PreferredGameLanguage(L::SpanishLatin, L::Japanese);
    assert(PickGameLanguage(preferred, [](L language) { return language == L::Spanish; }) ==
           L::Spanish);
    assert(PickGameLanguage(preferred, [](L language) { return language == L::EnglishAmerican; }) ==
           L::EnglishAmerican);
    assert(PickGameLanguage(preferred, [](L) { return false; }) == preferred);
    // Inherit is resolved each time, so later global changes propagate without rewriting a title.
    assert(PreferredGameLanguage(ReadGameLanguage(second, read), L::German) == L::German);
    assert(PreferredGameLanguage(ReadGameLanguage(second, read), L::French) == L::French);
    assert(WriteGameLanguage(second, L::Korean, write));
    assert(WriteGameLanguage(first, {}, write));
    assert(ReadGameLanguage(second, read) == L::Korean);
    for (const auto requested : SelectableGameLanguages()) {
        for (const auto available : SelectableGameLanguages()) {
            assert(PickGameLanguage(PreferredGameLanguage(requested, L::Japanese), [&](L language) {
                       return language == available;
                   }) == available);
        }
    }
    GameOptionsPage page;
    page.open = true;
    for (std::size_t i = 0; i < GameOptions().size(); ++i) {
        if (GameOptions()[i] != GameOption::Language)
            continue;
        SelectGameOption(page, i);
        assert(page.page == GameOption::Language && page.activate);
        HandleGameOptionsInput(page, GameOptionsInput::Confirm);
        assert(!page.discard && !page.confirm_discard);
        HandleGameOptionsInput(page, GameOptionsInput::Back);
        assert(!page.open);
    }

    for (const auto& ui_language : I18n::UiLanguages()) {
        I18n::SetUiLanguage(ui_language);
        assert(!I18n::Tr(I18n::Text::TitleLanguageHint).empty());
        assert(!I18n::Tr(I18n::Text::TitleLanguageReset).empty());
        const auto inherited = I18n::Format(I18n::Text::TitleLanguageInherit, {L"Deutsch"});
        assert(inherited.find(L"Deutsch") != std::wstring::npos);
        assert(inherited.find(L"{0}") == std::wstring::npos);
    }
}
