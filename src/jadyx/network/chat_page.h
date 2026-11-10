// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The room chat's page as the pad drives it, free of drawing and of the room so it is tested on
// its own: the history scrolls with up/down (the newest line at the bottom), A writes a message,
// Y opens the quick phrases (picked with up/down, sent with A), B goes back.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "jadyx/ui/ui_actions.h"

namespace Jadyx::Online {

/// The quick phrases, in the order the picker lists them (their texts are localized).
enum class QuickPhrase : std::uint8_t { GoodGame, Again, Ready, OnMyWay, Wait, Leaving, Laugh, WhoHosts };
inline constexpr std::array QuickPhrases{QuickPhrase::GoodGame, QuickPhrase::Again,  QuickPhrase::Ready,
                                         QuickPhrase::OnMyWay,  QuickPhrase::Wait,   QuickPhrase::Leaving,
                                         QuickPhrase::Laugh,    QuickPhrase::WhoHosts};

struct ChatPage {
    enum class Effect : std::uint8_t {
        None,    ///< Nothing changed.
        Redraw,  ///< The page changed (scrolled, picker moved).
        Compose, ///< Open the keyboard for a message.
        Send,    ///< Send `phrase`.
        Back,    ///< Leave the page.
    };

    /// How many lines the history is scrolled up from the newest; 0 follows new messages.
    std::size_t scroll{};
    bool picking{};
    std::size_t phrase{};

    /// `lines`: how many the history holds.
    Effect Handle(MenuAction action, std::size_t lines) {
        if (picking) {
            switch (action) {
            case MenuAction::Up:
                phrase = (phrase + QuickPhrases.size() - 1) % QuickPhrases.size();
                return Effect::Redraw;
            case MenuAction::Down:
                phrase = (phrase + 1) % QuickPhrases.size();
                return Effect::Redraw;
            case MenuAction::Confirm:
                picking = false;
                scroll = 0;
                return Effect::Send;
            case MenuAction::Back:
            case MenuAction::Refresh:
                picking = false;
                return Effect::Redraw;
            default:
                return Effect::None;
            }
        }
        switch (action) {
        case MenuAction::Up:
            if (scroll + 1 >= lines) return Effect::None;
            ++scroll;
            return Effect::Redraw;
        case MenuAction::Down:
            if (scroll == 0) return Effect::None;
            --scroll;
            return Effect::Redraw;
        case MenuAction::Confirm:
            return Effect::Compose;
        case MenuAction::Refresh:
            picking = true;
            return Effect::Redraw;
        case MenuAction::Back:
            return Effect::Back;
        default:
            return Effect::None;
        }
    }

    /// The history grew by `added` lines: a reader scrolled up keeps the same lines in view.
    void Grew(std::size_t added, std::size_t lines) {
        if (scroll != 0) scroll += added;
        if (lines == 0) scroll = 0;
        else if (scroll >= lines) scroll = lines - 1;
    }
};

[[nodiscard]] constexpr QuickPhrase PhraseAt(std::size_t index) {
    return QuickPhrases[index % QuickPhrases.size()];
}

} // namespace Jadyx::Online

namespace Jadyx {

/// One line of the room chat as drawn: a bubble (the player's own on the right) or a status line
/// (someone joined, left...), already in words.
struct ChatMenuLine {
    enum class Kind : std::uint8_t { Message, Own, Status };
    Kind kind{};
    std::wstring who;
    std::wstring text;
};

} // namespace Jadyx
