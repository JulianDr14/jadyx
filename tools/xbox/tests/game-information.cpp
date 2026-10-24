// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <limits>
#include "jadyx/library/game_information.h"
#include "jadyx/library/game_options_page.h"

int main() {
    using namespace Jadyx;
    using T = I18n::Text;
    assert(FormatGameSize(0) == L"0 B");
    assert(FormatGameSize(1023) == L"1023 B");
    assert(FormatGameSize(1024) == L"1.0 KiB");
    assert(FormatGameSize(1536) == L"1.5 KiB");
    assert(FormatGameSize(5ULL << 30) == L"5.0 GiB");
    assert(FormatGameSize(std::numeric_limits<std::uint64_t>::max()) == L"15.9 EiB");
    assert(FormatGameSize(104858) == L"102.4 KiB");
    Content::Revision update;
    update.application = 1;
    update.kind = Content::Kind::Update;
    Content::Revision dlc;
    dlc.application = 1;
    dlc.title = 2;
    dlc.kind = Content::Kind::Dlc;
    const std::array revisions{update, update, dlc, dlc};
    const auto counts = CountGameContent(revisions, 1);
    assert(counts.updates == 2 && counts.dlc == 1);
    assert(CountGameContent(revisions, 3).updates == 0);
    assert(CountGameContent(revisions, 0).dlc == 0);
    LibraryEntry game;
    game.name = L"Example";
    game.relative_path = "Example.NSP";
    game.launch_path = "/games/Example.NSP";
    game.program_id = 0x0100000000000000ULL;
    GameInformationFacts facts;
    auto data = BuildGameInformation(game, facts, {});
    assert(data.program_id == L"0100000000000000");
    assert(data.format == L"NSP");
    assert(data.size == I18n::Tr(T::GameInfoUnknown));
    assert(data.storage == I18n::Tr(T::SourceInternal));
    assert(data.path == L"/games/Example.NSP");
    assert(data.compatibility == T::CompatibilityUntested && !data.reported);
    assert(data.play_time.empty() && data.last_played.empty());
    assert(CanRemoveGame(game));
    GameInformationFacts zero;
    zero.bytes = 0;
    zero.content = GameContentCounts{};
    const auto empty = BuildGameInformation(game, zero, {});
    assert(empty.size == L"0 B" && empty.updates == L"0" && empty.dlc == L"0");
    game.launch_path = "jadyx-usb:/jadyx-games-test/folder/Example.NSP";
    game.relative_path = "folder/Example.NSP";
    game.source_name = L"My drive";
    game.play_seconds = 90;
    game.last_played = 86400;
    facts.bytes = 5ULL << 30;
    facts.content = GameContentCounts{.updates = 2, .dlc = 3};
    Compatibility::Report report;
    report.product = "Jadyx";
    report.console = "S";
    report.status = Compatibility::Status::Issues;
    report.fps_note = "30 FPS";
    report.jadyx_version = "0.3.8.0";
    report.date = "2026-10-20";
    data = BuildGameInformation(game, facts, report);
    assert(data.storage.find(L"My drive") != std::wstring::npos);
    assert(data.path == L"folder/Example.NSP");
    assert(data.size == L"5.0 GiB" && data.updates == L"2" && data.dlc == L"3");
    assert(data.reported && data.compatibility == T::CompatibilityIssues);
    assert(data.report.fps_note == "30 FPS" && data.report.date == "2026-10-20");
    assert(!data.play_time.empty() && !data.last_played.empty());
    assert(!CanRemoveGame(game));
    report.product = "NXbox";
    assert(!BuildGameInformation(game, facts, report).reported);
    assert(GameOptions().front() == GameOption::Information);
    GameOptionsPage page;
    page.open = true;
    SelectGameOption(page, 0);
    assert(page.page == GameOption::Information);
    HandleGameOptionsInput(page, GameOptionsInput::Confirm);
    assert(!page.confirm_discard && !page.discard);
    HandleGameOptionsInput(page, GameOptionsInput::Back);
    assert(!page.open);
    // The new first category never changes the two-step Remove/Pulse contract.
    for (std::size_t index = 0; index < GameOptions().size(); ++index) {
        SelectGameOption(page, index);
        const auto option = GameOptions()[index];
        assert(!page.confirm_discard && !page.discard);
        HandleGameOptionsInput(page, GameOptionsInput::Confirm);
        if (option == GameOption::Remove || option == GameOption::Pulse) {
            assert(page.confirm_discard && !page.discard);
            HandleGameOptionsInput(page, GameOptionsInput::Confirm);
            assert(page.discard && !page.confirm_discard);
        } else
            assert(!page.confirm_discard && !page.discard);
    }
    game.program_id = 0;
    game.launch_path = "/games/example.nro";
    assert(!CanRemoveGame(game));
    data = BuildGameInformation(game, {}, {});
    assert(data.program_id == I18n::Tr(T::GameInfoUnknown));
    for (const auto& language : I18n::UiLanguages()) {
        I18n::SetUiLanguage(language);
        data = BuildGameInformation(game, facts, {});
        assert(data.compatibility == T::CompatibilityUntested);
        assert(data.storage == I18n::Tr(T::SourceInternal));
        game.launch_path = "jadyx-usb:/jadyx-games-test/folder/Example.NSP";
        data = BuildGameInformation(game, facts, {});
        assert(data.storage == I18n::Format(T::GameInfoExternal, {L"My drive"}));
        assert(data.path == L"folder/Example.NSP");
        assert(!I18n::Tr(T::GameInfoTitle).empty());
        game.launch_path = "/games/example.nro";
    }
    std::puts(
        "PASS game information: size, storage, metadata, reports, history, removal and navigation");
}
