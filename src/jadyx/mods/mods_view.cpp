// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/mods/mods_view.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>

#include "jadyx/ui/text_entry_panel.h"
#include "jadyx/ui/ui_painter.h"
#include "jadyx/storage/utf8.h"

namespace Jadyx::Mods {
namespace {
using I18n::Text;
using I18n::Tr;
using L = ModsLayout;

constexpr float RowWidth = L::Width - 2 * L::Margin;
const std::wstring Dot =L"  ·  ";

/// "512 KB", "37 MB", "1.4 GB".
std::wstring Size(std::uint64_t bytes) {
    wchar_t text[32];
    if (bytes >= (1ull << 30)) std::swprintf(text, 32, L"%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20)) std::swprintf(text, 32, L"%llu MB", static_cast<unsigned long long>(bytes >> 20));
    else std::swprintf(text, 32, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) >> 10));
    return text;
}

/// "950", "12K", "1.3M": counts that fit a card.
std::wstring Count(std::int64_t value) {
    wchar_t text[32];
    if (value >= 1'000'000) std::swprintf(text, 32, L"%.1fM", static_cast<double>(value) / 1e6);
    else if (value >= 10'000) std::swprintf(text, 32, L"%lldK", static_cast<long long>(value / 1000));
    else std::swprintf(text, 32, L"%lld", static_cast<long long>(std::max<std::int64_t>(value, 0)));
    return text;
}

/// "2022-06-12": the same in every language.
std::wstring Date(std::int64_t unix_time) {
    if (unix_time <= 0) return {};
    const std::chrono::sys_days day{std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{std::chrono::seconds{unix_time}})};
    const std::chrono::year_month_day ymd{day};
    wchar_t text[16];
    std::swprintf(text, 16, L"%04d-%02u-%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()));
    return text;
}

std::wstring Parts(std::uint8_t parts) {
    std::wstring text;
    const std::pair<ModPart, Text> names[] = {
        {RomFs, Text::ModsPartRomFs}, {RomFsExt, Text::ModsPartRomFsExt}, {ExeFs, Text::ModsPartExeFs},
        {Patch, Text::ModsPartPatch}, {Cheats, Text::ModsPartCheats}};
    for (const auto& [part, name] : names) {
        if (!(parts & part)) continue;
        if (!text.empty()) text += L" · ";
        text += Tr(name);
    }
    return text;
}

Text SortText(GameBanana::Sort sort) {
    switch (sort) {
    case GameBanana::Sort::Popular: return Text::ModsSortPopular;
    case GameBanana::Sort::Newest: return Text::ModsSortNewest;
    case GameBanana::Sort::Updated: return Text::ModsSortUpdated;
    case GameBanana::Sort::Liked: return Text::ModsSortLiked;
    }
    return Text::ModsSortPopular;
}

Text SourceText(StoreSource source) {
    return source == StoreSource::Cnx ? Text::ModsSourceCnx : Text::ModsSourceGameBanana;
}

/// The category the store lists, as a path ("Skins › Characters").
std::wstring CategoryLabel(const StoreState& store) {
    if (store.adult) return std::wstring{Tr(Text::ModsCategoryAdult)};
    if (store.category.empty()) return std::wstring{Tr(Text::ModsCategoryAll)};
    std::wstring label;
    for (const auto& category : store.category) {
        if (!label.empty()) label += L" › ";
        label += Utf8ToUtf16(category.name);
    }
    return label;
}

/// "Changes 14 game files: a.szs, b.bars, c.szs…", naming the largest; empty for a mod of patches
/// or cheats only.
std::wstring ChangesLine(const ModContents& contents) {
    const auto count = contents.files.size();
    if (count == 0) return {};
    constexpr std::size_t named = 3;
    std::wstring names;
    for (const auto& name : MainFiles(contents.listing, named)) {
        if (!names.empty()) names += L", ";
        names += Utf8ToUtf16(name);
    }
    if (count > named) names += L"…";
    return I18n::Format(count == 1 ? Text::ModsChangesOne : Text::ModsChangesOther, {std::to_wstring(count), names});
}

std::size_t ActiveCount(const std::vector<InstalledMod>& mods) {
    return static_cast<std::size_t>(
        std::count_if(mods.begin(), mods.end(), [](const InstalledMod& m) { return m.record.enabled; }));
}

/// Conflicts counted once per pair.
std::size_t ConflictCount(const std::vector<InstalledMod>& mods) {
    std::size_t count = 0;
    for (const auto& mod : mods)
        count += static_cast<std::size_t>(
            std::count_if(mod.conflicts.begin(), mod.conflicts.end(), [](const Conflict& c) { return c.wins; }));
    return count;
}

/// A small rounded label over a picture ("18+", "Installed").
void Chip(UiPainter& paint, std::wstring_view text, float x, float y, unsigned background, unsigned color) {
    const float width = paint.MeasureText(text, 13, true) + 16;
    paint.Round(x, y, width, 22, 11, background);
    paint.Text(text, x, y, 13, color, width, 22, true, false, true);
}

void Bar(UiPainter& paint, float x, float y, float width, std::uint64_t done, std::uint64_t total) {
    paint.Round(x, y, width, 6, 3, Palette::Track);
    if (total == 0) return;
    const float filled = width * static_cast<float>(std::min(done, total)) / static_cast<float>(total);
    if (filled > 0) paint.Round(x, y, std::max(filled, 6.0f), 6, 3, Palette::Accent);
}

/// The first download and how the queue goes, in one line; empty when nothing downloads.
std::wstring DownloadLine(const std::vector<DownloadStatus>& downloads) {
    if (downloads.empty()) return {};
    const auto& first = downloads.front();
    std::wstring line = I18n::Format(Text::ModsDownloading, {Utf8ToUtf16(first.title)});
    if (first.running && first.total > 0) {
        line += Dot;
        line += std::to_wstring(std::min<std::uint64_t>(first.done * 100 / first.total, 100)) + L"%";
        if (first.speed > 0) line += Dot + Size(first.speed) + L"/s";
    }
    if (downloads.size() > 1)
        line += Dot + I18n::Plural(Text::ModsQueuedOne, Text::ModsQueuedOther, downloads.size() - 1);
    return line;
}

void DrawTabs(UiPainter& paint, const ModsView& view, float x, float y) {
    paint.Round(x + L::Margin, y - 4, L::Width - 2 * L::Margin, L::TabHeight + 8, 24, Palette::Surface);
    float left = x + L::Margin;
    paint.Badge(view.previous_tab, left + 18, y + L::TabHeight / 2, Palette::Text);
    left += 50;
    const std::wstring labels[ModsTabCount] = {
        std::wstring{Tr(Text::ModsTabInstalled)} + L"  " + std::to_wstring(view.session.View().mods.size()),
        std::wstring{Tr(Text::ModsTabExplore)}, std::wstring{Tr(Text::ModsTabFiles)}};
    for (std::size_t i = 0; i < ModsTabCount; ++i) {
        const bool active = static_cast<std::size_t>(view.page.tab) == i;
        const float width = paint.MeasureType(labels[i], TypeRole::LabelLarge) + 36;
        paint.Round(left, y, width, L::TabHeight, L::TabHeight / 2, active && view.tabs_focused ? Palette::Accent : Palette::Row);
        paint.Type(labels[i], TypeRole::LabelLarge, left, y + 6, width,
                   active && view.tabs_focused ? Palette::OnAccent : active ? Palette::Text : Palette::TextSecondary,
                   1, TextAlign::Center);
        paint.Target(UiTarget::ManagerTab, i, left, y, width, L::TabHeight);
        left += width + 10;
    }
    paint.Badge(view.next_tab, left + 20, y + L::TabHeight / 2, Palette::Text);
    // Downloads keep going on the other tabs: their progress stays in sight.
    const auto downloads = view.session.Downloads();
    if (downloads.empty() && view.page.tab == ModsTab::Installed) {
        const auto& mods = view.session.View().mods;
        auto summary = I18n::Plural(Text::ModsActiveOne, Text::ModsActiveOther, ActiveCount(mods));
        const auto conflicts = ConflictCount(mods);
        if (conflicts) summary += Dot + I18n::Plural(Text::ModsConflictsOne, Text::ModsConflictsOther, conflicts);
        paint.Type(summary, TypeRole::Label, x + L::Width - L::Margin - 360, y + 7,
                   360, conflicts ? Palette::Warning : Palette::TextHint, 1, TextAlign::Trailing);
    }
    if (!downloads.empty() && view.page.tab != ModsTab::Explore) {
        const auto& first = downloads.front();
        std::wstring text = I18n::Plural(Text::ModsDownloadsOne, Text::ModsDownloadsOther, downloads.size());
        if (first.running && first.total > 0)
            text += Dot + std::to_wstring(std::min<std::uint64_t>(first.done * 100 / first.total, 100)) + L"%";
        paint.Text(text, x + L::Width - L::Margin - 300, y + 8, 15, Palette::Accent, 300, 24, false, false, false, true);
    }
}

constexpr float ListHeight = L::VisibleRows * L::RowStride - (L::RowStride - L::RowHeight);

void DrawScrollMarks(UiPainter& paint, std::size_t first, std::size_t count, float x, float y) {
    paint.ScrollIndicator(x + L::Width - L::Margin + 14, y + L::RowTop, ListHeight, first, L::VisibleRows, count);
}

/// The list's first row as drawn: eased between windows while the window moves.
float ListFirst(const ModsView& view) {
    return view.scroll < 0 ? static_cast<float>(view.page.first) : view.scroll;
}

/// Rows glide under the tabs: the list is clipped to its own band.
void BeginList(UiPainter& paint, float x, float y) {
    paint.PushClip(x, y + L::RowTop - 4, L::Width, ListHeight + 8);
}

void DrawMessage(UiPainter& paint, UiIcon icon, std::wstring_view message, float x, float y) {
    const float top = y + L::RowTop + 90;
    paint.Icon(icon, x + L::Width / 2 - 16, top - 50, Palette::TextSecondary);
    paint.Text(message, x + L::Margin + 60, top, 20, Palette::TextHint, RowWidth - 120, 120, false, true, true);
}

std::wstring Origin(const ModSource& source) {
    switch (source.kind) {
    case SourceKind::Local: return std::wstring{Tr(Text::ModsSourceLocal)};
    case SourceKind::Archive: return I18n::Format(Text::ModsSourceArchive, {Utf8ToUtf16(source.archive)});
    case SourceKind::GameBanana: return I18n::Format(Text::ModsSourceGameBananaFile, {Utf8ToUtf16(source.archive)});
    case SourceKind::Cnx: return I18n::Format(Text::ModsSourceCnxFile, {Utf8ToUtf16(source.archive)});
    }
    return {};
}

void DrawInstalled(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (mods.empty()) {
        DrawMessage(paint, UiIcon::Mods, Tr(view.session.View().loaded ? Text::ModsEmpty : Text::ModsLoading), x, y);
        return;
    }
    const auto& page = view.page;
    const float first = ListFirst(view);
    const std::size_t end = std::min(mods.size(), static_cast<std::size_t>(first) + L::VisibleRows + 1);
    BeginList(paint, x, y);
    for (std::size_t i = static_cast<std::size_t>(first); i < end; ++i) {
        const auto& mod = mods[i];
        const float top = y + L::RowTop + (static_cast<float>(i) - first) * L::RowStride;
        const float left = x + L::Margin;
        const bool active = i == page.selected && view.content_focused;
        const bool on = mod.record.enabled;
        // A mod being moved is lifted: an accent rim around its row.
        if (active && page.moving) paint.Round(left - 3, top - 3, RowWidth + 6, L::RowHeight + 6, 15, Palette::Accent);
        paint.SelectionCard(left, top, RowWidth, L::RowHeight, active, 12);
        // Its place in the order: 1 wins over 2.
        paint.Round(left + 16, top + 6, 28, 28, 14, on ? Palette::Track : Palette::Row);
        paint.Text(std::to_wstring(i + 1), left + 16, top + 6, 14, on ? Palette::Text : Palette::TextSecondary,
                   28, 28, true, false, true);
        paint.Text(Utf8ToUtf16(mod.record.name), left + 60, top + 2, 17,
                   on || active ? Palette::Text : Palette::TextHint, RowWidth - 170, 24, active);
        std::wstring detail = on ? Parts(mod.contents.parts) : std::wstring{Tr(Text::ModsOff)};
        const float detail_width = paint.MeasureText(detail, 14, false);
        paint.Text(detail, left + 60, top + 24, 12, Palette::TextSecondary, RowWidth - 170, 16);
        // What needs the player's eye, after the parts: a problem first, then an update.
        std::wstring flag;
        unsigned flag_color = Palette::Warning;
        if (on && mod.compat == Compat::OtherVersion) flag = Tr(Text::ModsOtherVersion);
        else if (on && !mod.conflicts.empty())
            flag = I18n::Plural(Text::ModsConflictsOne, Text::ModsConflictsOther, mod.conflicts.size());
        else if (view.session.HasUpdate(i)) {
            flag = Tr(Text::ModsUpdateAvailable);
            flag_color = Palette::Accent;
        }
        if (!flag.empty())
            paint.Text(L"·  " + flag, left + 68 + detail_width, top + 24, 12, flag_color,
                       RowWidth - 180 - detail_width, 16);
        paint.Switch(left + RowWidth - 76, top + 6, on, active);
        paint.Target(UiTarget::ManagerRow, i, left, top, RowWidth, L::RowHeight);
    }
    paint.PopClip();
    DrawScrollMarks(paint, page.first, mods.size(), x, y);
}

/// Under the Installed list: the selected mod in plain words.
void DrawInstalledDetail(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    const float left = x + L::Margin;
    const float top = y + L::Detail;
    if (view.page.moving) {
        paint.Text(Tr(Text::ModsMovingHint), left, top, 14, Palette::Accent, RowWidth, 20);
        return;
    }
    if (view.page.selected >= mods.size()) return;
    const auto& mod = mods[view.page.selected];
    std::wstring line;
    unsigned color = Palette::TextHint;
    if (mod.record.enabled && mod.compat == Compat::OtherVersion) {
        line = Tr(Text::ModsOtherVersion);
        color = Palette::Warning;
    } else if (mod.record.enabled && !mod.conflicts.empty()) {
        // The heaviest conflict first; one line is room for two.
        auto conflicts = mod.conflicts;
        std::sort(conflicts.begin(), conflicts.end(), [](const Conflict& a, const Conflict& b) { return a.files > b.files; });
        for (std::size_t i = 0; i < std::min<std::size_t>(conflicts.size(), 2); ++i) {
            const auto& c = conflicts[i];
            if (!line.empty()) line += Dot;
            line += I18n::Format(c.wins ? Text::ModsWins : Text::ModsLoses,
                                 {Utf8ToUtf16(mods[c.other].record.name), std::to_wstring(c.files)});
            if (!c.wins) color = Palette::Warning;
        }
    } else if (mod.record.enabled) {
        line = Tr(mod.compat == Compat::Matches ? Text::ModsForThisVersion : Text::ModsNoConflicts);
        color = Palette::Accent;
    }
    // Then what it changes, which the conflicts already speak of when there are some.
    if (const auto changes = ChangesLine(mod.contents); !changes.empty() && (line.empty() || color == Palette::Accent)) {
        const float status = line.empty() ? 0 : paint.MeasureText(line + Dot, 14, false);
        if (!line.empty()) paint.Text(line + Dot, left, top, 14, color, RowWidth, 20);
        paint.Text(changes, left + status, top, 14, Palette::TextHint, RowWidth - status, 20);
    } else {
        paint.Text(line, left, top, 14, color, RowWidth, 20);
    }
    std::wstring origin = Origin(mod.record.source);
    if (!mod.record.source.version.empty() && mod.record.source.kind == SourceKind::GameBanana)
        origin += Dot + I18n::Format(Text::ModsVersion, {Utf8ToUtf16(mod.record.source.version)});
    origin += Dot + Size(mod.contents.bytes);
    const bool notice = !view.notice.empty();
    paint.Text(notice ? std::wstring{view.notice} : origin + std::wstring{Dot} + std::wstring{Tr(Text::ModsAppliesAtBoot)},
               left, top + 20, 13, notice ? Palette::Warning : Palette::TextSecondary, RowWidth, 18);
}

void DrawFiles(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& snapshot = view.session.View();
    const auto& rows = snapshot.files;
    if (rows.empty()) {
        DrawMessage(paint, UiIcon::Archive, Tr(Text::ModsFolderEmpty), x, y);
    } else {
        const auto& page = view.page;
        const float first = ListFirst(view);
        const std::size_t end = std::min(rows.size(), static_cast<std::size_t>(first) + L::VisibleRows + 1);
        BeginList(paint, x, y);
        for (std::size_t i = static_cast<std::size_t>(first); i < end; ++i) {
            const auto& row = rows[i];
            const float top = y + L::RowTop + (static_cast<float>(i) - first) * L::RowStride;
            const bool active = i == page.selected && view.content_focused;
            const UiIcon icon = row.kind == FileRow::Kind::Archive ? UiIcon::Archive : UiIcon::Folder;
            paint.Target(UiTarget::ManagerRow, i, x + L::Margin, top, RowWidth, L::RowHeight);
            paint.SelectionCard(x + L::Margin, top, RowWidth, L::RowHeight, active, 12);
            paint.Icon(icon, x + L::Margin + 20, top + (L::RowHeight - 32) / 2, Palette::TextSecondary);
            const unsigned color = row.kind == FileRow::Kind::AddFolder ? Palette::Accent : Palette::Text;
            paint.Type(row.name, TypeRole::BodyLarge, x + L::Margin + 68, top + 7, RowWidth - 120, color);
            if (row.kind != FileRow::Kind::Archive)
                paint.Chevron(x + L::Margin + RowWidth - 28, top + L::RowHeight / 2,
                              active ? Palette::Accent : Palette::TextSecondary);
        }
        paint.PopClip();
        DrawScrollMarks(paint, page.first, rows.size(), x, y);
    }

}

// Explore.

constexpr float RowPitch = L::CardHeight + L::CardGapY;

float CardX(std::size_t column, float x) {
    return x + L::Margin + (RowWidth - L::Columns * L::CardWidth - (L::Columns - 1) * L::CardGapX) / 2 +
           static_cast<float>(column) * (L::CardWidth + L::CardGapX);
}

/// `row` counts from the first row shown, which may be part-way scrolled.
float CardY(float row, float y) {
    return y + L::GridTop + row * RowPitch;
}

/// The store's picture for `url` in the rectangle, or a tile with `icon` while it downloads.
void DrawPicture(UiPainter& paint, const ModSession& session, const std::string& url, UiIcon icon,
                 float x, float y, float w, float h) {
    const auto file = url.empty() ? std::filesystem::path{} : session.Pictures().Get(url);
    if (!file.empty() && paint.Picture(file, x, y, w, h)) return;
    paint.Round(x, y, w, h, 12, Palette::Track);
    paint.Icon(icon, x + w / 2 - 16, y + h / 2 - 16, Palette::TextSecondary);
}

/// A label that stays legible over any picture: dark, or white for what stands out.
float OverlayChip(UiPainter& paint, std::wstring_view text, float x, float y, bool light = false) {
    const float width = paint.MeasureType(text, TypeRole::Label) + 20;
    paint.Round(x, y, width, 24, 12, light ? Palette::Text : 0x000000, light ? 1.0f : 0.62f);
    paint.Type(text, TypeRole::Label, x + 10, y + 4, width - 18, light ? Palette::OnAccent : Palette::Text);
    return width;
}

/// A mod as the store shows it: the picture with its numbers over the bottom edge, the name,
/// who made it and its category; the focused card offers its button.
void DrawCard(UiPainter& paint, const ModsView& view, const StoreCard& card, bool active, bool installed,
              float x, float y) {
    constexpr float w = L::CardWidth, h = L::CardHeight, ph = L::PictureHeight, pad = 16;
    const unsigned surface = active ? Palette::RowActive : Palette::Surface;
    paint.Round(x, y, w, h, 16, surface);
    const UiIcon icon = card.translation ? UiIcon::Language : UiIcon::Mods;
    DrawPicture(paint, view.session, card.thumbnail, icon, x, y, w, ph + 16);
    // The text sits on the card itself: square off the picture's lower corners under it.
    paint.Round(x, y + ph, w, h - ph, 16, surface);
    paint.Round(x, y + ph, w, 16, 0, surface);
    const bool gamebanana = view.session.Store().source == StoreSource::GameBanana;
    if (gamebanana) {
        // A soft shade under the numbers over the picture.
        paint.Round(x, y + ph - 44, w, 44, 0, 0x000000, 0.18f);
        paint.Round(x, y + ph - 28, w, 28, 0, 0x000000, 0.28f);
        paint.Type(L"♥ " + Count(card.likes), TypeRole::LabelLarge, x + pad, y + ph - 24, w / 2 - pad,
                   Palette::Text);
        paint.Type(I18n::Format(Text::ModsViews, {Count(card.views)}), TypeRole::LabelLarge, x + w / 2,
                   y + ph - 24, w / 2 - pad, Palette::Text, 1, TextAlign::Trailing);
    }
    if (card.sensitive) OverlayChip(paint, Tr(Text::ModsSensitive), x + 10, y + 10);
    if (installed) {
        const auto label = Tr(Text::ModsInstalledBadge);
        OverlayChip(paint, label, x + w - 30 - paint.MeasureType(label, TypeRole::Label), y + 10, true);
    }
    paint.Type(Utf8ToUtf16(card.name), TypeRole::HeadlineSmall, x + pad, y + ph + 12, w - 2 * pad, Palette::Text);
    std::wstring by;
    if (gamebanana) {
        if (!card.author.empty()) by = I18n::Format(Text::ModsBy, {Utf8ToUtf16(card.author)});
    } else {
        by = std::wstring{Tr(card.translation ? Text::ModsCnxTranslation : Text::ModsCnxModification)};
    }
    paint.Type(by, TypeRole::Label, x + pad, y + ph + 46, w - 2 * pad, Palette::TextSecondary);
    const std::wstring kind = gamebanana ? Utf8ToUtf16(card.category) : Utf8ToUtf16(card.size);
    const float row = y + ph + 70;
    float kind_width = w - 2 * pad;
    if (active) {
        const auto open = Tr(Text::ModsPromptOpen);
        const float pill = paint.PillWidth(open, view.confirm);
        paint.PillButton(open, view.confirm, view.confirm_color, x + w - pad - pill, row, true, 30);
        kind_width -= pill + 12;
    }
    paint.Type(kind, TypeRole::LabelLarge, x + pad, row + 6, kind_width, Palette::TextHint);
    if (active) paint.Outline(x + 0.5f, y + 0.5f, w - 1, h - 1, 16, Palette::Text, 0.9f, 2);
}

void DrawExplore(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const auto& page = view.page;
    if (store.cards.empty()) {
        switch (store.status) {
        case StoreStatus::Offline:
            DrawMessage(paint, UiIcon::Mods, I18n::Format(Text::ModsStoreOffline, {Tr(SourceText(store.source)), view.confirm}), x, y + 40);
            return;
        case StoreStatus::Empty:
            DrawMessage(paint, UiIcon::Mods,
                        Tr(store.search.empty() && store.category.empty() && !store.adult ? Text::ModsStoreEmpty
                                                                                           : Text::ModsStoreNoResults),
                        x, y + 40);
            return;
        case StoreStatus::NoGame:
            DrawMessage(paint, UiIcon::Game, I18n::Format(Text::ModsStoreNoGame, {view.game, view.confirm}), x, y + 40);
            return;
        default:
            // Cards to come: their outlines, so the grid does not jump when they arrive.
            for (std::size_t i = 0; i < L::Columns * 2; ++i) {
                const float cx = CardX(i % L::Columns, x), cy = CardY(static_cast<float>(i / L::Columns), y);
                paint.Round(cx, cy, L::CardWidth, L::CardHeight, 16, Palette::Surface);
                paint.Round(cx, cy, L::CardWidth, L::PictureHeight, 16, Palette::Track);
                paint.Round(cx + 16, cy + L::PictureHeight + 18, L::CardWidth * 0.6f, 14, 7, Palette::Track);
                paint.Round(cx + 16, cy + L::PictureHeight + 46, L::CardWidth * 0.35f, 10, 5, Palette::Track);
            }
            return;
        }
    }
    // Mods of this game already installed from the store wear a badge.
    std::set<std::int64_t> installed;
    for (const auto& mod : view.session.View().mods)
        if (mod.record.source.kind == SourceKind::GameBanana) installed.insert(mod.record.source.mod_id);
    const bool gamebanana = store.source == StoreSource::GameBanana;
    const float first = view.grid_scroll < 0 ? static_cast<float>(page.card_first / L::Columns) : view.grid_scroll;
    const std::size_t rows = (store.cards.size() + L::Columns - 1) / L::Columns;
    const std::size_t begin = static_cast<std::size_t>(first);
    // The rows glide under the filters; the next row peeks under the focused one.
    paint.PushClip(x, y + L::GridTop - 6, L::Width, L::Height - L::GridTop + 6);
    for (std::size_t row = begin; row < std::min(rows, begin + 3); ++row) {
        const float top = CardY(static_cast<float>(row) - first, y);
        for (std::size_t column = 0; column < L::Columns; ++column) {
            const std::size_t i = row * L::Columns + column;
            if (i >= store.cards.size()) break;
            const auto& card = store.cards[i];
            paint.Target(UiTarget::ManagerCard, i, CardX(column, x), top, L::CardWidth, L::CardHeight);
            DrawCard(paint, view, card, i == page.card && view.content_focused,
                     gamebanana && installed.contains(card.id), CardX(column, x), top);
        }
    }
    paint.PopClip();
    // The whole height the grid shows, down to the footer.
    paint.ScrollIndicator(x + L::Width - L::Margin + 14, y + L::GridTop, view.grid_bottom - 8 - L::GridTop,
                          page.card_first / L::Columns, 1, rows, store.loading_more);
}

/// Over the grid: what the store lists (category, order, search) and where it comes from.
void DrawExploreFilters(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const float left = x + L::Margin, top = y + L::FilterTop + 2;
    float chip = left;
    if (store.source == StoreSource::GameBanana) {
        const bool filtered = store.adult || !store.category.empty();
        chip += paint.Chip(CategoryLabel(store), chip, top, filtered ? Palette::Text : Palette::TextHint) + 8;
        chip += paint.Chip(Tr(SortText(store.sort)), chip, top, Palette::TextHint) + 8;
    }
    if (!store.search.empty())
        chip += paint.Chip(L"“" + Utf8ToUtf16(store.search) + L"”", chip, top, Palette::Text) + 8;
    const float right = left + RowWidth;
    const auto downloads = view.session.Downloads();
    if (!downloads.empty()) {
        // The downloads, with the first one's bar at the end of the line.
        constexpr float bar = 160;
        const auto& first = downloads.front();
        Bar(paint, right - bar, top + 11, bar, first.done, first.total);
        paint.Type(DownloadLine(downloads), TypeRole::Label, chip + 16, top + 6, right - bar - 16 - chip - 16,
                   Palette::Text, 1, TextAlign::Trailing);
        return;
    }
    std::wstring line;
    if (!view.notice.empty()) {
        line = view.notice;
    } else {
        line = Tr(SourceText(store.source));
        if (store.source == StoreSource::GameBanana && store.game) line += Dot + Utf8ToUtf16(store.game->name);
        if (store.hidden) line += Dot + I18n::Plural(Text::ModsHiddenOne, Text::ModsHiddenOther, store.hidden);
        if (store.loading_more) line += Dot + std::wstring{Tr(Text::ModsLoadingMore)};
    }
    paint.Type(line, TypeRole::Label, chip + 16, top + 6, right - chip - 16,
               view.notice.empty() ? Palette::TextSecondary : Palette::Warning, 1, TextAlign::Trailing);
}

/// A card over the dimmed page.
void Sheet(UiPainter& paint, float x, float y, float width, float height) {
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Canvas);
    paint.Round(x + (L::Width - width) / 2, y + std::max(104.0f, (L::Height - height) / 2), width, height, 20, Palette::Panel);
}

void DrawDetail(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    paint.ManagementSurface(x, y, L::Width, L::Height, L::Footer);
    if (!store.detail) {
        const auto text = store.detail_failed ? I18n::Format(Text::ModsStoreOffline, {Tr(Text::ModsSourceGameBanana), view.confirm})
                                              : std::wstring{Tr(Text::ModsDetailLoading)};
        paint.Icon(UiIcon::Mods, x + L::Width / 2 - 16, y + 240, Palette::TextSecondary);
        paint.Text(text, x + L::Margin, y + 290, 20, Palette::TextHint, RowWidth, 60, false, true, true);
        return;
    }
    const auto& detail = *store.detail;
    const auto& page = view.page;
    // Left: the pictures and the numbers.
    const float left = x + L::Margin;
    const float top = y + 108;
    constexpr float picture_w = 480, picture_h = 270;
    const auto& images = detail.images;
    const std::string url = images.empty() ? std::string{} : images[std::min(page.image, images.size() - 1)];
    DrawPicture(paint, view.session, url, detail.card.translation ? UiIcon::Language : UiIcon::Mods,
                left, top, picture_w, picture_h);
    if (images.size() > 1) {
        const auto count = std::to_wstring(std::min(page.image, images.size() - 1) + 1) + L" / " +
                           std::to_wstring(images.size());
        paint.ButtonGlyph(view.previous_tab, left, top + picture_h + 10, ButtonColor::Neutral);
        paint.Text(count, left + 44, top + picture_h + 12, 16, Palette::TextSecondary, picture_w - 88, 28, false,
                   false, true);
        paint.ButtonGlyph(view.next_tab, left + picture_w - 38, top + picture_h + 10, ButtonColor::Neutral);
    }
    float line_y = top + picture_h + 52;
    const auto stat_line = [&](const std::wstring& text, unsigned color) {
        if (text.empty()) return;
        paint.Text(text, left, line_y, 15, color, picture_w, 24);
        line_y += 26;
    };
    if (detail.source == StoreSource::GameBanana) {
        stat_line(L"♥ " + Count(detail.card.likes) + Dot +
                      I18n::Format(Text::ModsDownloadsCount, {Count(detail.downloads)}) + Dot +
                      I18n::Format(Text::ModsViews, {Count(detail.card.views)}),
                  Palette::Text);
        std::wstring by;
        if (!detail.version.empty()) by = I18n::Format(Text::ModsVersion, {Utf8ToUtf16(detail.version)});
        if (!detail.card.author.empty()) {
            if (!by.empty()) by += Dot;
            by += I18n::Format(Text::ModsBy, {Utf8ToUtf16(detail.card.author)});
        }
        stat_line(by, Palette::TextSecondary);
        stat_line(Utf8ToUtf16(detail.card.category), Palette::TextSecondary);
    } else {
        stat_line(std::wstring{Tr(detail.card.translation ? Text::ModsCnxTranslation : Text::ModsCnxModification)} +
                      Dot + Utf8ToUtf16(detail.card.size),
                  Palette::Text);
    }
    // Right: the name, the description and the files.
    const float right = x + 560;
    const float right_w = L::Width - 560 - L::Margin;
    paint.Text(Utf8ToUtf16(detail.card.name), right, top - 4, 24, Palette::Text, right_w, 64, true, true);
    float text_top = top + 64;
    if (!store.replace.empty()) {
        paint.Text(Tr(Text::ModsUpdateBanner), right, text_top, 15, Palette::Accent, right_w, 44, false, true);
        text_top += 48;
    }
    const std::wstring text = detail.text.empty() ? std::wstring{Tr(Text::ModsCnxDescription)} : Utf8ToUtf16(detail.text);
    paint.Text(text, right, text_top, 15, Palette::TextHint, right_w, top + 286 - text_top, false, true);
    paint.Text(Tr(Text::ModsFiles), right, top + 296, 17, Palette::Text, right_w, 26, true);
    const auto downloads = view.session.Downloads();
    constexpr std::size_t visible = 4;
    const std::size_t first = page.file >= visible ? page.file + 1 - visible : 0;
    const std::size_t end = std::min(detail.files.size(), first + visible);
    for (std::size_t i = first; i < end; ++i) {
        const auto& file = detail.files[i];
        const float row = top + 330 + static_cast<float>(i - first) * 56;
        const bool active = i == page.file;
        paint.Round(right, row, right_w, 50, 10, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(right, row + 12, 3, 26, 1.5f, Palette::Accent);
        paint.Text(Utf8ToUtf16(file.name), right + 14, row + 4, 16, Palette::Text, right_w - 28, 24, active);
        std::wstring meta;
        unsigned color = Palette::TextSecondary;
        const auto title = detail.card.name + " \xC2\xB7 " + file.name;
        const auto running = std::find_if(downloads.begin(), downloads.end(),
                                          [&](const DownloadStatus& d) { return d.title == title; });
        if (running != downloads.end()) {
            meta = running->running && running->total
                       ? I18n::Format(Text::ModsDownloadingFile,
                                      {std::to_wstring(std::min<std::uint64_t>(running->done * 100 / running->total, 100)) + L"%"})
                       : std::wstring{Tr(Text::ModsQueuedFile)};
            color = Palette::Accent;
        } else if (!file.safe) {
            meta = Tr(Text::ModsFileUnsafe);
            color = Palette::Warning;
        } else {
            meta = file.size ? Size(file.size) : std::wstring{};
            if (const auto date = Date(file.added); !date.empty()) meta += (meta.empty() ? L"" : std::wstring{Dot}) + date;
            if (file.downloads > 0)
                meta += Dot + I18n::Format(Text::ModsDownloadsCount, {Count(file.downloads)});
            if (!file.description.empty()) meta += Dot + Utf8ToUtf16(file.description);
        }
        paint.Text(meta, right + 14, row + 27, 13, color, right_w - 28, 20);
    }
    paint.ScrollIndicator(right + right_w + 14, top + 330, visible * 56 - 6,
                          first, visible, detail.files.size());
    if (detail.files.empty())
        paint.Text(Tr(Text::ModsNoFiles), right, top + 336, 15, Palette::TextHint, right_w, 24);
}

void DrawStoreMenu(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const auto downloads = view.session.Downloads();
    const auto actions = StoreActions({store.source == StoreSource::GameBanana, !store.search.empty(), !downloads.empty()});
    constexpr float width = 700;
    const float height = 90 + static_cast<float>(actions.size()) * 62;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 24;
    const float inner = width - 60;
    paint.Text(Tr(Text::ModsStoreMenuTitle), left, top, 22, Palette::Text, inner, 32, true);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const float row = top + 50 + static_cast<float>(i) * 62;
        const bool active = i == view.page.menu;
        paint.Round(left, row, inner, 54, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, row + 14, 3, 26, 1.5f, Palette::Accent);
        Text label{};
        std::wstring value;
        switch (actions[i]) {
        case StoreAction::Category:
            label = Text::ModsStoreCategory;
            value = CategoryLabel(store);
            break;
        case StoreAction::Sort:
            label = Text::ModsStoreSort;
            value = Tr(SortText(store.sort));
            break;
        case StoreAction::Search:
            label = Text::ModsStoreSearch;
            value = store.search.empty() ? L"—" : Utf8ToUtf16(store.search);
            break;
        case StoreAction::ClearSearch:
            label = Text::ModsStoreClearSearch;
            break;
        case StoreAction::Source:
            label = Text::ModsStoreSource;
            value = Tr(SourceText(store.source));
            break;
        case StoreAction::ChangeGame:
            label = Text::ModsStoreGame;
            value = store.game ? Utf8ToUtf16(store.game->name) : L"—";
            break;
        case StoreAction::CancelDownloads:
            label = Text::ModsStoreCancel;
            value = std::to_wstring(downloads.size());
            break;
        }
        paint.Text(Tr(label), left + 20, row + 13, 19, Palette::Text, inner / 2, 28, active);
        paint.Text(value, left + inner / 2, row + 15, 17, active ? Palette::Accent : Palette::TextSecondary,
                   inner / 2 - 20, 26, false, false, false, true);
    }
}

void DrawKeyboard(UiPainter& paint, const ModsView& view, float x, float y) {
    if (!view.page.keyboard) return;
    using K = TextEntryPanelLayout;
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Canvas);
    const float left = x + (L::Width - K::Width) / 2;
    paint.Round(left, y + 104, K::Width, K::Height, 22, Palette::Surface);
    const bool search = view.page.keyboard_for == KeyboardFor::Search;
    DrawTextEntryPanel(paint, {*view.page.keyboard, Tr(search ? Text::ModsSearchTitle : Text::ModsGameTitle),
                               Tr(search ? Text::ModsSearchHint : Text::ModsGameHint), {}},
                       left, y + 104);
}

