// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The on-screen keyboard as it is drawn: title, hint, the text box, a notice and the keys of a
// TextEntry, in a 980x610 panel. The library draws it over the System page and the in-game
// overlay over the game, both with UiPainter, so the keyboard looks and hit-tests the same
// everywhere. The host draws the panel's background and its footer (which buttons do what), since
// prompts differ between the library and a running game.

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "jadyx/ui/text_entry.h"

namespace Jadyx {

class UiPainter;

/// Panel geometry in layout units, relative to the panel's top-left corner.
struct TextEntryPanelLayout {
    static constexpr float Width = 980, Height = 610;
    static constexpr float KeyWidth = 82, KeyHeight = 54, Gap = 8;
    static constexpr float BoxWidth = TextEntry::Columns * KeyWidth + (TextEntry::Columns - 1) * Gap;
    static constexpr float Left = (Width - BoxWidth) / 2;
    static constexpr float Top = 248;
    /// Where the host draws its prompts.
    static constexpr float Footer = 562;
    static constexpr float KeyX(std::size_t column) { return Left + column * (KeyWidth + Gap); }
    static constexpr float KeyY(std::size_t row) { return Top + row * (KeyHeight + Gap); }

    /// The numeric pad: four big keys a row, centered under the text box.
    static constexpr float NumKeyWidth = 150, NumKeyHeight = 66;
    static constexpr float NumWidth = TextEntry::NumericColumns * NumKeyWidth + (TextEntry::NumericColumns - 1) * Gap;
    static constexpr float NumLeft = (Width - NumWidth) / 2;
    static constexpr float NumKeyX(std::size_t column) { return NumLeft + column * (NumKeyWidth + Gap); }
    static constexpr float NumKeyY(std::size_t row) { return Top + row * (NumKeyHeight + Gap); }

    /// The key (row, column) under a point in panel units, if any; wide keys report their
    /// first column, which TextEntry::Focus snaps to anyway.
    static std::optional<std::pair<std::size_t, std::size_t>> KeyAt(float x, float y, bool numeric = false) {
        if (numeric) {
            const float column = (x - NumLeft) / (NumKeyWidth + Gap);
            const float row = (y - Top) / (NumKeyHeight + Gap);
            if (column < 0 || row < 0 || column >= TextEntry::NumericColumns || row >= TextEntry::NumericRows)
                return std::nullopt;
            return std::pair{static_cast<std::size_t>(row), static_cast<std::size_t>(column)};
        }
        const float column = (x - Left) / (KeyWidth + Gap);
        const float row = (y - Top) / (KeyHeight + Gap);
        if (column < 0 || row < 0 || column >= TextEntry::Columns || row >= TextEntry::Rows)
            return std::nullopt;
        return std::pair{static_cast<std::size_t>(row), static_cast<std::size_t>(column)};
    }
};

struct TextEntryPanelView {
    const TextEntry& entry;
    std::wstring_view title;
    std::wstring_view hint;
    std::wstring_view notice; ///< A warning under the text box (too short, refused by the game...).
    bool masked{};            ///< A password: the box shows dots.
};

/// Draws the panel's content with its top-left corner at (x, y) in layout units.
void DrawTextEntryPanel(UiPainter& painter, const TextEntryPanelView& view, float x, float y);

} // namespace Jadyx
