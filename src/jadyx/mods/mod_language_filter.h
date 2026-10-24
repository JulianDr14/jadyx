// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Inspired by NXbox's translation/dub catalog filter (GPL-3.0-or-later,
// Copyright 2026 NSPX). Only already available name/category metadata is inspected.
// Unknown languages stay visible; this is a discovery hint, never compatibility evidence.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Jadyx::Mods {

enum class ModLanguageFilter : std::uint8_t {
    AllMods,
    Translations,
    Portuguese,
    English,
    Spanish,
    French,
    German,
    Italian,
    Japanese,
    Korean,
    Chinese,
};
inline constexpr std::size_t ModLanguageFilterCount = 11;

inline ModLanguageFilter NextModLanguageFilter(ModLanguageFilter filter) {
    return static_cast<ModLanguageFilter>((static_cast<std::size_t>(filter) + 1) %
                                          ModLanguageFilterCount);
}

namespace ModLanguageDetail {
inline std::string Lower(std::string_view text) {
    std::string result{text};
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto& letter = result[i];
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
        // UTF-8 Latin-1 capitals used by the language names (including Ç, Ñ, Ã and Ê).
        if (static_cast<unsigned char>(letter) == 0xc3 && i + 1 < result.size()) {
            const auto next = static_cast<unsigned char>(result[i + 1]);
            if (next >= 0x80 && next <= 0x9e && next != 0x97)
                result[i + 1] = static_cast<char>(next + 0x20);
            ++i;
        }
    }
    return result;
}

inline bool WordByte(char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') ||
           static_cast<unsigned char>(letter) >= 0x80;
}

inline bool HasWord(std::string_view text, std::string_view word) {
    for (auto pos = text.find(word); pos != std::string_view::npos;
         pos = text.find(word, pos + 1)) {
        const auto end = pos + word.size();
        if ((pos == 0 || !WordByte(text[pos - 1])) && (end == text.size() || !WordByte(text[end])))
            return true;
    }
    return false;
}

inline bool HasAny(std::string_view text, std::string_view words) {
    while (!words.empty()) {
        const auto split = words.find('|');
        if (HasWord(text, words.substr(0, split)))
            return true;
        if (split == std::string_view::npos)
            break;
        words.remove_prefix(split + 1);
    }
    return false;
}

inline constexpr std::array<std::string_view, 9> Languages{
    "portuguese|português|portugues|portuguesa|brazilian|brasileiro|brasileira|pt-br|pt_br|ptbr|pt "
    "br|[pt]|(pt)",
    "english|[en]|(en)|[eng]|(eng)",
    "spanish|español|espanol|española|castellano|[es]|(es)",
    "french|français|francais|française|[fr]|(fr)",
    "german|deutsch|[de]|(de)",
    "italian|italiano|italiana|[it]|(it)",
    "japanese|nihongo|日本語|[ja]|(ja)|[jp]|(jp)",
    "korean|한국어|[ko]|(ko)",
    "chinese|mandarin|中文|[zh]|(zh)",
};
inline constexpr std::string_view TranslationWords =
    "translation|translations|translated|tradução|traduções|traducao|traducoes|traducción|"
    "traducciones|"
    "traduccion|traduction|traduzione|localization|localisation|subtitles|subtitle|subtitulos|"
    "subtítulos|"
    "legenda|legendas|language|idioma|língua|lingua|voice|voices|voz|vozes|dub|dubs|dubbed|dubbing|"
    "dublagem";
} // namespace ModLanguageDetail

inline bool MatchesModLanguage(ModLanguageFilter filter, std::string_view name,
                               std::string_view category, bool translation) {
    using namespace ModLanguageDetail;
    const auto choice = static_cast<std::size_t>(filter);
    if (filter == ModLanguageFilter::AllMods || choice >= ModLanguageFilterCount)
        return true;
    // Authors, URLs, descriptions and archive contents are not language declarations.
    const auto text = Lower(std::string{name} + " " + std::string{category});
    bool known_language = false;
    bool selected_language = false;
    for (std::size_t i = 0; i < Languages.size(); ++i) {
        if (!HasAny(text, Languages[i]))
            continue;
        known_language = true;
        if (choice == i + 2)
            selected_language = true;
    }
    if (!translation && !known_language && !HasAny(text, TranslationWords))
        return false;
    return filter == ModLanguageFilter::Translations || selected_language || !known_language;
}

/// Retain the backing order and IDs, so details/files still refer to the same catalog entry.
/// The default copies verbatim, including records with no language metadata.
template <typename Card>
std::vector<Card> FilterModCards(const std::vector<Card>& cards, ModLanguageFilter filter) {
    std::vector<Card> visible;
    visible.reserve(cards.size());
    for (const auto& card : cards)
        if (MatchesModLanguage(filter, card.name, card.category, card.translation))
            visible.push_back(card);
    return visible;
}

} // namespace Jadyx::Mods