void DrawGames(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    constexpr float width = 760, height = 520;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 24;
    const float inner = width - 60;
    paint.Text(Tr(Text::ModsGamesTitle), left, top, 22, Palette::Text, inner, 32, true);
    if (store.searching_games || store.games.empty()) {
        paint.Text(Tr(store.searching_games ? Text::ModsGamesSearching : Text::ModsGamesNone), left, top + 180, 18,
                   Palette::TextHint, inner, 60, false, true, true);
        return;
    }
    constexpr std::size_t visible = 6;
    const std::size_t first = view.page.game >= visible ? view.page.game + 1 - visible : 0;
    const std::size_t end = std::min(store.games.size(), first + visible);
    for (std::size_t i = first; i < end; ++i) {
        const float row = top + 52 + static_cast<float>(i - first) * 68;
        paint.RowFrame(left, row, inner, 60, i == view.page.game, UiIcon::Game);
        paint.Text(Utf8ToUtf16(store.games[i].name), left + 68, row + 16, 19, Palette::Text, inner - 90, 28,
                   i == view.page.game);
    }
    paint.ScrollIndicator(left + inner + 14, top + 52, visible * 68 - 8,
                          first, visible, store.games.size());
}

void DrawActions(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (view.page.selected >= mods.size()) return;
    const auto actions = InstalledActions(view.session.HasUpdate(view.page.selected));
    constexpr float width = 560;
    const float height = 122 + static_cast<float>(actions.size()) * 64;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 26;
    paint.Text(Utf8ToUtf16(mods[view.page.selected].record.name), left, top, 22, Palette::Text, width - 60, 32, true);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const float row = top + 56 + static_cast<float>(i) * 64;
        const bool active = i == view.page.action;
        Text label{};
        UiIcon icon{};
        switch (actions[i]) {
        case ModAction::Contents: label = Text::ModsActionContents; icon = UiIcon::Folder; break;
        case ModAction::Update: label = Text::ModsActionUpdate; icon = UiIcon::Archive; break;
        case ModAction::Move: label = Text::ModsActionMove; icon = UiIcon::Mods; break;
        case ModAction::Remove: label = Text::ModsActionRemove; icon = UiIcon::Remove; break;
        }
        paint.RowFrame(left, row, width - 60, 54, active, icon);
        paint.Text(Tr(label), left + 68, row + 13, 19, Palette::Text, width - 140, 28, active);
    }
}

