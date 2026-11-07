// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/memory_edit/jme_screen.h"

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include <winrt/base.h>
#include <winrt/Windows.Storage.h>

#include "common/host_memory.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/memory_edit/jme_guest_memory.h"
#include "jadyx/settings/uwp_system_settings.h"
#include "jadyx/storage/utf8.h"

namespace Jadyx::MemoryEdit {

namespace {

using I18n::Text;

constexpr auto DRAW_EVERY = std::chrono::milliseconds{250};
constexpr u64 FEW_RESULTS = 5;

std::wstring ResultPath() {
    const std::filesystem::path folder = std::filesystem::path{std::wstring{
                                             winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}} /
                                         L"jadyx";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    return (folder / L"jme_results.bin").wstring();
}

Text KindName(ValueKind kind) {
    switch (kind) {
    case ValueKind::I8:
        return Text::MemoryEditKindI8;
    case ValueKind::I16:
        return Text::MemoryEditKindI16;
    case ValueKind::I32:
        return Text::MemoryEditKindI32;
    case ValueKind::I64:
        return Text::MemoryEditKindI64;
    case ValueKind::F32:
        return Text::MemoryEditKindF32;
    case ValueKind::F64:
        return Text::MemoryEditKindF64;
    }
    return Text::MemoryEditKindAuto;
}

const char* EndName(SearchEnd end) {
    switch (end) {
    case SearchEnd::None:
        return "none";
    case SearchEnd::Done:
        return "done";
    case SearchEnd::Partial:
        return "stopped, partial";
    case SearchEnd::Cancelled:
        return "stopped, list unchanged";
    case SearchEnd::NoMatches:
        return "no matches";
    case SearchEnd::NoMemory:
        return "no memory";
    case SearchEnd::TooManyResults:
        return "too many results";
    case SearchEnd::DiskError:
        return "disk error";
    }
    return "?";
}

std::string Mib(u64 bytes) {
    return std::to_string(bytes >> 20);
}

std::string Address(u64 va) {
    char text[24]{};
    std::snprintf(text, std::size(text), "0x%010llX", static_cast<unsigned long long>(va));
    return text;
}

} // namespace

Screen::Screen(Core::System& system_)
    : system{system_}, session{Session::Context{ResultPath(), Common::DramDecommitMutex(),
                                                [&system = system_] { return WalkGuestMemory(system); }}} {}

void Screen::Open() {
    notice.clear();
    notice_warns = false;
    page.hidden = false;
    if (session.Running()) return; // It cannot be, as closing the menu stops it.
    if (!LoadSystemSettings().memory_edit_warned) {
        page.mode = PageMode::Warning;
        return;
    }
    GoHome();
}

void Screen::GoHome() {
    if (!session.HasList()) {
        EnterNumber(page, EntryPurpose::Search);
        return;
    }
    // The game ran since the list was last drawn: its values are not the ones in the rows.
    page.mode = PageMode::List;
    ClampListWindow(page.selected, page.first, static_cast<std::size_t>(session.ShownHits()), LIST_ROWS);
    UpdateHint();
    LoadRows(true);
}

Screen::Reply Screen::Handle(MenuAction action) {
    const PageEffect effect =
        HandlePageInput(page, action, static_cast<std::size_t>(session.ShownHits()), session.HasList());
    if (effect == PageEffect::None) return Reply::Stay;
    if (effect == PageEffect::Stop) {
        // Tick sees it end.
        session.Cancel();
        return Reply::Stay;
    }
    if (effect == PageEffect::Leave) return Reply::Leave;
    notice.clear();
    notice_warns = false;
    switch (effect) {
    case PageEffect::Accept: {
        SystemSettings settings = LoadSystemSettings();
        settings.memory_edit_warned = true;
        if (!SaveSystemSettings(settings)) WriteDiag("jme: the warning's answer could not be saved");
        GoHome();
        break;
    }
    case PageEffect::Start:
        Start();
        break;
    case PageEffect::Pick:
        PickSelected();
        break;
    case PageEffect::Mark:
        MarkSelected();
        break;
    default:
        if (page.mode == PageMode::List) LoadRows();
        break;
    }
    return Reply::Redraw;
}

void Screen::SetNotice(std::wstring text, bool warns) {
    notice = std::move(text);
    notice_warns = warns;
}

void Screen::Start() {
    if (page.purpose == EntryPurpose::Edit) {
        ApplyEdit();
        return;
    }
    swap_before = Common::Permafrost::GetStats();
    u64 limit{};
    QueryAppMemory(app_before, limit);
    search_start = std::chrono::steady_clock::now();
    searching_refines = page.purpose == EntryPurpose::Refine;
    const SearchStart started =
        searching_refines ? session.Refine(page.text) : session.Search(page.text, ChosenKind(page));
    switch (started) {
    case SearchStart::Started:
        page.mode = PageMode::Searching;
        last_draw = search_start;
        drawn_permille = 0;
        drawn_phase = ScanPhase::Memory;
        break;
    case SearchStart::Busy:
        break;
    case SearchStart::BadNumber:
        SetNotice(std::wstring{I18n::Tr(Text::MemoryEditBadNumber)}, true);
        break;
    case SearchStart::Zero:
        SetNotice(std::wstring{I18n::Tr(Text::MemoryEditZero)}, true);
        break;
    case SearchStart::NoList:
        EnterNumber(page, EntryPurpose::Search, page.text);
        break;
    case SearchStart::NoMemory:
        SetNotice(std::wstring{I18n::Tr(Text::MemoryEditNoMemory)}, true);
        WriteDiag("jme: no guest memory to look through");
        break;
    case SearchStart::NoFile:
        SetNotice(std::wstring{I18n::Tr(Text::MemoryEditNoFile)}, true);
        WriteDiag("jme: the result file could not be opened");
        break;
    }
}

void Screen::AfterSearch() {
    LogSearch();
    const SearchEnd end = session.LastEnd();
    rows_loaded = false;
    if (end == SearchEnd::Done || end == SearchEnd::Partial) {
        const auto typed = ParseNumber(page.text);
        show_signed = typed && typed->negative;
        page.searched = page.text;
        page.selected = 0;
        page.first = 0;
        // A new list: what was marked may not be in it.
        page.marked.clear();
        page.mode = PageMode::List;
        UpdateHint();
        LoadRows(true);
        return;
    }
    if (end == SearchEnd::NoMatches) {
        // What was typed stays: the player may have mistyped it. A refined search left the list as it
        // was, and B goes back to it.
        page.mode = PageMode::Entry;
        SetNotice(std::wstring{I18n::Tr(searching_refines ? Text::MemoryEditNoMatchesRefine : Text::MemoryEditNoMatches)},
                  true);
        return;
    }
    const Text text = end == SearchEnd::Cancelled        ? Text::MemoryEditCancelled
                      : end == SearchEnd::NoMemory       ? Text::MemoryEditNoMemory
                      : end == SearchEnd::TooManyResults ? Text::MemoryEditTooMany
                                                         : Text::MemoryEditDiskError;
    if (session.HasList()) {
        page.mode = PageMode::List;
        UpdateHint();
        LoadRows(true);
    } else {
        page.mode = PageMode::Entry;
        page.purpose = EntryPurpose::Search;
    }
    SetNotice(std::wstring{I18n::Tr(text)}, true);
}

void Screen::UpdateHint() {
    if (session.MustRefine()) {
        hint = I18n::Format(Text::MemoryEditHintTooMany, {std::to_wstring(SHOWN_WHEN_TOO_MANY)});
    } else {
        hint = I18n::Tr(session.Hits() <= FEW_RESULTS ? Text::MemoryEditHintFew : Text::MemoryEditHintMany);
    }
    if (session.IsPartial()) {
        hint += L" ";
        hint += I18n::Tr(Text::MemoryEditHintPartial);
    }
}

void Screen::LoadRows(bool force) {
    if (!force && rows_loaded && rows_first == page.first) return;
    rows.clear();
    rows_first = page.first;
    rows_loaded = true;
    const u64 shown = session.ShownHits();
    if (page.first >= shown) {
        entries.clear();
        return;
    }
    const u32 count = static_cast<u32>(std::min<u64>(LIST_ROWS, shown - page.first));
    if (!session.ReadEntries(page.first, count, entries)) {
        SetNotice(std::wstring{I18n::Tr(Text::MemoryEditReadFailed)}, true);
        WriteDiag("jme: the result file could not be read");
    }
    for (const ResultEntry& entry : entries) {
        const auto bits = ReadGuestValue(system, entry.va, entry.kind);
        ResultRow row;
        row.value = bits ? Utf8ToUtf16(FormatValue(entry.kind, *bits, show_signed)) : L"—";
        row.details = std::wstring{I18n::Tr(KindName(entry.kind))} + L" · " + Utf8ToUtf16(Address(entry.va));
        row.marked = IsMarked(page, {entry.va, entry.kind});
        rows.push_back(std::move(row));
    }
}

const ResultEntry* Screen::SelectedEntry() const {
    const std::size_t index = page.selected - rows_first;
    if (page.selected < rows_first || index >= entries.size()) return nullptr;
    return &entries[index];
}

void Screen::MarkSelected() {
    const ResultEntry* entry = SelectedEntry();
    if (!entry) return;
    ToggleMark(page, {entry->va, entry->kind});
    LoadRows(true);
}

void Screen::PickSelected() {
    const ResultEntry* entry = SelectedEntry();
    if (!entry) return;
    const Marked selected{entry->va, entry->kind};
    editing = page.marked.empty() ? std::vector<Marked>{selected} : page.marked;
    // The pad opens on the selected one when it is among those written, or on the first marked.
    const Marked shown = std::find(editing.begin(), editing.end(), selected) != editing.end() ? selected : editing.front();
    const bool one_kind = std::all_of(editing.begin(), editing.end(),
                                      [&](const Marked& target) { return target.kind == shown.kind; });
    kind_text = I18n::Tr(one_kind ? KindName(shown.kind) : Text::MemoryEditKindMixed);
    // The pad opens on what memory holds, when the pad could have written it: not 1e+20 or nan.
    std::string current;
    if (const auto bits = ReadGuestValue(system, shown.va, shown.kind)) {
        current = FormatValue(shown.kind, *bits, show_signed);
        if (current.size() > MAX_TYPED || !ParseNumber(current)) current.clear();
    }
    EnterNumber(page, EntryPurpose::Edit, current);
}

void Screen::ApplyEdit() {
    if (editing.empty()) return;
    const auto number = ParseNumber(page.text);
    // All of them or none: a number one of them cannot hold writes nowhere.
    std::vector<u64> encoded;
    for (const Marked& target : editing) {
        const auto bits = number ? EncodeValue(*number, target.kind) : std::nullopt;
        if (!bits) {
            SetNotice(std::wstring{I18n::Tr(Text::MemoryEditBadNumber)}, true);
            return;
        }
        encoded.push_back(*bits);
    }
    std::size_t held_count = 0;
    std::optional<u64> first_now;
    for (std::size_t i = 0; i < editing.size(); ++i) {
        const Marked& target = editing[i];
        const bool wrote = WriteGuestValue(system, target.va, encoded[i], target.kind);
        const auto now = ReadGuestValue(system, target.va, target.kind);
        const bool held = wrote && now && *now == encoded[i];
        if (held) ++held_count;
        if (i == 0) first_now = now;
        WriteDiag("jme: wrote " + page.text + " at " + Address(target.va) + (held ? ", it holds" : ", it did NOT hold"));
    }
    show_signed = show_signed || number->negative;
    page.mode = PageMode::List;
    UpdateHint();
    LoadRows(true);
    const std::size_t total = editing.size();
    if (held_count == 0) {
        SetNotice(std::wstring{I18n::Tr(Text::MemoryEditEditFailed)}, true);
    } else if (held_count < total) {
        SetNotice(I18n::Format(Text::MemoryEditEditedSome, {std::to_wstring(held_count), std::to_wstring(total)}), true);
    } else {
        const std::wstring value = Utf8ToUtf16(FormatValue(editing.front().kind, *first_now, show_signed));
        SetNotice(total == 1 ? I18n::Format(Text::MemoryEditEdited, {value})
                             : I18n::Format(Text::MemoryEditEditedMany, {std::to_wstring(total), value}),
                  false);
    }
}

bool Screen::Tick() {
    if (!session.Running()) return false;
    if (session.Poll()) {
        AfterSearch();
        return true;
    }
    const SearchProgress progress = session.Progress();
    if (progress.permille == drawn_permille && progress.phase == drawn_phase) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_draw < DRAW_EVERY) return false;
    last_draw = now;
    drawn_permille = progress.permille;
    drawn_phase = progress.phase;
    return true;
}

