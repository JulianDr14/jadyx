// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/i18n/localization.cpp and src/jadyx/i18n/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <set>
#include "jadyx/settings/system_page.h"

int main() {
    using namespace Jadyx;
    using L = Settings::Language;
    using R = Settings::Region;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    auto locale = [](std::wstring language, std::wstring region = {}) {
        return SystemLocale{{std::move(language)}, std::move(region)};
    };

    // Game language from Windows: the variant follows the region, from the tag or the home region.
    check(GameLanguageForLocale(locale(L"es-CO")) == L::SpanishLatin);
    check(GameLanguageForLocale(locale(L"es-419")) == L::SpanishLatin);
    check(GameLanguageForLocale(locale(L"es-ES")) == L::Spanish);
    check(GameLanguageForLocale(locale(L"es", L"ES")) == L::Spanish);
    check(GameLanguageForLocale(locale(L"es", L"MX")) == L::SpanishLatin);
    check(GameLanguageForLocale(locale(L"en-US")) == L::EnglishAmerican);
    check(GameLanguageForLocale(locale(L"en-GB")) == L::EnglishBritish);
    check(GameLanguageForLocale(locale(L"en", L"AU")) == L::EnglishBritish);
    check(GameLanguageForLocale(locale(L"fr-CA")) == L::FrenchCanadian);
    check(GameLanguageForLocale(locale(L"pt-BR")) == L::PortugueseBrazilian);
    check(GameLanguageForLocale(locale(L"pt-PT")) == L::Portuguese);
    check(GameLanguageForLocale(locale(L"zh-Hant-TW")) == L::ChineseTraditional);
    check(GameLanguageForLocale(locale(L"zh-CN")) == L::ChineseSimplified);
    check(GameLanguageForLocale(locale(L"ja-JP")) == L::Japanese);
    check(GameLanguageForLocale(locale(L"sv-SE")) == L::EnglishBritish);
    check(GameLanguageForLocale({}) == L::EnglishAmerican);
    // The first language in the list that a Switch has, not just the first one.
    check(GameLanguageForLocale(SystemLocale{{L"ca-ES", L"es-ES"}, {}}) == L::Spanish);
    check(GameLanguageForLocale(SystemLocale{{L"sv-SE", L"pt-BR", L"en-US"}, {}}) == L::PortugueseBrazilian);
    check(GameLanguageForLocale(SystemLocale{{L"sv-SE", L"fi-FI"}, {}}) == L::EnglishBritish);

    // Region: home region first, else the language's region; Europe for the rest of the world.
    check(RegionForLocale(locale(L"es-ES", L"CO")) == R::Usa);
    check(RegionForLocale(locale(L"es-ES")) == R::Europe);
    check(RegionForLocale(locale(L"ja-JP", L"JP")) == R::Japan);
    check(RegionForLocale(locale(L"en", L"NZ")) == R::Australia);
    check(RegionForLocale(locale(L"zh", L"HK")) == R::Taiwan);
    check(RegionForLocale(locale(L"de", L"DE")) == R::Europe);
    check(RegionForLocale({}) == R::Usa);

    // A game gets the closest language it declares.
    auto pick = [](L preferred, std::set<L> has) {
        return PickGameLanguage(preferred, [&](L language) { return has.contains(language); });
    };
    check(pick(L::SpanishLatin, {L::Spanish, L::EnglishAmerican}) == L::Spanish);
    check(pick(L::Spanish, {L::SpanishLatin, L::EnglishAmerican}) == L::SpanishLatin);
    check(pick(L::SpanishLatin, {L::French, L::EnglishBritish}) == L::EnglishBritish);
    check(pick(L::German, {L::Japanese}) == L::Japanese);
    check(pick(L::Korean, {}) == L::Korean);
    check(pick(L::French, {L::French, L::EnglishAmerican}) == L::French);

    // Choices step through "follow the system" and wrap both ways.
    const auto regions = SelectableRegions();
    check(StepChoice<R>(std::nullopt, regions, false) == regions.front());
    check(StepChoice<R>(std::nullopt, regions, true) == regions.back());
    check(!StepChoice<R>(regions.back(), regions, false));
    std::set<L> unique(SelectableGameLanguages().begin(), SelectableGameLanguages().end());
    check(unique.size() == SelectableGameLanguages().size());

    // User names: trimmed, control characters dropped, ten units, never a split surrogate pair.
    check(SanitizeUserName(L"  Ana  ") == L"Ana");
    check(SanitizeUserName(L"\tJulián\n") == L"Julián");
    check(SanitizeUserName(L"ABCDEFGHIJKLMN") == L"ABCDEFGHIJ");
#if WCHAR_MAX <= 0xFFFF
    check(SanitizeUserName(L"ABCDEFGHI😀") == L"ABCDEFGHI");
#else
    check(SanitizeUserName(L"ABCDEFGHI😀") == L"ABCDEFGHI😀");
#endif
    check(SanitizeUserName(L"   ").empty());

    // The page: values, changes and the name editor.
    I18n::SetUiLanguage(*I18n::FindUiLanguage(L"es"));
    SystemPage page;
    page.open = true;
    page.locale = locale(L"es-CO", L"CO");
    check(SystemRowValue(page, SystemPage::UserName) == L"Jadyx");
    check(SystemRowValue(page, SystemPage::InterfaceLanguage) == L"简体中文");
    check(SystemRowValue(page, SystemPage::GameLanguage) == L"Automático · Español (Latinoamérica)");
    check(SystemRowValue(page, SystemPage::Region) == L"Automático · América");
    page.selected = SystemPage::InterfaceLanguage;
    check(HandleSystemInput(page, SystemInput::Right) == SystemPageEffect::Changed);
    check(page.settings.ui_language.empty());
    check(HandleSystemInput(page, SystemInput::Left) == SystemPageEffect::Changed);
    check(page.settings.ui_language == L"zh-CN");
    page.selected = SystemPage::Region;
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::Changed && page.settings.region);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::AdultMods);
    check(SystemRowValue(page, SystemPage::AdultMods) == L"Ocultos");
    check(HandleSystemInput(page, SystemInput::Right) == SystemPageEffect::Changed && page.settings.show_adult_mods);
    check(SystemRowValue(page, SystemPage::AdultMods) == L"Se muestran");
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::PerformanceStats);
    // Pulse sessions are on unless the player turns them off.
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::PulseSessions && page.settings.pulse_sessions);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::Changed &&
          !page.settings.pulse_sessions);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::Changed &&
          page.settings.pulse_sessions);
    // Play time is on unless the player turns it off.
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::PlayTime && page.settings.track_play_time);
    check(SystemRowValue(page, SystemPage::PlayTime) == L"Sí");
    check(HandleSystemInput(page, SystemInput::Left) == SystemPageEffect::Changed &&
          !page.settings.track_play_time && SystemRowValue(page, SystemPage::PlayTime) == L"No");
    check(HandleSystemInput(page, SystemInput::Right) == SystemPageEffect::Changed &&
          page.settings.track_play_time);
    // Extra controllers play unless the player turns them off.
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::MultipleControllers && page.settings.multiple_controllers);
    check(SystemRowValue(page, SystemPage::MultipleControllers) == L"Sí");
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::Changed &&
          !page.settings.multiple_controllers &&
          SystemRowValue(page, SystemPage::MultipleControllers) == L"No");
    check(HandleSystemInput(page, SystemInput::Right) == SystemPageEffect::Changed &&
          page.settings.multiple_controllers);
    // Games stay offline unless the player turns it off.
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::OfflineMode && page.settings.offline_mode);
    check(SystemRowValue(page, SystemPage::OfflineMode) == L"Sí");
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::Changed &&
          !page.settings.offline_mode && SystemRowValue(page, SystemPage::OfflineMode) == L"No");
    check(HandleSystemInput(page, SystemInput::Right) == SystemPageEffect::Changed &&
          page.settings.offline_mode);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::MiiEditor);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::OpenMiiEditor);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::WebBrowser);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::OpenWebBrowser);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::VideoPlayer);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::OpenVideoPlayer);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::ClearCache);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::ClearCache);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::Updates);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::OpenUpdates);
    check(HandleSystemInput(page, SystemInput::Down) == SystemPageEffect::None &&
          page.selected == SystemPage::UserName);
    check(HandleSystemInput(page, SystemInput::Left) == SystemPageEffect::None && !page.name_editor);
    check(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::EditName && page.name_editor);
    check(FinishNameEditor(page, true) == SystemPageEffect::None && page.name_editor &&
          !page.editor_notice.empty());
    for (const wchar_t c : std::wstring_view{L"Ana"}) page.name_editor->Type(c);
    check(FinishNameEditor(page, true) == SystemPageEffect::NameDone && !page.name_editor);
    check(page.settings.user_name == L"Ana" && SystemRowValue(page, SystemPage::UserName) == L"Ana");
    HandleSystemInput(page, SystemInput::Confirm);
    page.name_editor->Type(L'X');
    check(FinishNameEditor(page, false) == SystemPageEffect::None && page.settings.user_name == L"Ana");
    check(HandleSystemInput(page, SystemInput::Back) == SystemPageEffect::None && !page.open);

    // Resolution: explicit choices win, the rest follow the system.
    SystemSettings settings;
    check(settings.ui_language == L"zh-CN");
    check(ResolveSystem(settings, locale(L"es-ES")).ui_language->tag == L"zh-CN");
    settings.region = R::Japan;
    settings.ui_language.clear();
    const auto resolved = ResolveSystem(settings, locale(L"es-ES"));
    check(resolved.ui_language->tag == L"es" && resolved.game_language == L::Spanish &&
          resolved.region == R::Japan);
    settings.ui_language = L"en";
    check(ResolveSystem(settings, locale(L"es-ES")).ui_language->tag == L"en");

    std::puts("system settings: locale detection, regions, game language fallback, names and page PASS");
}