/// Every file of the selected mod, with its size; the largest stand out.
void DrawContents(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (view.page.selected >= mods.size()) return;
    const auto& mod = mods[view.page.selected];
    const auto& listing = mod.contents.listing;
    constexpr float width = 900, height = 600;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 24;
    const float inner = width - 60;
    paint.Text(Utf8ToUtf16(mod.record.name), left, top, 22, Palette::Text, inner, 32, true);
    paint.Text(I18n::Plural(Text::ModsFilesOne, Text::ModsFilesOther, listing.size()) + Dot + Size(mod.contents.bytes),
               left, top + 36, 15, Palette::TextSecondary, inner, 22);
    paint.Text(Tr(Text::ModsContentsHint), left, top + 60, 14, Palette::TextHint, inner, 40, false, true);
    const std::size_t first = std::min(view.page.line, listing.size());
    const std::size_t end = std::min(listing.size(), first + L::SheetRows);
    for (std::size_t i = first; i < end; ++i) {
        const auto& file = listing[i];
        const float row = top + 108 + static_cast<float>(i - first) * 50;
        paint.Round(left, row, inner, 44, 10, Palette::Row);
        const auto slash = file.path.rfind('/');
        const std::wstring folder = slash == std::string::npos ? L"" : Utf8ToUtf16(file.path.substr(0, slash + 1));
        const float folder_w = folder.empty() ? 0 : paint.MeasureText(folder, 16, false);
        if (!folder.empty()) paint.Text(folder, left + 16, row + 10, 16, Palette::TextSecondary, inner - 150, 24);
        paint.Text(Utf8ToUtf16(file.path.substr(slash == std::string::npos ? 0 : slash + 1)), left + 16 + folder_w,
                   row + 10, 16, Palette::Text, std::max(inner - 150 - folder_w, 60.0f), 24, true);
        paint.Text(Size(file.bytes), left + inner - 130, row + 11, 15, Palette::TextSecondary, 114, 22, false, false,
                   false, true);
    }
    paint.ScrollIndicator(left + inner + 14, top + 108, L::SheetRows * 50 - 6,
                          first, L::SheetRows, listing.size());
}

