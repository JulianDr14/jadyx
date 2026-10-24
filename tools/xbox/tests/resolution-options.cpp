// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <map>
#include <stdexcept>
#include "jadyx/game/game_menu.h"
#include "jadyx/settings/system_page.h"
#include "jadyx/settings/resolution_options.h"
#include "video_core/renderer_d3d12/d3d12_resolution.h"

int main() {
    using namespace Jadyx;
    using Settings::ResolutionSetup;
    assert(SelectableResolutions().size() == 6);
    for (const auto choice : SelectableResolutions()) {
        assert(ResolutionFromValue(static_cast<int>(choice)) == choice);
        assert(EffectiveResolution(choice, false) == ResolutionSetup::Res1X);
        assert(EffectiveResolution(choice, true) == choice);
        auto next = choice;
        for (int index = 0; index < 6; ++index)
            next = StepResolution(next, false);
        assert(next == choice);
        assert(StepResolution(StepResolution(choice, false), true) == choice);
    }
    assert(!ResolutionFromValue(-1) && !ResolutionFromValue(12));
    std::map<std::wstring, int> saved;
    auto read = [&](const std::wstring& key) -> std::optional<int> {
        const auto found = saved.find(key);
        return found == saved.end() ? std::nullopt : std::optional{found->second};
    };
    assert(ReadResolution(1, read) == ResolutionSetup::Res1X);
    saved[ResolutionKey(0)] = static_cast<int>(ResolutionSetup::Res3_4X);
    assert(ReadResolution(1, read) == ResolutionSetup::Res3_4X);
    saved[ResolutionKey(1)] = static_cast<int>(ResolutionSetup::Res2X);
    assert(ReadResolution(1, read) == ResolutionSetup::Res2X);
    assert(ReadResolution(2, read) == ResolutionSetup::Res3_4X);
    saved[ResolutionKey(1)] = 999;
    assert(ReadResolution(1, read) == ResolutionSetup::Res3_4X);
    saved.erase(ResolutionKey(1));
    assert(ReadResolution(1, read) == ResolutionSetup::Res3_4X);
    assert(ResolutionKey(0) != ResolutionKey(1));
    for (const auto choice : SelectableResolutions()) {
        assert(StoreResolution(1, choice,
                               [&](const std::wstring& key, int value) { saved[key] = value; }));
        const auto reopened = saved;
        assert(ReadResolution(1, read) == choice);
        assert(reopened.at(ResolutionKey(1)) == static_cast<int>(choice));
    }
    assert(!StoreResolution(1, ResolutionSetup::Res2X,
                            [](const auto&, int) { throw std::runtime_error("full"); }));
    assert(!StoreResolution(1, static_cast<ResolutionSetup>(999),
                            [](const auto&, int) { assert(false); }));
    SystemPage page;
    page.selected = SystemPage::Resolution;
    assert(HandleSystemInput(page, SystemInput::Right) == SystemPageEffect::Changed);
    assert(page.settings.resolution == ResolutionSetup::Res3_2X);
    GameMenu menu{{}};
    while (menu.SelectedItem() != GameMenu::DisplayPage)
        menu.Apply(MenuAction::Down);
    menu.Apply(MenuAction::Confirm);
    while (menu.SelectedItem() != GameMenu::Resolution)
        menu.Apply(MenuAction::Down);
    assert(menu.Apply(MenuAction::Right) == GameMenuResult::ResolutionChanged);
    assert(menu.Settings().resolution == ResolutionSetup::Res3_2X);
    menu.SetResolutionSaveResult(ResolutionSetup::Res1X, false);
    assert(menu.Settings().resolution == ResolutionSetup::Res1X);
    assert(menu.Rows()[menu.Selected()].detail == I18n::Tr(I18n::Text::SystemSaveFailed));
    for (const auto& language : I18n::UiLanguages()) {
        I18n::SetUiLanguage(language);
        assert(!I18n::Tr(I18n::Text::ResolutionHint).empty());
        for (const auto choice : SelectableResolutions())
            assert(!I18n::Tr(ResolutionText(choice)).empty());
    }
    D3D12::ResolutionFlags flags;
    for (int index = 0; index < 130; ++index)
        flags.PushTexture(index == 31 || index == 32 || index == 127 || index == 129);
    assert(flags.words[0] == 0x80000000U && flags.words[1] == 1 && flags.words[3] == 0x80000000U);
    assert(flags.words[4] == 0 && flags.words[5] == 0);
    for (int index = 0; index < 66; ++index)
        flags.PushImage(index == 0 || index == 63 || index == 65);
    assert(flags.words[4] == 1 && flags.words[5] == 0x80000000U);

    const D3D12::ResolutionScale half{.numerator = 1, .shift = 1};
    const D3D12::ResolutionScale three_quarters{.numerator = 3, .shift = 2};
    assert(half.Size(1) == 1 && half.Size(0) == 0 && half.Size(1080) == 540);
    assert(three_quarters.Size(720) == 540);
    const auto rect = half.Rect(1, 3, 9, 11);
    assert(rect.left == 0 && rect.top == 1 && rect.right == 5 && rect.bottom == 6);
    const auto flipped = half.BlitRect(9, 11, 1, 3);
    assert(flipped.left == 5 && flipped.top == 6 && flipped.right == 0 && flipped.bottom == 1);
}
