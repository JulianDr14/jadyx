// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "jadyx/mods/mod_language_filter.h"
#include "jadyx/mods/mods_page.h"

namespace {
std::size_t checks{};

void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
struct Card {
    int id{};
    std::string name;
    std::string category;
    bool translation{};
};
} // namespace

int main() {
    using namespace Jadyx::Mods;
    using F = ModLanguageFilter;
    const std::vector<Card> cards{
        {.id = 1, .name = "Sharper shadows", .category = "Graphics"},
        {.id = 2, .name = "Translation patch", .category = "Translations"},
        {.id = 3, .name = "ENGLISH dub", .category = "Audio"},
        {.id = 4, .name = "Legendas PT-BR", .category = "Interface"},
        {.id = 5, .name = "Traducción español", .category = "Text"},
        {.id = 6, .name = "Translation English + Português", .category = "Text"},
        {.id = 7, .name = "Unnamed patch", .translation = true},
        {.id = 8, .name = "Double jump", .category = "Gameplay"},
        {.id = 9, .name = "France costume", .category = "Skins"},
    };
    const auto ids = [](const auto& list) {
        std::vector<int> result;
        for (const auto& card : list)
            result.push_back(card.id);
        return result;
    };
    Check(ids(FilterModCards(cards, F::AllMods)) == ids(cards),
          "default preserves order and every card");
    Check(ids(FilterModCards(cards, F::Translations)) == std::vector<int>({2, 3, 4, 5, 6, 7}),
          "translations and dubs only, CNX flag, no substring false positives");
    Check(ids(FilterModCards(cards, F::Portuguese)) == std::vector<int>({2, 4, 6, 7}),
          "Portuguese, multilingual and unknown language fallback");
    Check(ids(FilterModCards(cards, F::English)) == std::vector<int>({2, 3, 6, 7}), "English");
    Check(ids(FilterModCards(cards, F::Spanish)) == std::vector<int>({2, 5, 7}), "Spanish");
    Check(FilterModCards(std::vector<Card>{}, F::English).empty(), "empty page");
    Check(ids(FilterModCards(cards, F::AllMods)) == ids(cards), "reset keeps backing cards intact");
    Check(MatchesModLanguage(F::French, "Traduction FRANÇAIS", "", false),
          "French accents and case");
    Check(!MatchesModLanguage(F::English, "Traduction français", "", false),
          "known other language");
    Check(MatchesModLanguage(F::German, "[DE] voice pack", "", false), "language tags");
    Check(MatchesModLanguage(F::Italian, "Traduzione italiano", "", false), "Italian");
    Check(MatchesModLanguage(F::Japanese, "日本語 translation", "", false), "Japanese");
    Check(MatchesModLanguage(F::Korean, "한국어 dub", "", false), "Korean");
    Check(MatchesModLanguage(F::Chinese, "中文 subtitles", "", false), "Chinese");
    Check(MatchesModLanguage(F::English, "", "", true), "missing CNX metadata remains visible");
    Check(!MatchesModLanguage(F::English, "Portuguese translation", "", true),
          "CNX explicit language wins");
    Check(MatchesModLanguage(F::Translations, "", "Voice packs", false), "category evidence");
    Check(MatchesModLanguage(F::AllMods, "", "", false), "legacy missing metadata unchanged");
    Check(MatchesModLanguage(static_cast<F>(255), "shadows", "Graphics", false),
          "invalid filter is safe");
    for (std::size_t i = 0; i < ModLanguageFilterCount; ++i) {
        const auto filter = static_cast<F>(i);
        Check(static_cast<std::size_t>(NextModLanguageFilter(filter)) ==
                  (i + 1) % ModLanguageFilterCount,
              "cycle through all choices and reset");
    }
    ModsPage page;
    page.tab = ModsTab::Explore;
    page.screen = ModsScreen::StoreMenu;
    ModsCounts counts;
    const auto actions = StoreActions(counts.store);
    Check(actions.back() == StoreAction::Language, "filter reachable in existing menu");
    page.menu = actions.size() - 1;
    page.card = 12;
    page.card_first = 8;
    Check(HandleModsInput(page, counts, ModsInput::Confirm).kind == ModsCommand::Kind::Language &&
              page.card == 0 && page.card_first == 0 && page.screen == ModsScreen::StoreMenu,
          "filter cycles in place and resets grid");
    page.screen = ModsScreen::List;
    counts.more_cards = true;
    Check(HandleModsInput(page, counts, ModsInput::Confirm).kind == ModsCommand::Kind::Want,
          "empty filtered page can request next page");
    counts.more_cards = false;
    Check(HandleModsInput(page, counts, ModsInput::Confirm).kind == ModsCommand::Kind::None,
          "complete empty list does not request");
    const auto cnx_actions = StoreActions({.gamebanana = false});
    Check(cnx_actions.back() == StoreAction::Language, "CNX offers the same filter");
    page.screen = ModsScreen::StoreMenu;
    page.menu = cnx_actions.size() - 1;
    counts.store.gamebanana = false;
    Check(HandleModsInput(page, counts, ModsInput::Confirm).kind == ModsCommand::Kind::Language,
          "CNX language command");
    Check(HandleModsInput(page, counts, ModsInput::Back).kind == ModsCommand::Kind::Moved &&
              page.screen == ModsScreen::List,
          "B closes options without another filter command");
    counts.store.gamebanana = true;
    counts.store.more = true;
    counts.cards = 1;
    page.screen = ModsScreen::StoreMenu;
    const auto paged_actions = StoreActions(counts.store);
    page.menu = paged_actions.size() - 2;
    Check(paged_actions[page.menu] == StoreAction::More,
          "next page reachable even with visible matches");
    const auto next_page = HandleModsInput(page, counts, ModsInput::Confirm);
    Check(next_page.kind == ModsCommand::Kind::Want && next_page.index == counts.cards &&
              page.screen == ModsScreen::List,
          "explicit paging works from any cursor position");
    counts.store.more = false;
    Check(StoreActions(counts.store).size() + 1 == paged_actions.size(),
          "complete or loading catalogs omit next-page action");
    const std::vector<Card> other_languages{
        {.id = 20, .name = "Translation Français"}, {.id = 21, .name = "Translation Deutsch"},
        {.id = 22, .name = "Translation Italiano"}, {.id = 23, .name = "Translation 日本語"},
        {.id = 24, .name = "Translation 한국어"},   {.id = 25, .name = "Translation 中文"},
    };
    for (std::size_t i = 0; i < other_languages.size(); ++i) {
        const auto filtered = FilterModCards(other_languages, static_cast<F>(i + 5));
        Check(filtered.size() == 1 && filtered.front().id == static_cast<int>(i) + 20,
              "each known language excludes all other known languages");
    }
    auto loaded = std::vector<Card>{cards[0], cards[7]};
    Check(FilterModCards(loaded, F::English).empty(), "first page may have no translations");
    loaded.push_back(cards[2]);
    Check(ids(FilterModCards(loaded, F::English)) == std::vector<int>{3},
          "next page retains catalog ID for details");
    Check(ids(FilterModCards(loaded, F::AllMods)) == std::vector<int>({1, 8, 3}),
          "reset restores every loaded page in source order");
    Check(MatchesModLanguage(F::Portuguese, "Tradução PORTUGUÊS", "", false),
          "Latin uppercase UTF-8");
    Check(MatchesModLanguage(F::English, "Russian translation", "", false),
          "unrecognized language remains discoverable");
    Check(!MatchesModLanguage(F::Translations, "debug localizationfix", "", false),
          "embedded keywords are not declarations");
    Check(ModsPage{}.tab == ModsTab::Installed, "opening mods retains the installed tab");
    std::printf("mod-language-filter PASS (%zu checks)\n", checks);
}