/// The store's categories at the level browsed; the one listed is marked.
void DrawCategories(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const auto rows = view.session.CategoryRows();
    constexpr float width = 760, height = 600;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 24;
    const float inner = width - 60;
    std::wstring title{Tr(Text::ModsCategoriesTitle)};
    for (const auto& category : store.browse) title += L" › " + Utf8ToUtf16(category.name);
    paint.Text(title, left, top, 22, Palette::Text, inner, 32, true);
    if (rows.empty()) {
        const auto text = store.categories_failed
                              ? I18n::Format(Text::ModsStoreOffline, {Tr(Text::ModsSourceGameBanana), view.confirm})
                              : std::wstring{Tr(Text::ModsCategoriesLoading)};
        paint.Text(text, left, top + 220, 18, Palette::TextHint, inner, 60, false, true, true);
        return;
    }
    const auto& page = view.page;
    const std::size_t cursor = std::min(page.category, rows.size() - 1);
    const std::size_t first = cursor >= L::SheetRows ? cursor + 1 - L::SheetRows : 0;
    const std::size_t end = std::min(rows.size(), first + L::SheetRows);
    for (std::size_t i = first; i < end; ++i) {
        const auto& row = rows[i];
        const float row_y = top + 52 + static_cast<float>(i - first) * 70;
        const bool active = i == cursor;
        paint.Round(left, row_y, inner, 62, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, row_y + 16, 3, 30, 1.5f, Palette::Accent);
        std::wstring name, meta;
        bool nested = false, shown = false;
        switch (row.kind) {
        case CategoryRow::Kind::All:
            name = store.browse.empty() ? std::wstring{Tr(Text::ModsCategoryAll)}
                                        : I18n::Format(Text::ModsCategoryAllOf, {Utf8ToUtf16(store.browse.back().name)});
            shown = !store.adult && store.category.size() == store.browse.size() &&
                    std::equal(store.browse.begin(), store.browse.end(), store.category.begin(),
                               [](const auto& a, const auto& b) { return a.id == b.id; });
            break;
        case CategoryRow::Kind::Adult:
            name = Tr(Text::ModsCategoryAdult);
            meta = Tr(Text::ModsCategoryAdultHint);
            shown = store.adult;
            break;
        case CategoryRow::Kind::Category: {
            const auto& category = store.categories[row.index];
            name = Utf8ToUtf16(category.name);
            meta = I18n::Plural(Text::ModsModsOne, Text::ModsModsOther, static_cast<std::uint64_t>(category.mods));
            nested = category.nested;
            shown = !store.adult && !store.category.empty() && store.category.back().id == category.id;
            break;
        }
        }
        float text_left = left + 20;
        if (row.kind == CategoryRow::Kind::Adult) {
            Chip(paint, Tr(Text::ModsSensitive), left + 18, row_y + 20, Palette::Warning, Palette::OnAccent);
            text_left += paint.MeasureText(Tr(Text::ModsSensitive), 13, true) + 28;
        }
        paint.Text(name, text_left, row_y + (meta.empty() ? 17 : 6), 19, shown ? Palette::Accent : Palette::Text,
                   inner - (text_left - left) - 60, 28, active || shown);
        if (!meta.empty())
            paint.Text(meta, text_left, row_y + 34, 14, Palette::TextSecondary, inner - (text_left - left) - 60, 22);
        if (shown) paint.Text(L"✓", left + inner - 48, row_y + 17, 20, Palette::Accent, 28, 28, true, false, true);
        else if (nested) paint.Chevron(left + inner - 30, row_y + 31, active ? Palette::Accent : Palette::TextSecondary);
    }
    paint.ScrollIndicator(left + inner + 14, top + 52, L::SheetRows * 70 - 8,
                          first, L::SheetRows, rows.size());
}

