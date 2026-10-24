// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/core/memory/cheat_text.cpp.
#include <cstdio>
#include <cstdlib>
#include "jadyx/cheats/cheats_page.h"
#include "jadyx/library/game_options_page.h"

namespace {
using namespace Jadyx;
using namespace Jadyx::Cheats;

/// What CheatSession gives the page, over a plain catalog.
struct Model {
    CheatCatalog catalog;
    int saves = 0;
    const CheatCatalog& Catalog() const { return catalog; }
    ToggleResult Toggle(CheatRef ref) {
        const auto result = catalog.Toggle(ref);
        saves += result == ToggleResult::Changed;
        return result;
    }
    ToggleResult SetCategory(CheatCategory category, bool on) { return catalog.SetCategory(category, on); }
};

CheatSet Set(std::string mod, int count, int words = 1) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += "[Cheat " + std::to_string(i) + "]\n";
        for (int w = 0; w < words; ++w) text += "00000000 ";
        text += "\n";
    }
    auto cheats = Core::Memory::TextCheatParser{}.Parse(text);
    for (auto& cheat : cheats) cheat.enabled = false;
    return {mod, CategoryOfMod(mod), std::move(cheats), {}};
}
} // namespace

int main() {
    static_assert(CheatsLayout::RowTop - GameOptionsLayout::HeroCollapse +
        (CheatsLayout::VisibleRows - 1) * CheatsLayout::RowStride + CheatsLayout::RowHeight <= GameOptionsLayout::ListBottom);
    if (CheatsInputForAction(MenuAction::Toggle) ||
        CheatsInputForAction(MenuAction::Left) != CheatsInput::PreviousCategory ||
        CheatsInputForAction(MenuAction::PreviousTab) != CheatsInput::PreviousCategory ||
        CheatsInputForAction(MenuAction::Right) != CheatsInput::NextCategory ||
        CheatsInputForAction(MenuAction::Confirm) != CheatsInput::Toggle ||
        CheatsInputForAction(MenuAction::Alternate) != CheatsInput::ToggleAll)
        std::abort();

    auto check = [](bool ok) { if (!ok) std::abort(); };
    constexpr std::size_t rows = 3;

    // Tabs follow the categories that have cheats; the page lands on the first.
    Model model{CheatCatalog{{Set("Online graphics", 2), Set("Online cheats", 5), Set("Mine", 0)}}};
    CheatsPage page{CheatCategory::Mine, 9, 9};
    ClampCheatsPage(page, model.catalog, rows);
    check(page.category == CheatCategory::Gameplay && page.selected == 0 && page.first == 0);
    std::array<CheatCategory, CategoryCount> tab_storage;
    const auto tabs = CheatTabs(model.catalog, tab_storage);
    check(tabs.size() == 2 && tabs[0] == CheatCategory::Gameplay && tabs[1] == CheatCategory::Graphics);
    check(tabs.data() == tab_storage.data() && CheatTabCount(model.catalog) == tabs.size());
    check(CheatTabs(CheatCatalog{}, tab_storage).empty());

    // Down scrolls the window with the cursor and wraps to the top.
    for (int i = 0; i < 4; ++i) check(HandleCheatsInput(page, model, CheatsInput::Down, rows) == CheatsEffect::Moved);
    check(page.selected == 4 && page.first == 2);
    check(HandleCheatsInput(page, model, CheatsInput::Down, rows) == CheatsEffect::Moved);
    check(page.selected == 0 && page.first == 0);
    check(HandleCheatsInput(page, model, CheatsInput::Up, rows) == CheatsEffect::Moved);
    check(page.selected == 4 && page.first == 2);

    // A toggles and saves; X turns the whole tab on, then off again.
    check(HandleCheatsInput(page, model, CheatsInput::Toggle, rows) == CheatsEffect::Changed && model.saves == 1);
    check(model.catalog.EnabledCount(CheatCategory::Gameplay) == 1);
    check(HandleCheatsInput(page, model, CheatsInput::ToggleAll, rows) == CheatsEffect::Changed);
    check(model.catalog.EnabledCount(CheatCategory::Gameplay) == 5);
    check(HandleCheatsInput(page, model, CheatsInput::ToggleAll, rows) == CheatsEffect::Changed);
    check(model.catalog.EnabledCount(CheatCategory::Gameplay) == 0);

    // The bumpers switch tabs (wrapping) and start the new tab from its top.
    check(HandleCheatsInput(page, model, CheatsInput::NextCategory, rows) == CheatsEffect::Moved);
    check(page.category == CheatCategory::Graphics && page.selected == 0);
    check(HandleCheatsInput(page, model, CheatsInput::NextCategory, rows) == CheatsEffect::Moved);
    check(page.category == CheatCategory::Gameplay);
    check(HandleCheatsInput(page, model, CheatsInput::PreviousCategory, rows) == CheatsEffect::Moved);
    check(page.category == CheatCategory::Graphics);

    // Refresh and Back are the host's to act on.
    check(HandleCheatsInput(page, model, CheatsInput::Refresh, rows) == CheatsEffect::Refresh);
    check(HandleCheatsInput(page, model, CheatsInput::Back, rows) == CheatsEffect::Back);

    // A cheat that does not fit the VM is refused and says so.
    Model full{CheatCatalog{{Set("Online cheats", 5, 0xF0)}}};
    CheatsPage big;
    check(HandleCheatsInput(big, full, CheatsInput::ToggleAll, rows) == CheatsEffect::OverBudget);
    check(full.catalog.EnabledCount(CheatCategory::Gameplay) == 4 && full.catalog.UsedWords() <= WordBudget);
    big.selected = 4;
    check(HandleCheatsInput(big, full, CheatsInput::Toggle, rows) == CheatsEffect::OverBudget);

    // An empty catalog: nothing moves, nothing toggles.
    Model empty;
    CheatsPage none;
    ClampCheatsPage(none, empty.catalog, rows);
    check(HandleCheatsInput(none, empty, CheatsInput::Down, rows) == CheatsEffect::None);
    check(HandleCheatsInput(none, empty, CheatsInput::Toggle, rows) == CheatsEffect::None);
    check(HandleCheatsInput(none, empty, CheatsInput::NextCategory, rows) == CheatsEffect::None);

    // Selecting a category activates its manager without an intermediate Open action; the focus
    // starts in its pane (LB/RB switch categories, not the stick).
    GameOptionsPage options{true, 7};
    HandleGameOptionsInput(options, GameOptionsInput::Up);
    check(options.selected == GameOptions().size() - 1); // the list wraps
    HandleGameOptionsInput(options, GameOptionsInput::Down);
    HandleGameOptionsInput(options, GameOptionsInput::Down);
    HandleGameOptionsInput(options, GameOptionsInput::Down); // Information, then Mods, then Cheats
    check(GameOptions()[options.selected] == GameOption::Cheats);
    check(options.page == GameOption::Cheats && options.activate && (options.focus == GameOptionsFocus::Content));
    HandleGameOptionsInput(options, GameOptionsInput::Confirm);
    check(options.page == GameOption::Cheats && options.open && !(options.focus == GameOptionsFocus::Sections));
    HandleGameOptionsInput(options, GameOptionsInput::Back);
    check(!options.open);

    // Sections -> manager tabs -> content, and back: neither focus change reopens the manager.
    SelectGameOption(options, 0);
    options.activate = false; // The coordinator finished initializing Mods.
    const auto active_page = options.page;
    options.focus = GameOptionsFocus::Sections; // The helpers still walk all three levels.
    EnterGameOptionsPane(options, true);
    check(options.focus == GameOptionsFocus::ManagerTabs && !options.activate);
    EnterGameOptionsPane(options, true);
    check(options.focus == GameOptionsFocus::Content && options.page == active_page);
    LeaveGameOptionsPane(options, true);
    check(options.focus == GameOptionsFocus::ManagerTabs && !options.activate);
    LeaveGameOptionsPane(options, true);
    check(options.focus == GameOptionsFocus::Sections && options.page == active_page);
    EnterGameOptionsPane(options, false);
    check(options.focus == GameOptionsFocus::Content);
    options.confirm_discard = true;
    LeaveGameOptionsPane(options, false);
    check(options.focus == GameOptionsFocus::Sections && !options.confirm_discard);

    // Pulse still requires two confirms; changing category cancels it.
    options = {true, 7};
    HandleGameOptionsInput(options, GameOptionsInput::Up);
    check(GameOptions()[options.selected] == GameOption::Pulse);
    HandleGameOptionsInput(options, GameOptionsInput::Confirm);
    check(options.confirm_discard && !options.discard);
    HandleGameOptionsInput(options, GameOptionsInput::Down);
    check(!options.confirm_discard && !options.discard);
    HandleGameOptionsInput(options, GameOptionsInput::Up);
    HandleGameOptionsInput(options, GameOptionsInput::Confirm);
    HandleGameOptionsInput(options, GameOptionsInput::Confirm);
    check(options.discard && !options.confirm_discard && !options.page);

    for (std::size_t i = 0; i < GameOptions().size(); ++i) {
        SelectGameOption(options, i);
        check(options.selected == i && options.activate && (options.focus == GameOptionsFocus::Sections));
        check(!options.discard && !options.confirm_discard);
        check(GameOptions()[i] == GameOption::Pulse ? !options.page : options.page == GameOptions()[i]);
    }
    SelectGameOption(options, GameOptions().size());
    check(options.selected == GameOptions().size() - 1); // invalid pointer index is ignored

    std::puts("cheats page: tabs, scrolling, toggles, all on/off, budget and game options PASS");
    return 0;
}
