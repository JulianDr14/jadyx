// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/content/content_view.h"
#include <iomanip>
#include <sstream>
#include "jadyx/ui/ui_painter.h"
#include "jadyx/storage/utf8.h"

namespace Jadyx::Content {
using T = I18n::Text;
using I18n::Tr;
I18n::Text ErrorText(Error error) {
    switch (error) {
    case Error::None: return T::ContentReady;
    case Error::Cancelled: return T::ContentCancelled;
    case Error::Unavailable: return T::ContentUnavailable;
    case Error::Unsupported: return T::ContentUnsupported;
    case Error::Invalid: return T::ContentInvalid;
    case Error::MissingKeys: return T::ContentMissingKeys;
    case Error::Incomplete: return T::ContentIncomplete;
    case Error::HashMismatch: return T::ContentHashMismatch;
    case Error::WrongGame: return T::ContentWrongGame;
    case Error::Conflict: return T::ContentConflict;
    case Error::Space: return T::ContentSpace;
    case Error::Registry: return T::ContentRegistryError;
    case Error::Requirements: return T::ContentRequirements;
    default: return T::ContentIo;
    }
}
namespace {
std::wstring SystemVersionText(std::uint32_t v) {
    return std::to_wstring(v >> 26) + L"." + std::to_wstring((v >> 20) & 0x3F) + L"." + std::to_wstring((v >> 16) & 0xF);
}
} // namespace
std::wstring LaunchErrorText(const LaunchResolution& resolution) {
    if (resolution.error == Error::Requirements && resolution.required_system)
        return I18n::Format(T::ContentRequiresFirmware, {SystemVersionText(resolution.required_system),
                                                          SystemVersionText(SupportedSystemVersion())});
    return std::wstring{Tr(ErrorText(resolution.error))};
}
namespace {
std::wstring Label(const Revision& r) {
    std::wostringstream title;
    if (!r.name.empty()) title << Utf8ToUtf16(r.name);
    else title << Tr(r.kind == Kind::Update ? T::ContentUpdateItem : T::ContentDlcItem);
    title << L"  ·  ";
    if (!r.display_version.empty()) title << Utf8ToUtf16(r.display_version);
    else title << L"v" << r.version;
    if (r.name.empty()) title << L"  ·  " << std::uppercase << std::hex << std::setw(16) << std::setfill(L'0') << r.title;
    return title.str();
}
std::wstring Detail(const Revision& r, Error error) {
    std::wostringstream text;
    text << (error != Error::None ? Tr(ErrorText(error)) :
        Tr(r.enabled ? T::ContentActive : T::ContentInactive));
    text << L"  ·  " << Tr(r.internal ? T::ContentInternal : T::ContentUsb);
    text << L"  ·  " << r.bytes / (1024 * 1024) << L" MiB";
    return text.str();
}
} // namespace
void DrawMissingContent(UiPainter& paint) {
    paint.Round(120, 210, 1040, 320, 24, Palette::Panel);
    paint.Text(Tr(T::ContentMissingLaunch), 160, 245, 25, Palette::Warning, 960, 130, true, true);
    paint.Prompt(L"A", ButtonColor::A, Tr(T::ContentReconnect), 165, 435);
    paint.Prompt(L"X", ButtonColor::X, Tr(T::ContentLaunchWithout), 430, 435);
    paint.Prompt(L"B", ButtonColor::B, Tr(T::Cancel), 980, 435);
}
namespace {
/// Under the game's tab bars, as the other option pages; the prompts sit on the shared footer line.
constexpr float Top = 214, Rows = 260, RowStride = 48, Status = 626, Footer = 676;
constexpr std::size_t VisibleRows = 7;
} // namespace
void DrawContentPanel(UiPainter& paint, const ContentSession& session, std::wstring_view game, const Keys& keys) {
    const auto& view = session.View();
    paint.Round(0, 0, 1280, 720, 0, Palette::Backdrop);
    paint.ManagementSurface(0, 0, 1280, 720, Footer);
    const auto title = Tr(session.Category() == Kind::Update ? T::ContentUpdates : T::ContentDlc);
    paint.Type(title, TypeRole::LabelLarge, 56, 100, 700, Palette::TextHint);
    paint.Type(game, TypeRole::HeadlineMedium, 56, 124, 1168, Palette::Text, 1, TextAlign::Leading, 2);
    const auto storage = Tr(session.CopyInternal() ? T::ContentInternal : T::ContentUsb);
    std::wstring header;
    switch (view.screen) {
    case Screen::Manager: header = Tr(T::ContentManagerHint); break;
    case Screen::Storage: header = Tr(T::ContentChooseStorage); break;
    case Screen::Sources: header = Tr(T::ContentSourcesStep); break;
    case Screen::Browser: header = std::wstring{Tr(T::ContentChooseFiles)} + L"  ·  " + std::wstring{storage}; break;
    case Screen::Preview: case Screen::ConfirmImport:
        header = std::wstring{Tr(T::ContentReview)} + L"  ·  " + std::wstring{storage}; break;
    case Screen::Result:
        header = I18n::Format(T::ContentResult, {std::to_wstring(view.added), std::to_wstring(view.already_present)}); break;
    default: {
        const auto item = std::find_if(view.rows.begin(), view.rows.end(), [&](const auto& r) { return r.id == view.action_id; });
        if (item != view.rows.end()) header = Label(*item) + L"  ·  " + Detail(*item, Error::None);
        break;
    }
    }
    paint.Round(56, Top, 1168, 34, 17, Palette::Surface);
    paint.Type(header, TypeRole::LabelLarge, 80, Top + 8, 1120, Palette::TextHint);
    if (session.Busy()) {
        paint.Text(Tr(view.screen == Screen::ConfirmImport ? (session.CopyInternal() ? T::ContentAddingInternal : T::ContentAddingUsb) : T::ContentWorking), 145, Rows + 16, 23, Palette::Text, 980, 55, true);
        paint.Round(145, Rows + 86, 980, 12, 6, Palette::Track);
        if (session.Total()) {
            const auto fraction = std::min(1.0, static_cast<double>(session.Done()) / session.Total());
            paint.Round(145, Rows + 86, static_cast<float>(980 * fraction), 12, 6, Palette::Text);
            paint.Text(std::to_wstring(session.Done() / (1024 * 1024)) + L" / " +
                std::to_wstring(session.Total() / (1024 * 1024)) + L" MiB", 145, Rows + 112, 19, Palette::TextHint, 980);
        }
        paint.Prompt(keys.back.letters, keys.back.color, Tr(T::Cancel), 56, Footer); return;
    }
    if (view.screen == Screen::ConfirmImport || view.screen == Screen::ConfirmRemove) {
        paint.Text(Tr(view.screen == Screen::ConfirmImport ? T::ContentConfirmImport : T::ContentConfirmRemove),
                   145, Rows + 16, 23, Palette::Warning, 980, 100, true, true);
        if (view.screen == Screen::ConfirmImport) {
            paint.Text(Tr(session.CopyInternal() ? T::ContentInternalHint : T::ContentUsbHint),
                       145, Rows + 126, 19, Palette::Text, 980, 75, false, true);
            if (session.CopyInternal()) {
                std::uint64_t required{};
                for (size_t i = 0; i < view.preview.size(); ++i)
                    if (view.checked[i] && PlanImport(view.catalog.revisions, view.preview[i].revision) == ImportAction::Add)
                        required += view.preview[i].revision.bytes;
                paint.Text(std::to_wstring(required / (1024*1024)) + L" MiB  ·  " +
                    I18n::Format(T::ContentFreeSpace, {std::to_wstring(view.available / (1024*1024))}),
                    145, Rows + 206, 18, Palette::TextHint, 980, 35);
            }
        }
        paint.Prompt(keys.confirm.letters, keys.confirm.color, Tr(view.screen == Screen::ConfirmImport ? T::ContentImportSelected : T::Yes), 56, Footer);
        paint.Prompt(keys.back.letters, keys.back.color, Tr(T::Cancel), 1100, Footer); return;
    }
    struct Row { std::wstring name, detail; bool checked{}; };
    std::vector<Row> rows;
    switch (view.screen) {
    case Screen::Manager:
        rows.push_back({std::wstring{Tr(T::ContentInstall)}, std::wstring{Tr(T::ContentInstallHint)}});
        for (size_t i = 0; i < view.rows.size(); ++i)
            rows.push_back({Label(view.rows[i]), Detail(view.rows[i], view.row_errors[i]), view.rows[i].enabled});
        break;
    case Screen::Storage:
        rows.push_back({std::wstring{Tr(T::ContentUsb)}, std::wstring{Tr(T::ContentUsbHint)}});
        rows.push_back({std::wstring{Tr(T::ContentInternal)}, std::wstring{Tr(T::ContentInternalHint)}});
        break;
    case Screen::Result:
        for (const auto& item : view.completed) rows.push_back({Label(item), Detail(item, Error::None), item.enabled});
        rows.push_back({std::wstring{Tr(T::ContentReturnManager)}, std::wstring{Tr(view.error == Error::None ? T::ContentFinished : T::ContentResultFailed)}});
        break;
    case Screen::Sources:
        rows.push_back({std::wstring{Tr(T::ContentAuthorize)}, std::wstring{Tr(T::ContentAuthorizeHint)}});
        for (const auto& item : view.browser) rows.push_back({item.name, std::wstring{Tr(T::ContentFolder)}});
        break;
    case Screen::Browser:
        for (const auto& item : view.browser) rows.push_back({item.name,
            std::wstring{Tr(item.folder ? T::ContentFolder : T::ContentPackage)},
            std::find(view.selected_files.begin(), view.selected_files.end(), item.path) != view.selected_files.end()});
        rows.push_back({I18n::Format(T::ContentContinueFiles, {std::to_wstring(view.selected_files.size())}), std::wstring{Tr(T::ContentSelectionHint)}});
        break;
    case Screen::Preview:
        for (size_t i = 0; i < view.preview.size(); ++i) {
            const auto& item = view.preview[i];
            const auto plan = PlanImport(view.catalog.revisions, item.revision);
            const auto status = item.error != Error::None ? ErrorText(item.error) :
                plan == ImportAction::Duplicate ? T::ContentDuplicate : T::ContentVerifyBeforeActivate;
            auto detail = std::wstring{Tr(status)};
            if (plan == ImportAction::Duplicate) {
                const auto saved = std::find_if(view.catalog.revisions.begin(), view.catalog.revisions.end(),
                    [&](const auto& r) { return r.id == item.revision.id; });
                if (saved != view.catalog.revisions.end()) detail += L"  ·  " + std::wstring{Tr(saved->internal ? T::ContentInternal : T::ContentUsb)};
            } else detail += L"  ·  " + std::to_wstring(item.revision.bytes / (1024*1024)) + L" MiB";
            rows.push_back({Label(item.revision), std::move(detail), view.checked[i]});
        }
        rows.push_back({I18n::Format(T::ContentContinueItems, {std::to_wstring(std::count(view.checked.begin(), view.checked.end(), true))}), std::wstring{storage}});
        break;
    case Screen::Actions: {
        const auto item = std::find_if(view.rows.begin(), view.rows.end(), [&](const auto& r) { return r.id == view.action_id; });
        const auto toggle = item != view.rows.end() && item->enabled ? T::ContentDisable : T::ContentEnable;
        for (auto text : {toggle, T::ContentVerify, T::ContentRelocate, T::ContentDelete})
            rows.push_back({std::wstring{Tr(text)}, {}});
        break;
    }
    default: break;
    }
    const bool manager = view.screen == Screen::Manager;
    const float row_width = manager ? 744.0f : 1168.0f;
    if (manager) {
        constexpr float card = Rows, shift = Rows - 354;
        paint.Card(824, card, 400, (VisibleRows - 1) * RowStride + 44, 24);
        paint.Icon(UiIcon::Folder, 848, 370 + shift, Palette::Text);
        paint.Type(title, TypeRole::HeadlineSmall, 848, 408 + shift, 352, Palette::Text);
        paint.Round(848, 450 + shift, 352, 1, 0, Palette::Graphite);
        std::uint64_t bytes{};
        for (const auto& revision : view.rows) bytes += revision.bytes;
        paint.Type(std::to_wstring(view.rows.size()) + L"  ·  " +
                   std::to_wstring(bytes / (1024 * 1024)) + L" MiB",
                   TypeRole::HeadlineMedium, 848, 468 + shift, 352, Palette::Text);
        if (view.selected > 0 && view.selected <= view.rows.size()) {
            const auto& revision = view.rows[view.selected - 1];
            paint.Type(Label(revision), TypeRole::BodyLarge, 848, 512 + shift, 352,
                       Palette::Text, 1, TextAlign::Leading, 2);
            paint.Type(Detail(revision, view.row_errors[view.selected - 1]), TypeRole::Body,
                       848, 572 + shift, 352, Palette::TextHint, 1, TextAlign::Leading, 3);
        } else {
            paint.Type(Tr(T::ContentInstallHint), TypeRole::BodyLarge, 848, 512 + shift, 352,
                       Palette::TextHint, 1, TextAlign::Leading, 4);
        }
    }
    const size_t first = view.selected / VisibleRows * VisibleRows;
    for (size_t i = first; i < std::min(rows.size(), first + VisibleRows); ++i) {
        const float y = Rows + static_cast<float>(i - first) * RowStride;
        paint.SelectionCard(56, y, row_width, 44, i == view.selected, 12);
        paint.Icon(rows[i].checked ? UiIcon::Game : UiIcon::Folder, 76, y + 8,
                   rows[i].checked ? Palette::Text : Palette::TextSecondary);
        paint.Text(rows[i].name, 126, y + 2, 17, Palette::Text, row_width - 94, 24, true);
        paint.Text(rows[i].detail, 126, y + 26, 12, Palette::TextHint, row_width - 94, 16);
    }
    paint.ScrollIndicator(56 + row_width + 14, Rows, (VisibleRows - 1) * RowStride + 44, first,
                          VisibleRows, rows.size());
    if (rows.empty()) paint.Text(Tr(T::ContentEmpty), 80, Rows + 16, 22, Palette::TextHint, row_width - 48);
    if (view.error != Error::None) paint.Text(Tr(ErrorText(view.error)), 56, Status, 17, Palette::Warning, 1168, 32);
    float next = 56;
    next += paint.Prompt(keys.confirm.letters, keys.confirm.color, Tr(view.screen == Screen::Result ? T::ContentReturnManager : T::Select), next, Footer);
    if (view.screen == Screen::Browser || view.screen == Screen::Preview)
        next += paint.Prompt(keys.primary.letters, keys.primary.color, Tr(T::ContentImportSelected), next, Footer);
    if (view.screen == Screen::Browser) paint.Prompt(keys.secondary.letters, keys.secondary.color, Tr(T::ContentMarkFile), next, Footer);
    if (view.screen == Screen::Browser)
        paint.Text(I18n::Format(T::ContentContinueFiles, {std::to_wstring(view.selected_files.size())}), 56, Status, 17, Palette::Text, 1168, 26);
    if (view.screen == Screen::Result && view.error == Error::None)
        paint.Text(Tr(T::ContentFinished), 56, Status, 18, Palette::Text, 1168, 26, true);
    paint.Prompt(keys.back.letters, keys.back.color, Tr(T::Back), 1100, Footer);
}
} // namespace Jadyx::Content