void DrawRemove(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (view.page.selected >= mods.size()) return;
    constexpr float width = 640, height = 180;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 30;
    paint.Icon(UiIcon::Remove, left, top + 6, Palette::Warning);
    paint.Text(I18n::Format(Text::ModsRemoveConfirm, {Utf8ToUtf16(mods[view.page.selected].record.name)}),
               left + 48, top, 20, Palette::Text, width - 110, 120, false, true);
}

void DrawVariants(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& snapshot = view.session.View();
    const auto& candidates = snapshot.candidates;
    const bool several = candidates.size() > 1;
    constexpr float width = 860, height = 540;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + std::max(104.0f, (L::Height - height) / 2) + 24;
    const float inner = width - 60;
    paint.Icon(UiIcon::Archive, left, top + 2, Palette::Accent);
    paint.Text(snapshot.archive, left + 46, top, 22, Palette::Text, inner - 46, 34, true);
    // A save replaces the player's: the sheet says so before anything else.
    const bool save = !candidates.empty() && candidates.front().save;
    paint.Text(Tr(save ? Text::ModsSaveHint : several ? Text::ModsVariantsHint : Text::ModsInstallOneHint), left,
               top + 42, 16, save ? Palette::Warning : Palette::TextHint, inner, 26);
    constexpr std::size_t visible = 5;
    const std::size_t first = view.page.variant >= visible ? view.page.variant + 1 - visible : 0;
    const std::size_t end = std::min(candidates.size(), first + visible);
    for (std::size_t i = first; i < end; ++i) {
        const auto& candidate = candidates[i];
        const float row = top + 84 + static_cast<float>(i - first) * 76;
        const bool active = several && i == view.page.variant;
        const bool ticked = i < view.page.chosen.size() && view.page.chosen[i];
        paint.Round(left, row, inner, 68, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, row + 14, 3, 40, 1.5f, Palette::Accent);
        float text_left = left + 20;
        if (several) {
            // A tick box: filled when the variant installs.
            paint.Round(left + 18, row + 20, 28, 28, 7, ticked ? Palette::Accent : Palette::Track);
            if (ticked) paint.Text(L"✓", left + 18, row + 20, 18, Palette::OnAccent, 28, 28, true, false, true);
            text_left = left + 62;
        }
        paint.Text(Utf8ToUtf16(candidate.name), text_left, row + 8, 19, Palette::Text, inner - 90, 28, active);
        std::wstring detail = (candidate.save ? std::wstring{Tr(Text::ModsPartSave)} : Parts(candidate.parts)) +
                              std::wstring{Dot} + Size(candidate.bytes);
        paint.Text(detail, text_left, row + 38, 14, Palette::TextSecondary, inner - 90, 22);
        if (candidate.other_title)
            paint.Text(Tr(Text::ModsOtherTitle), text_left + paint.MeasureText(detail, 14, false) + 12, row + 38, 14,
                       Palette::Warning, inner - 100 - paint.MeasureText(detail, 14, false), 22);
    }
    paint.ScrollIndicator(left + inner + 14, top + 84, visible * 76 - 8,
                          first, visible, candidates.size());
}

