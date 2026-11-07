// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Jadyx Memory Edit as the player meets it: one search and its page, from the first number to the
// value written. It outlives the menu, as the player goes back to the game between numbers.

#include <chrono>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/permafrost.h"
#include "jadyx/memory_edit/jme_page.h"
#include "jadyx/memory_edit/jme_session.h"
#include "jadyx/ui/ui_actions.h"

namespace Core {
class System;
}

namespace Jadyx::MemoryEdit {

class Screen {
public:
    enum class Reply : u8 { Stay, Redraw, Leave };

    explicit Screen(Core::System& system);

    /// The page comes up: the warning the first time, the list if there is one, or the pad.
    void Open();
    Reply Handle(MenuAction action);
    /// How the running search goes: true when the page has something new to show.
    bool Tick();
    /// Stops a running search and waits for it. Before the guest runs.
    void Stop();

    [[nodiscard]] MenuView View() const;
    [[nodiscard]] u64 Hits() const {
        return session.Hits();
    }

private:
    void GoHome();
    void Start();
    void ApplyEdit();
    void PickSelected();
    void MarkSelected();
    [[nodiscard]] const ResultEntry* SelectedEntry() const;
    void AfterSearch();
    void LoadRows(bool force = false);
    void UpdateHint();
    void LogSearch() const;
    void SetNotice(std::wstring text, bool warns);

    Core::System& system;
    Session session;
    Page page;
    std::vector<ResultEntry> entries;
    std::vector<ResultRow> rows;
    std::size_t rows_first{};
    bool rows_loaded{};
    /// What the pad writes to: the marked results, or the one picked.
    std::vector<Marked> editing;
    bool show_signed{};
    std::wstring notice;
    bool notice_warns{};
    std::wstring hint;
    std::wstring kind_text;

    // Of the search running, for its line in the log.
    bool searching_refines{};
    Common::Permafrost::Stats swap_before{};
    u64 app_before{};
    std::chrono::steady_clock::time_point search_start{};
    // Each drawing of the menu costs a whole frame's worth of memory: progress draws only so often.
    std::chrono::steady_clock::time_point last_draw{};
    u32 drawn_permille{};
    ScanPhase drawn_phase{};
};

} // namespace Jadyx::MemoryEdit
