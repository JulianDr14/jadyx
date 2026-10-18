// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/cheats/cheats_view.h"

#include <algorithm>
#include <string>

#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_painter.h"
#include "jadyx/storage/utf8.h"

namespace Jadyx::Cheats {
using I18n::Text;

std::wstring_view CategoryName(CheatCategory category) {
    switch (category) {
    case CheatCategory::Gameplay: return I18n::Tr(Text::CheatsCategoryGameplay);
    case CheatCategory::Graphics: return I18n::Tr(Text::CheatsCategoryGraphics);
    case CheatCategory::Mine: return I18n::Tr(Text::CheatsCategoryMine);
    }
    return {};
}

std::wstring_view SourceText(CheatSource source) {
    switch (source) {
    case CheatSource::Checking: return I18n::Tr(Text::CheatsSourceChecking);
    case CheatSource::Online: return I18n::Tr(Text::CheatsSourceOnline);
    case CheatSource::Cached: return I18n::Tr(Text::CheatsSourceCached);
    case CheatSource::Offline: return I18n::Tr(Text::CheatsOffline);
    }
    return {};
}

namespace {
using L = CheatsLayout;

/// Tabs with how many of their cheats are on; LB and RB beside them when there is a choice.
void DrawTabs(UiPainter& paint, const CheatCatalog& catalog, CheatCategory current, float x, float y, bool focused, std::wstring_view previous, std::wstring_view next) {
    std::array<CheatCategory, CategoryCount> storage;
    const auto tabs = CheatTabs(catalog, storage);
    paint.Round(x + L::Margin, y - 4, L::Width - 2 * L::Margin, L::TabHeight + 8, 24, Palette::Surface);
    float left = x + L::Margin;
    if (tabs.size() > 1) {
        paint.Badge(previous, left + 18, y + L::TabHeight / 2, Palette::Text);
        left += 50;
    }
    std::size_t tab_index = 0;
    for (const CheatCategory category : tabs) {
        const std::wstring label = std::wstring{CategoryName(category)} + L"  " +
                                   std::to_wstring(catalog.EnabledCount(category)) + L"/" +
                                   std::to_wstring(catalog.Cheats(category).size());
        const bool active = category == current;
        const float width = paint.MeasureType(label, TypeRole::LabelLarge) + 36;
        paint.Round(left, y, width, L::TabHeight, L::TabHeight / 2, active && focused ? Palette::Accent : Palette::Row);
        paint.Type(label, TypeRole::LabelLarge, left, y + 6, width,
                   active && focused ? Palette::OnAccent : active ? Palette::Text : Palette::TextSecondary,
                   1, TextAlign::Center);
        paint.Target(UiTarget::ManagerTab, tab_index++, left, y, width, L::TabHeight);
        left += width + 10;
    }
    if (tabs.size() > 1) paint.Badge(next, left + 20, y + L::TabHeight / 2, Palette::Text);
}

void DrawList(UiPainter& paint, const CheatCatalog& catalog, const CheatsPage& page, float x, float y, bool focused,
              float scroll) {
    const auto cheats = catalog.Cheats(page.category);
    const float row_width = L::Width - 2 * L::Margin;
    const float first = scroll < 0 ? static_cast<float>(page.first) : scroll;
    const float height = L::VisibleRows * L::RowStride - (L::RowStride - L::RowHeight);
    // Rows glide between windows; the clip keeps them under the tabs.
    paint.PushClip(x, y + L::RowTop - 4, L::Width, height + 8);
    const std::size_t begin = static_cast<std::size_t>(first);
    const std::size_t end = std::min(cheats.size(), begin + L::VisibleRows + 1);
    for (std::size_t i = begin; i < end; ++i) {
        const float top = y + L::RowTop + (static_cast<float>(i) - first) * L::RowStride;
        const float left = x + L::Margin;
        const bool active = i == page.selected && focused;
        const bool on = catalog.Enabled(cheats[i]);
        paint.SelectionCard(left, top, row_width, L::RowHeight, active, 12);
        paint.Type(Utf8ToUtf16(catalog.Name(cheats[i])), TypeRole::BodyLarge, left + 24, top + 6,
                   row_width - 130, on || active ? Palette::Text : Palette::TextHint);
        paint.Switch(left + row_width - 76, top + 5, on, active);
        if (i >= page.first && i < page.first + L::VisibleRows)
            paint.Target(UiTarget::ManagerRow, i, left, top, row_width, L::RowHeight);
    }
    paint.PopClip();
    paint.ScrollIndicator(x + L::Width - L::Margin + 14, y + L::RowTop, height,
                          page.first, L::VisibleRows, cheats.size());
}

/// Why the list is empty, in its place.
void DrawEmpty(UiPainter& paint, const CheatSession& session, float x, float y) {
    const float width = L::Width - 2 * L::Margin;
    const float top = y + L::RowTop + 70;
    std::wstring message;
    if (session.Loading() || session.Source() == CheatSource::Checking) {
        message = I18n::Tr(Text::CheatsLoading);
    } else if (session.Source() == CheatSource::Offline) {
        message = I18n::Tr(Text::CheatsOffline);
    } else {
        message = I18n::Tr(Text::CheatsNone);
        if (session.OtherBuilds())
            message += L"\n" + I18n::Plural(Text::CheatsOtherBuildsOne, Text::CheatsOtherBuildsOther,
                                            session.OtherBuilds());
    }
    paint.Icon(UiIcon::Cheats, x + L::Width / 2 - 16, top - 50, Palette::TextSecondary);
    paint.Text(message, x + L::Margin, top, 20, Palette::TextHint, width, 120, false, true, true);
}

void DrawStatus(UiPainter& paint, const CheatsView& view, float x, float y) {
    const CheatSession& session = view.session;
    const std::size_t used = session.Catalog().UsedWords();
    const float left = x + L::Margin;
    const float right = x + L::Width - L::Margin;
    // The cheat engine's room: what the enabled cheats take of the VM's program.
    const auto budget = I18n::Format(Text::CheatsBudget, {std::to_wstring(used), std::to_wstring(WordBudget)});
    const float budget_width = paint.MeasureType(budget, TypeRole::Label);
    paint.Type(budget, TypeRole::Label, left, y + L::Status, budget_width + 4, Palette::TextSecondary);
    constexpr float BarWidth = 160;
    const float bar = left + budget_width + 14;
    const float filled = BarWidth * static_cast<float>(std::min(used, WordBudget)) / WordBudget;
    paint.Round(bar, y + L::Status + 5, BarWidth, 6, 3, Palette::Track);
    if (filled > 0)
        paint.Round(bar, y + L::Status + 5, std::max(filled, 6.0f), 6, 3,
                    used * 5 > WordBudget * 4 ? Palette::Warning : Palette::Text);
    // Where the cheats came from, and who made them, on the right of both lines.
    paint.Type(SourceText(session.Source()), TypeRole::Label, bar + BarWidth + 24, y + L::Status,
               right - bar - BarWidth - 24, session.Source() == CheatSource::Online ? Palette::TextHint : Palette::Warning,
               1, TextAlign::Trailing);
    std::wstring credit{I18n::Tr(Text::CheatsCredit)};
    for (std::size_t i = 0; i < std::min<std::size_t>(session.Credits().size(), 3); ++i)
        credit += (i ? L", " : L" · ") + Utf8ToUtf16(session.Credits()[i]);
    // The last press's outcome, or when changes take effect.
    const bool notice = !view.notice.empty();
    const std::wstring_view note = notice ? view.notice
                                   : I18n::Tr(view.running ? Text::CheatsAppliesNow : Text::CheatsAppliesAtBoot);
    const float note_width = std::min(paint.MeasureType(note, TypeRole::Label) + 4, (right - left) / 2);
    paint.Type(note, TypeRole::Label, left, y + L::Note, note_width, notice ? Palette::Warning : Palette::TextSecondary);
    paint.Type(credit, TypeRole::Label, left + note_width + 24, y + L::Note, right - left - note_width - 24,
               Palette::TextSecondary, 1, TextAlign::Trailing);
}
} // namespace

void DrawCheatsPanel(UiPainter& paint, const CheatsView& view, float x, float y) {
    const CheatCatalog& catalog = view.session.Catalog();
    // The game options' shell draws the footer's hairline: this surface moves with the header.
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Canvas);
    if (catalog.Empty()) {
        DrawEmpty(paint, view.session, x, y);
    } else {
        DrawTabs(paint, catalog, view.page.category, x, y + L::TabsTop, view.tabs_focused, view.previous_tab, view.next_tab);
        DrawList(paint, catalog, view.page, x, y, view.content_focused, view.scroll);
    }

}

void DrawCheatsStatus(UiPainter& paint, const CheatsView& view, float x, float y) {
    DrawStatus(paint, view, x, y);
}

} // namespace Jadyx::Cheats