/// The running job, over the list: what it does and, while installing, how far it is.
void DrawBusy(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& session = view.session;
    const float left = x + L::Margin;
    const float top = y + L::RowTop;
    paint.Round(left, top, RowWidth, L::RowStride * L::VisibleRows - 6, 16, Palette::Panel);
    std::wstring text;
    switch (session.Running()) {
    case ModJob::Installing:
        text = I18n::Format(Text::ModsInstalling, {Size(session.Done()), Size(session.Total())});
        break;
    case ModJob::Reading: text = Tr(Text::ModsReading); break;
    case ModJob::Browsing: text = Tr(Text::ModsBrowsing); break;
    case ModJob::Removing: text = Tr(Text::ModsRemoving); break;
    default: text = Tr(Text::ModsLoading); break;
    }
    const float middle = top + 120;
    paint.Icon(session.Running() == ModJob::Installing ? UiIcon::Archive : UiIcon::Mods, x + L::Width / 2 - 16,
               middle - 56, Palette::Accent);
    paint.Text(text, left, middle, 21, Palette::Text, RowWidth, 32, true, false, true);
    if (session.Running() == ModJob::Installing && !session.View().archive.empty())
        paint.Text(session.View().archive, left, middle + 34, 16, Palette::TextSecondary, RowWidth, 24, false, false, true);
    if (session.Running() == ModJob::Installing && session.Total() > 0) {
        constexpr float bar = 520;
        Bar(paint, x + (L::Width - bar) / 2, middle + 70, bar, session.Done(), session.Total());
    }
}

} // namespace

