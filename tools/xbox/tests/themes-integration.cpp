// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>

#include "jadyx/settings/system_page.h"
#include "jadyx/ui/ui_theme.h"

int main() {
  using namespace Jadyx;
  static_assert(SystemPage::Resolution == SystemPage::Updates + 1);
  static_assert(SystemPage::Theme == SystemPage::Resolution + 1);
  static_assert(SystemPage::RowCount == SystemPage::Theme + 1);
  static_assert(Palette::CompatibilityPlayable >= Palette::First);
  static_assert(Palette::CompatibilityBroken >= Palette::First);
  static_assert(Palette::CompatibilityPlayable != Palette::CompatibilityBroken);
  assert(JadyxTheme.Color(Palette::CompatibilityPlayable).rgb == 0x73d69b);
  assert(JadyxTheme.Color(Palette::CompatibilityBroken).rgb == 0xf08b8b);
  for (const auto id : {ThemeId::Jadyx, ThemeId::OldNxbox}) {
    const auto &theme = LookupTheme(id);
    for (const auto token :
         {Palette::CompatibilityPlayable, Palette::CompatibilityBroken,
          Palette::Warning, Palette::TextSecondary}) {
      const auto color = theme.Color(token);
      assert(color.rgb <= 0xffffff && color.alpha > 0 && color.alpha <= 1);
      assert(color.rgb != theme.Color(Palette::Canvas).rgb);
    }
  }
  for (const auto &language : I18n::UiLanguages()) {
    I18n::SetUiLanguage(language);
    SystemPage page;
    page.settings.resolution = Settings::ResolutionSetup::Res2X;
    page.selected = SystemPage::Theme;
    assert(SystemRowTitle(SystemPage::Theme) == I18n::Text::Theme);
    assert(HandleSystemInput(page, SystemInput::Confirm) ==
           SystemPageEffect::Changed);
    assert(page.settings.theme == ThemeId::OldNxbox);
    assert(page.settings.resolution == Settings::ResolutionSetup::Res2X);
    HandleSystemInput(page, SystemInput::Up);
    assert(page.selected == SystemPage::Resolution);
    assert(HandleSystemInput(page, SystemInput::Right) ==
           SystemPageEffect::Changed);
    assert(page.settings.theme == ThemeId::OldNxbox);
    HandleSystemInput(page, SystemInput::Down);
    assert(page.selected == SystemPage::Theme);
    assert(HandleSystemInput(page, SystemInput::Left) ==
           SystemPageEffect::Changed);
    assert(page.settings.theme == ThemeId::Jadyx);
    assert(!I18n::Tr(I18n::Text::CompatibilityPlayable).empty());
    assert(!I18n::Tr(I18n::Text::FtpStartFailed).empty());
  }
  std::puts("theme integration: badge tokens, row order, independent settings "
            "and locales PASS");
}