void Screen::Stop() {
    if (!session.Running()) return;
    session.Stop();
    AfterSearch();
}

MenuView Screen::View() const {
    const SearchProgress progress = session.Progress();
    std::wstring_view kind;
    if (page.mode == PageMode::Entry && page.purpose == EntryPurpose::Edit) {
        kind = kind_text;
    } else if (page.mode == PageMode::Entry && page.purpose == EntryPurpose::Search) {
        const auto chosen = ChosenKind(page);
        kind = I18n::Tr(chosen ? KindName(*chosen) : Text::MemoryEditKindAuto);
    }
    return {page,
            rows,
            session.Hits(),
            session.ShownHits(),
            progress.phase == ScanPhase::Disk,
            progress.permille,
            kind,
            notice,
            notice_warns,
            hint};
}

void Screen::LogSearch() const {
    const ScanStats& stats = session.LastStats();
    const Common::Permafrost::Stats swap = Common::Permafrost::GetStats();
    u64 app{}, limit{};
    QueryAppMemory(app, limit);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - search_start)
                        .count();
    WriteDiag(std::string{"jme: "} + (searching_refines ? "refine " : "search ") + page.text + " | " +
              std::to_string(stats.TotalHits()) + " found, list " + std::to_string(session.Hits()) + " | " +
              EndName(session.LastEnd()) + " in " + std::to_string(ms) + " ms | memory " + Mib(stats.memory_bytes) +
              " MiB " + std::to_string(stats.memory_ms) + " ms | disk " + Mib(stats.disk_bytes) + " MiB " +
              std::to_string(stats.disk_ms) + " ms | sweep " + Mib(stats.sweep_bytes) + " MiB " +
              std::to_string(stats.sweep_ms) + " ms | app " + (app >= app_before ? "+" + Mib(app - app_before) : "-" + Mib(app_before - app)) +
              " MiB | swap_ins +" + std::to_string(swap.swap_ins - swap_before.swap_ins) + " probation_touches +" +
              std::to_string(swap.probation_touches - swap_before.probation_touches) + " refaults +" +
              std::to_string(swap.refaults - swap_before.refaults) + " fault_stall_us +" +
              std::to_string(swap.fault_stall_us - swap_before.fault_stall_us));
}

} // namespace Jadyx::MemoryEdit