I18n::Text ErrorText(ModError error) {
    switch (error) {
    case ModError::NotAMod: return Text::ModsErrorNotAMod;
    case ModError::Encrypted: return Text::ModsErrorEncrypted;
    case ModError::Unreadable: return Text::ModsErrorUnreadable;
    case ModError::TooLarge: return Text::ModsErrorTooLarge;
    case ModError::Space: return Text::ModsErrorSpace;
    case ModError::Write: return Text::ModsErrorWrite;
    case ModError::Cancelled: return Text::ModsErrorCancelled;
    case ModError::Unavailable: return Text::ModsErrorUnavailable;
    case ModError::Network: return Text::ModsErrorNetwork;
    case ModError::Checksum: return Text::ModsErrorChecksum;
    case ModError::NeedsSkyline: return Text::ModsErrorNeedsSkyline;
    case ModError::NeedsArcropolis: return Text::ModsErrorNeedsArcropolis;
    case ModError::NoSave: return Text::ModsErrorNoSave;
    case ModError::None: break;
    }
    return Text::ModsErrorWrite;
}

void DrawModsPanel(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto screen = view.page.screen;
    // Full-panel screens draw over everything.
    if (!view.session.Busy() && screen == ModsScreen::Detail) {
        DrawDetail(paint, view, x, y);
        return;
    }
    // The game options' shell draws the footer's hairline: this surface moves with the header.
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Canvas);
    DrawTabs(paint, view, x, y + L::TabsTop);
    if (view.session.Busy()) {
        DrawBusy(paint, view, x, y);
        return;
    }
    switch (view.page.tab) {
    case ModsTab::Installed:
        DrawInstalled(paint, view, x, y);

        break;
    case ModsTab::Explore:
        DrawExploreFilters(paint, view, x, y);
        DrawExplore(paint, view, x, y);

        break;
    case ModsTab::Files:
        DrawFiles(paint, view, x, y);
        break;
    }
    switch (screen) {
    case ModsScreen::Actions: DrawActions(paint, view, x, y); break;
    case ModsScreen::Remove: DrawRemove(paint, view, x, y); break;
    case ModsScreen::Variants: DrawVariants(paint, view, x, y); break;
    case ModsScreen::StoreMenu: DrawStoreMenu(paint, view, x, y); break;
    case ModsScreen::Keyboard: DrawKeyboard(paint, view, x, y); break;
    case ModsScreen::Games: DrawGames(paint, view, x, y); break;
    case ModsScreen::Contents: DrawContents(paint, view, x, y); break;
    case ModsScreen::Categories: DrawCategories(paint, view, x, y); break;
    case ModsScreen::Detail: case ModsScreen::List: break;
    }
}

