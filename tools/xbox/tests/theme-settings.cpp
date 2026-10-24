// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include "jadyx/library/library_layout.h"
#include "jadyx/settings/system_page.h"

int main() {
    using namespace Jadyx;
    static_assert(LibraryLayout::Margin == 64 && LibraryLayout::Tile == 144);
    static_assert(LibraryLayout::Gutter == 24 && LibraryLayout::Columns == 7);
    static_assert(LibraryLayout::RowTop == 474 && LibraryLayout::RowHeight == 216);
    static_assert(LibraryLayout::HeaderTop == 34 && LibraryLayout::FooterTop == 664);
    assert(LibraryLayout::TileLeft(0) == 64 && LibraryLayout::TileLeft(6) == 1072);
    assert(LibraryLayout::RowContentTop(1, 1) == 258);
    assert(LibraryLayout::ShelfScroll(14) == 204);
    SystemPage page;
    assert(page.settings.theme == ThemeId::Jadyx);
    page.selected = SystemPage::Theme;
    for (const auto& language : I18n::UiLanguages()) {
        I18n::SetUiLanguage(language);
        assert(!I18n::Tr(SystemRowTitle(SystemPage::Theme)).empty());
        assert(!I18n::Tr(SystemRowHint(SystemPage::Theme)).empty());
        assert(SystemRowValue(page, SystemPage::Theme) == L"Jadyx");
        assert(HandleSystemInput(page, SystemInput::Confirm) == SystemPageEffect::Changed);
        assert(page.settings.theme == ThemeId::OldNxbox);
        assert(SystemRowValue(page, SystemPage::Theme) == L"Old NXbox");
        const auto stored = ThemeValue(page.settings.theme);
        SystemSettings reloaded;
        reloaded.theme = ThemeFromValue(stored);
        assert(reloaded.theme == page.settings.theme);
        assert(HandleSystemInput(page, SystemInput::Left) == SystemPageEffect::Changed);
        assert(page.settings.theme == ThemeId::Jadyx);
        HandleSystemInput(page, SystemInput::Right);
        HandleSystemInput(page, SystemInput::Right);
        assert(page.settings.theme == ThemeId::Jadyx);
    }
    std::puts("theme settings: default, input, locales and saved value round-trip PASS");
}