void DrawModsStatus(UiPainter& paint, const ModsView& view, float x, float y) {
    if (view.session.Busy() || view.page.screen != ModsScreen::List) return;
    switch (view.page.tab) {
    case ModsTab::Installed: DrawInstalledDetail(paint, view, x, y); break;
    case ModsTab::Explore: break; // Its status shares the filters' line.
    case ModsTab::Files: {
        const auto& snapshot = view.session.View();
    const float top = y + L::Detail;
    const std::wstring where = snapshot.folder.empty() ? std::wstring{Tr(Text::ModsSourcesHint)} : snapshot.folder_name;
    paint.Text(where, x + L::Margin, top, 14, Palette::TextHint, RowWidth, 20);
    if (!view.notice.empty())
        paint.Text(view.notice, x + L::Margin, top + 20, 13, Palette::Warning, RowWidth, 18);
        break;
    }
    }
}

std::vector<ModsPrompt> ModsPrompts(const ModSession& session, const ModsPage& page) {
    using B = ModsPrompt::Button;
    if (session.Busy()) return {{B::B, Text::ModsPromptCancel}};
    const auto& store = session.Store();
    switch (page.screen) {
    case ModsScreen::Actions: return {{B::A, Text::Select}, {B::B, Text::Back}};
    case ModsScreen::Remove: return {{B::A, Text::ModsPromptUninstall}, {B::B, Text::ModsPromptCancel}};
    case ModsScreen::Variants:
        if (page.chosen.size() > 1)
            return {{B::A, Text::ModsPromptTick}, {B::X, Text::ModsPromptInstall}, {B::B, Text::ModsPromptCancel}};
        return {{B::A, Text::ModsPromptInstall}, {B::B, Text::ModsPromptCancel}};
    case ModsScreen::Detail: {
        std::vector<ModsPrompt> prompts;
        if (store.detail && !store.detail->files.empty()) prompts.push_back({B::A, Text::ModsPromptDownload});
        if (store.detail && store.detail->images.size() > 1) prompts.push_back({B::Bumpers, Text::ModsPromptPictures});
        prompts.push_back({B::B, Text::Back});
        return prompts;
    }
    case ModsScreen::StoreMenu: return {{B::A, Text::Select}, {B::B, Text::Back}};
    case ModsScreen::Keyboard:
        return {{B::A, Text::ModsPromptType}, {B::X, Text::ModsPromptDelete}, {B::Y, Text::ModsPromptSpace},
                {B::Menu, Text::ModsPromptDone}, {B::B, Text::ModsPromptCancel}};
    case ModsScreen::Games:
        if (store.games.empty()) return {{B::X, Text::ModsPromptSearch}, {B::B, Text::Back}};
        return {{B::A, Text::Select}, {B::X, Text::ModsPromptSearch}, {B::B, Text::Back}};
    case ModsScreen::Contents: return {{B::B, Text::Back}};
    case ModsScreen::Categories: {
        const auto rows = session.CategoryRows();
        if (rows.empty()) return {{B::B, Text::Back}};
        const auto& row = rows[std::min(page.category, rows.size() - 1)];
        const bool nested = row.kind == CategoryRow::Kind::Category && store.categories[row.index].nested;
        if (nested) return {{B::A, Text::ModsPromptOpen}, {B::X, Text::ModsPromptListAll}, {B::B, Text::Back}};
        return {{B::A, Text::Select}, {B::B, Text::Back}};
    }
    case ModsScreen::List: break;
    }
    if (page.moving) return {{B::A, Text::ModsPromptDrop}};
    switch (page.tab) {
    case ModsTab::Files:
        return {{B::A, Text::ModsPromptOpen}, {B::Bumpers, Text::ModsPromptTab}, {B::B, Text::Back}};
    case ModsTab::Explore: {
        std::vector<ModsPrompt> prompts;
        if (!store.cards.empty()) prompts.push_back({B::A, Text::ModsPromptOpen});
        else if (store.status == StoreStatus::NoGame) prompts.push_back({B::A, Text::ModsPromptFindGame});
        else if (store.status == StoreStatus::Offline) prompts.push_back({B::A, Text::ModsPromptRetry});
        prompts.push_back({B::X, Text::ModsPromptSearch});
        if (store.source == StoreSource::GameBanana) prompts.push_back({B::Menu, Text::ModsPromptCategories});
        prompts.push_back({B::Y, Text::ModsPromptOptions});
        prompts.push_back({B::Bumpers, Text::ModsPromptTab});
        prompts.push_back({B::B, Text::Back});
        return prompts;
    }
    case ModsTab::Installed:
        break;
    }
    std::vector<ModsPrompt> prompts;
    const auto count = session.View().mods.size();
    if (count) prompts.push_back({B::A, Text::ModsPromptToggle});
    if (count > 1) prompts.push_back({B::X, Text::ModsPromptMove});
    if (count) prompts.push_back({B::Y, Text::ModsPromptOptions});
    prompts.push_back({B::Bumpers, Text::ModsPromptTab});
    prompts.push_back({B::B, Text::Back});
    return prompts;
}

ModsCounts CountsFor(const ModSession& session, const ModsPage& page, std::wstring_view game_name) {
    const auto& state = session.View();
    const auto& store = session.Store();
    ModsCounts counts;
    counts.installed = state.mods.size();
    counts.files = state.files.size();
    counts.files_root = state.folder.empty();
    counts.busy = session.Busy();
    counts.update = session.HasUpdate(page.selected);
    counts.cards = store.cards.size();
    counts.no_game = store.status == StoreStatus::NoGame;
    counts.offline = store.status == StoreStatus::Offline;
    if (store.detail) {
        counts.detail_files = store.detail->files.size();
        counts.detail_images = store.detail->images.size();
    }
    counts.games = store.searching_games ? 0 : store.games.size();
    if (page.selected < state.mods.size()) counts.contents = state.mods[page.selected].contents.listing.size();
    for (const auto& row : session.CategoryRows())
        counts.category_nested.push_back(row.kind == CategoryRow::Kind::Category && store.categories[row.index].nested);
    counts.category_top = store.browse.empty();
    counts.store = {store.source == StoreSource::GameBanana, !store.search.empty(), !session.Downloads().empty()};
    counts.search = Utf8ToUtf16(store.search);
    counts.game_name = std::wstring{game_name};
    return counts;
}

} // namespace Jadyx::Mods
