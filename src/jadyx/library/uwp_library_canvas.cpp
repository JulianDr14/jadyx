// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/uwp_library_canvas.h"
#include "jadyx/updates/update_ui.h"
#include <ctime>
#include "jadyx/input/pro_controller_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include "common/logging.h"
#include "jadyx/cheats/cheats_view.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/play_history.h"
#include "jadyx/library/game_options_page.h"
#include "jadyx/library/library_layout.h"
#include "jadyx/content/content_view.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/mods/mods_view.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_painter.h"
#include "jadyx/ui/text_entry_panel.h"
#include "jadyx/ui/tween.h"
#include "jadyx/ui/ui_text.h"
#include "jadyx/input/uwp_input.h"

namespace Jadyx {
using Microsoft::WRL::ComPtr;
using I18n::Tr;
using T = I18n::Text;
// The launcher owns a small D3D11/Direct2D surface only while selecting a game.
// Release every reference (including the swapchain) before D3D12 starts.
struct LibraryCanvas::Impl : UiPainter {
public:
    Impl(void* window, unsigned width, unsigned height) {
        ComPtr<ID3D11Device> device;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = std::max(1U, width);
        desc.Height = std::max(1U, height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        winrt::check_hresult(factory->CreateSwapChainForCoreWindow(device.Get(),
            static_cast<IUnknown*>(window), &desc, nullptr, &swapchain));
        ComPtr<ID2D1Factory1> d2d;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                              IID_PPV_ARGS(&d2d)));
        ComPtr<ID2D1Device> d2d_device;
        winrt::check_hresult(d2d->CreateDevice(dxgi.Get(), &d2d_device));
        winrt::check_hresult(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                           &target));
        ComPtr<IDXGISurface> backbuffer;
        winrt::check_hresult(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE));
        ComPtr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(target->CreateBitmapFromDxgiSurface(backbuffer.Get(),
                                                                &properties, &bitmap));
        target->SetTarget(bitmap.Get());
        ComPtr<IDWriteFactory> factory_write;
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory_write.GetAddressOf())));
        Attach(target.Get(), factory_write.Get());
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        // Cache the installed version so the footer follows manifest updates
        // without a second version constant or per-frame WinRT calls.
        const auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        version_text = L"v" + std::to_wstring(version.Major) + L"." +
            std::to_wstring(version.Minor) + L"." + std::to_wstring(version.Build) +
            L"." + std::to_wstring(version.Revision);
        try {
            asset_root = std::filesystem::path{
                winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()} / L"Assets";
        } catch (const winrt::hresult_error&) { /* Pictures from the package stay unavailable. */ }
        const float scale = std::min(width / Canvas::Unit1280_0F, height / Canvas::Unit720_0F);
        target->SetTransform(D2D1::Matrix3x2F::Scale(std::max(scale, 0.01f), std::max(scale, 0.01f)) *
            D2D1::Matrix3x2F::Translation((width - 1280 * scale) / 2, (height - 720 * scale) / 2));
    }
    ~Impl() {
        target->SetTarget(nullptr);
        target.Reset();
        swapchain.Reset();
        context->ClearState();
        context->Flush();
    }
    void Draw(const LibraryView& view) {
        const auto& scan = view.scan;
        const auto& options = view.options;
        const auto& panel = view.panel;
        const auto& configuration = view.configuration;
        const auto now = Tween::Clock::now();
        if (canvas_theme != UiTheme()) {
            canvas_theme = UiTheme();
            for (auto& scrim : scrims)
                scrim.Reset();
            cover_mask.Reset();
        }
        SyncLanguage();
        prompt_family = PromptFamily::Keyboard;
        if (panel.xbox) prompt_family = PromptFamily::Xbox;
        else if (const auto index = SelectController(panel.devices, options.controller_id))
            prompt_family = panel.devices[*index].pro ? PromptFamily::Nintendo : PromptFamily::Xbox;
        swap_prompt_faces = false;
        if (const auto active = SelectController(panel.devices, options.controller_id))
            swap_prompt_faces = SwapControllerFaceButtons(panel.devices[*active], options.swap_face_buttons);
        using LibraryLayout::LibraryTab;
        const bool library = !view.loading && view.selected < scan.entries.size();
        const LibraryEntry* game = library ? &scan.entries[view.selected] : nullptr;
        // Each tab enters settled: no scroll or focus transition carried over from another.
        if (view.tab != shown_tab) {
            shown_tab = view.tab;
            rows_shown = false;
        }
        const bool management = view.game_options.game || view.game_options.content;
        // Only the active library content prepares and draws its backdrop.
        if (!management && !configuration.network_screen) UpdateBackdrop(game, now);
        hits.clear();
        target->BeginDraw();
        target->Clear(Ink(Palette::Canvas));
        if (!management && !configuration.network_screen) DrawBackdrop(now);
        // The backdrop moves on its own while it shows (AmbientDue).
        ambient = !management && !configuration.network_screen && !backdrop_now.empty();
        last_frame = now;
        if(configuration.confirm_import){
            rows_shown=false;
            Type(Tr(T::SystemImportReview), TypeRole::HeadlineLarge, Canvas::Unit80, Canvas::Unit80,
                 Canvas::Unit1120, Palette::Text);
            Type(configuration.review_text, TypeRole::BodyLarge, Canvas::Unit80, Canvas::Unit190,
                 Canvas::Unit1120, Palette::Warning, Canvas::Unit1, TextAlign::Leading, 10);
            for (size_t i = 0; i < 2; ++i) {
                const float x = Canvas::Unit80 + static_cast<float>(i) * Canvas::Unit570;
                const bool focused = configuration.network_action == i;
                Round(x, Canvas::Unit590, Canvas::Unit540, Canvas::Unit64, Canvas::Unit14,
                      focused ? Palette::Accent : Palette::Surface);
                Type(Tr(i == 0 ? T::Cancel : T::Import), TypeRole::LabelLarge, x + Canvas::Unit20,
                     Canvas::Unit611, Canvas::Unit500, focused ? Palette::OnAccent : Palette::Text);
                AddHit(x, Canvas::Unit590, Canvas::Unit540, Canvas::Unit64,
                       LibraryHit::NetworkAction, i);
            }
        } else if (configuration.ftp_screen) {
            rows_shown = false;
            DrawFtpConnection(configuration);
        } else if (configuration.network_screen && configuration.network_update) {
            rows_shown = false; DrawUpdateGuide(configuration, view.system.update);
        } else if (view.system.updates_open) {
            rows_shown = false; DrawUpdates(view.system);
        } else if (configuration.network_screen) {
            // A dedicated page: only its controls populate the pointer hit map.
            rows_shown = false;
            DrawNetworkConnection(configuration);
        } else if (management) {
            rows_shown = false;
        } else if (view.tab == LibraryTab::Settings) {
            rows_shown = false;
            // Settings dims the art further: its own page, not a dialog over the library.
            Round(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720, Canvas::Unit0,
                  Palette::Canvas, Canvas::Unit0_62F);
            DrawSettings(view);
        } else if (game) {
            if (view.tab == LibraryTab::Home) {
                DrawRows(view, now);
                DrawHero(view, *game, now);
            } else {
                DrawShelf(view, now);
            }
        } else {
            rows_shown = false;
            Type(Tr(view.loading ? T::LoadingTitle : T::EmptyTitle), TypeRole::HeadlineLarge,
                 LibraryLayout::Margin, Canvas::Unit220, Canvas::Unit1000, Palette::Text,
                 Canvas::Unit1, TextAlign::Leading, 2);
            Type(Tr(view.loading ? T::LoadingBody : T::EmptyBody), TypeRole::BodyLarge,
                 LibraryLayout::Margin, Canvas::Unit330, Canvas::Unit900, Palette::TextSecondary,
                 Canvas::Unit1, TextAlign::Leading, 3);
            if (!view.notice.empty())
                Type(view.notice, TypeRole::Body, LibraryLayout::Margin, Canvas::Unit430,
                     Canvas::Unit1000, Palette::Warning, Canvas::Unit1, TextAlign::Leading, 2);
        }
        if (!management && !configuration.network_screen && !configuration.confirm_import && !view.system.updates_open) {
            DrawHeader(view, now);
            DrawFooter(view);
        }
        // On the Settings tab the controller's rows are its page; only the device list floats.
        if (view.controller_panel && view.tab != LibraryTab::Settings) DrawControllerPanel(view);
        else if (view.controller_panel && panel.expanded) DrawControllerChoices(view, true);
        if (panel.keyboard.open) DrawKeyboard(panel.keyboard);
        if (configuration.open && view.system.open && view.system.name_editor) DrawNameEditor(view.system);
        if (management) {
            DrawGameOptions(view.game_options);
            // The library owns the header. Pages replace its content, never its chrome.
            Round(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit94, Canvas::Unit0,
                  Palette::Canvas);
            DrawHeader(view, now);
        }
        if (!view.system.updates_open && !management && !configuration.network_screen && !configuration.open &&
            view.tab != LibraryTab::Settings && !view.controller_panel && !view.missing_content && !panel.keyboard.open) {
            if (view.system.updated) DrawUpdatedNotice(view.system.update.installed);
            else if (view.system.update.notice && view.system.update.release) DrawUpdateNotice(*view.system.update.release);
        }
        if (!view.game_options.game) management_collapse.Snap(0);
        if (view.missing_content) Content::DrawMissingContent(*this);
        // A text whose fallback font the system cannot read (0x8007067D, now and then on the
        // console) fails the frame, not the library: the next frame draws again.
        if (const HRESULT drawn = target->EndDraw(); FAILED(drawn)) {
            if (drawn == D2DERR_RECREATE_TARGET) winrt::throw_hresult(drawn);
            if (drawn != last_draw_error) LOG_WARNING(Frontend, "Library: frame failed: {:08X}", static_cast<u32>(drawn));
            last_draw_error = drawn;
        }
        winrt::check_hresult(swapchain->Present(1, 0));
        animating = scroll.Running(now) || focus_in.Running(now) || focus_out.Running(now) ||
                    hero.Running(now) || backdrop_fade.Running(now) || tab_slide.Running(now) ||
                    filter_slide.Running(now) || management_collapse.Running(now) ||
                    list_scroll.Running(now) || grid_scroll.Running(now);
    }
    /// The library's corner card for a new version: Y opens the update page, X hides it a day.
    void DrawUpdateNotice(const Updates::Release& release) {
        const auto version = winrt::to_hstring(Updates::FormatVersion(release.version));
        const auto text = I18n::Format(T::UpdaterNotice, {version.c_str()});
        const auto view_label = Tr(T::UpdaterView), hide_label = Tr(T::UpdaterHide);
        const float text_width =
            std::min(Canvas::Unit300_0F, MeasureType(text, TypeRole::LabelLarge) + Canvas::Unit4);
        const float view_width = PromptWidth(Navigation::Pin, view_label);
        const float width = Canvas::Unit20 + text_width + Canvas::Unit24 + view_width +
                            PromptWidth(Navigation::Rescan, hide_label) - Canvas::Unit28 +
                            Canvas::Unit16;
        const float x = Canvas::Unit1280 - LibraryLayout::Margin - width, y = Canvas::Unit98;
        // Dot, text (20 px line) and prompts (32 px line) share the card's center line.
        const float center = y + Canvas::Unit24;
        Card(x, y, width, Canvas::Unit48, Canvas::Unit14);
        Round(x + Canvas::Unit12, center - Canvas::Unit4, Canvas::Unit8, Canvas::Unit8,
              Canvas::Unit4, Palette::Accent);
        Type(text, TypeRole::LabelLarge, x + Canvas::Unit28, center - Canvas::Unit10, text_width,
             Palette::Text);
        float next = x + Canvas::Unit28 + text_width + Canvas::Unit16;
        AddHit(next - Canvas::Unit4, y, view_width - Canvas::Unit20, Canvas::Unit48,
               LibraryHit::UpdateNotice, 0);
        next += ControlPrompt(Navigation::Pin, view_label, next, center - Canvas::Unit16);
        AddHit(next - Canvas::Unit4, y, x + width - next + Canvas::Unit4, Canvas::Unit48,
               LibraryHit::UpdateNotice, 1);
        ControlPrompt(Navigation::Rescan, hide_label, next, center - Canvas::Unit16);
    }
    /// The library's corner card after a guided install: this launch runs the new version.
    void DrawUpdatedNotice(const Updates::Version& installed) {
        const auto version = winrt::to_hstring(Updates::FormatVersion(installed));
        const auto text = I18n::Format(T::UpdaterUpdatedTo, {version.c_str()});
        const float text_width =
            std::min(Canvas::Unit360_0F, MeasureType(text, TypeRole::LabelLarge) + Canvas::Unit4);
        const float width = Canvas::Unit28 + text_width + Canvas::Unit20;
        const float x = Canvas::Unit1280 - LibraryLayout::Margin - width, y = Canvas::Unit98,
                    center = y + Canvas::Unit24;
        Card(x, y, width, Canvas::Unit48, Canvas::Unit14);
        Round(x + Canvas::Unit12, center - Canvas::Unit4, Canvas::Unit8, Canvas::Unit8,
              Canvas::Unit4, Palette::Accent);
        Type(text, TypeRole::LabelLarge, x + Canvas::Unit28, center - Canvas::Unit10, text_width,
             Palette::Text);
    }
    /// The update page's footer: A selects (when a row is there), B goes back.
    void DrawUpdatesFooter(bool select) {
        const float y = LibraryLayout::FooterTop + Canvas::Unit12;
        float x = LibraryLayout::Margin;
        if (select) x += ControlPrompt(Navigation::Play, Tr(T::Select), x, y);
        const auto back = Tr(T::UpdaterBack);
        AddHit(x - Canvas::Unit4, y - Canvas::Unit6,
               PromptWidth(Navigation::Back, back) - Canvas::Unit20, Canvas::Unit44,
               LibraryHit::UpdateAction, UpdateBackHit);
        ControlPrompt(Navigation::Back, back, x, y);
    }
    void DrawUpdates(const SystemPage& page) {
        using namespace LibraryLayout;
        const auto& state = page.update;
        const auto release_version =
            state.release ? std::wstring{winrt::to_hstring(Updates::FormatVersion(state.release->version))} : std::wstring{};
        Round(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720, Canvas::Unit0,
              Palette::Canvas);
        if (page.update_detail && state.release) {
            // Every note, scrolled with the d-pad.
            Type(I18n::Format(T::UpdaterNotesTitle, {release_version}), TypeRole::HeadlineLarge,
                 Margin, Canvas::Unit40, Canvas::Unit1150, Palette::Text);
            const auto language = I18n::CurrentUiLanguage().tag;
            if (update_detail_revision != state.revision || update_detail_language != language) {
                update_detail_revision = state.revision; update_detail_language = language;
                auto& lines = update_detail_lines; lines.clear();
                const std::wstring text = winrt::to_hstring(state.release->notes).c_str();
                for (std::size_t start = 0; start < text.size();) {
                    const auto newline = text.find(L'\n', start);
                    auto end = std::min(start + 110, newline == text.npos ? text.size() : newline);
                    if (end < text.size() && end > start && end != newline) {
                        auto space = text.rfind(L' ', end); if (space != text.npos && space > start) end = space;
                        if (end > start && text[end] >= 0xDC00 && text[end] <= 0xDFFF) --end;
                    }
                    lines.push_back(text.substr(start, end - start)); start = end;
                    if (start < text.size() && (text[start] == L' ' || text[start] == L'\n')) ++start;
                }
            }
            const auto& lines = update_detail_lines;
            constexpr std::size_t shown = 16;
            const auto first = std::min(page.update_scroll, lines.size() > shown ? lines.size() - shown : 0);
            for (std::size_t n = 0; n < shown && first + n < lines.size(); ++n)
                Type(lines[first + n], TypeRole::Body, Margin,
                     Canvas::Unit116 + static_cast<float>(n) * Canvas::Unit32, Canvas::Unit1150,
                     Palette::Text);
            DrawUpdatesFooter(false);
            return;
        }
        Type(Tr(T::UpdaterTitle), TypeRole::HeadlineLarge, Margin, Canvas::Unit40, Canvas::Unit700,
             Palette::Text);
        // The state card: what is happening, the versions and what comes next.
        constexpr float card_x = Margin, card_y = Canvas::Unit112, card_w = Canvas::Unit660,
                        card_h = Canvas::Unit236, inner = card_x + Canvas::Unit28,
                        inner_w = card_w - Canvas::Unit56;
        Card(card_x, card_y, card_w, card_h);
        Type(Tr(Updates::PhaseText(state)), TypeRole::HeadlineMedium, inner,
             card_y + Canvas::Unit24, inner_w, Palette::Text);
        const auto installed = winrt::to_hstring(Updates::FormatVersion(state.installed));
        Type(state.release ? I18n::Format(T::UpdaterVersions, {installed.c_str(), release_version})
                           : I18n::Format(T::UpdaterInstalled, {installed.c_str()}),
             TypeRole::BodyLarge, inner, card_y + Canvas::Unit70, inner_w, Palette::TextHint);
        if (state.release) {
            const auto published = winrt::to_hstring(state.release->published.substr(0, 10));
            Type(I18n::Format(
                     T::UpdaterReleaseInfo,
                     {std::to_wstring(state.release->size / (1024 * 1024)), published.c_str()}),
                 TypeRole::Body, inner, card_y + Canvas::Unit100, inner_w, Palette::TextSecondary);
        }
        const bool progress = state.received && state.total &&
            (state.phase == Updates::Phase::Downloading || state.phase == Updates::Phase::Paused ||
             state.phase == Updates::Phase::Verifying || state.phase == Updates::Phase::Failed);
        float body_y = card_y + Canvas::Unit140;
        if (progress) {
            const auto pct = std::min<std::uint64_t>(100, state.received * 100 / state.total);
            Round(inner, body_y, inner_w, Canvas::Unit10, Canvas::Unit5, Palette::Graphite);
            if (pct)
                Round(inner, body_y, inner_w * static_cast<float>(pct) / Canvas::Unit100,
                      Canvas::Unit10, Canvas::Unit5, Palette::Accent);
            auto eta = std::wstring{Tr(T::UpdaterEta)};
            if (state.bytes_per_second > 0 && state.total >= state.received) {
                const auto seconds = static_cast<unsigned>(std::min(3599999.0, (state.total - state.received) / state.bytes_per_second));
                eta = std::to_wstring(seconds / 3600) + L"h " + std::to_wstring(seconds / 60 % 60) + L"m " + std::to_wstring(seconds % 60) + L"s";
            }
            const auto speed = std::to_wstring(static_cast<unsigned>(state.bytes_per_second * 10 / (1024 * 1024)));
            const auto speed_text = speed.size() > 1 ? speed.substr(0, speed.size() - 1) + L"." + speed.back() : L"0." + speed;
            const bool moving = state.phase == Updates::Phase::Downloading;
            Type(moving
                     ? I18n::Format(T::UpdaterProgress,
                                    {std::to_wstring(pct),
                                     std::to_wstring(state.received / (1024 * 1024)),
                                     std::to_wstring(state.total / (1024 * 1024)), speed_text, eta})
                     : std::to_wstring(pct) + L"% · " +
                           std::to_wstring(state.received / (1024 * 1024)) + L" / " +
                           std::to_wstring(state.total / (1024 * 1024)) + L" MiB",
                 TypeRole::Body, inner, body_y + Canvas::Unit16, inner_w, Palette::TextSecondary);
            body_y += Canvas::Unit46;
        }
        // One line on what happens next, or what went wrong.
        std::optional<T> body;
        unsigned body_color = Palette::TextSecondary;
        if (state.error != Updates::Error::None && state.phase != Updates::Phase::Checking) {
            body = Updates::ErrorText(state.error); body_color = Palette::Warning;
        } else switch (state.phase) {
        case Updates::Phase::Idle: case Updates::Phase::Confirmed:
            body = state.release ? T::UpdaterAvailableBody : T::UpdaterIdleBody; break;
        case Updates::Phase::Available: body = T::UpdaterAvailableBody; break;
        case Updates::Phase::Paused: body = state.received ? T::UpdaterDownloadingBody : T::UpdaterAvailableBody; break;
        case Updates::Phase::Downloading: case Updates::Phase::Verifying: body = T::UpdaterDownloadingBody; break;
        case Updates::Phase::Ready: case Updates::Phase::Requested: body = T::UpdaterReadyBody; break;
        case Updates::Phase::Preparing: body = T::UpdaterInstallHint; break;
        case Updates::Phase::Assistance: body = T::UpdaterHelp; break;
        default: break;
        }
        if (body)
            Type(Tr(*body), TypeRole::Body, inner, body_y, inner_w, body_color, Canvas::Unit1,
                 TextAlign::Leading,
                 static_cast<unsigned>(std::max(1.0f, (card_y + card_h - 16 - body_y) / 22)));
        // The release notes, on the page: the full list is one row away.
        if (state.release && !state.release->notes.empty()) {
            Type(I18n::Format(T::UpdaterNotesTitle, {release_version}), TypeRole::HeadlineSmall,
                 Margin, Canvas::Unit376, card_w, Palette::Text);
            Type(winrt::to_hstring(state.release->notes).c_str(), TypeRole::Body, Margin,
                 Canvas::Unit414, card_w, Palette::TextHint, Canvas::Unit1, TextAlign::Leading, 9);
        }
        std::wstring checked = std::wstring{Tr(T::UpdaterNever)};
        if (state.last_check) {
            const std::time_t stamp = state.last_check; std::tm local{}; localtime_s(&local, &stamp);
            wchar_t date[64]{}; std::wcsftime(date, 64, L"%Y-%m-%d %H:%M", &local); checked = date;
        }
        Type(I18n::Format(T::UpdaterLastCheck, {checked}), TypeRole::Label,
             Canvas::Unit1280 - Margin - Canvas::Unit400, FooterTop + Canvas::Unit20,
             Canvas::Unit400, Palette::TextSecondary, Canvas::Unit1, TextAlign::Trailing);
        // The rows: only what can be done now; the focused one explains itself underneath.
        const auto actions = Updates::Actions(state);
        constexpr float rows_x = Canvas::Unit780, rows_w = Canvas::Unit1280 - Margin - rows_x;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const bool focused = i == std::min(page.update_row, actions.size() - 1);
            const float y = card_y + static_cast<float>(i) * Canvas::Unit64;
            Round(rows_x, y, rows_w, Canvas::Unit54, Canvas::Unit12,
                  focused ? Palette::Accent : Palette::Surface);
            Type(Updates::ActionLabel(actions[i], state), TypeRole::BodyLarge,
                 rows_x + Canvas::Unit20, y + Canvas::Unit14, rows_w - Canvas::Unit40,
                 focused ? Palette::OnAccent : Palette::Text);
            AddHit(rows_x, y, rows_w, Canvas::Unit54, LibraryHit::UpdateAction, i);
        }
        if (!actions.empty()) {
            const auto focused = actions[std::min(page.update_row, actions.size() - 1)];
            float y = card_y + static_cast<float>(actions.size()) * Canvas::Unit64 + Canvas::Unit8;
            y += Type(Tr(Updates::ActionHint(focused, state)), TypeRole::Body,
                      rows_x + Canvas::Unit4, y, rows_w - Canvas::Unit8, Palette::TextSecondary,
                      Canvas::Unit1, TextAlign::Leading, 5);
            if (focused == Updates::Action::Channel && state.channel == Updates::Channel::Preview)
                Type(Tr(T::UpdaterPreviewWarning), TypeRole::Body, rows_x + Canvas::Unit4,
                     y + Canvas::Unit8, rows_w - Canvas::Unit8, Palette::Warning, Canvas::Unit1,
                     TextAlign::Leading, 3);
        }
        DrawUpdatesFooter(!actions.empty());
    }
    void InvalidateCovers() { covers.clear(); pictures.clear(); backdrops.clear(); backdrop_now.clear(); backdrop_before.clear(); }
    bool Animating() const {
        return animating || (ambient && Tween::Clock::now() - last_frame >= AmbientFrame);
    }
    LibraryTarget HitAt(float x, float y) const {
        // Drawn last is on top: the header over the rows scrolling under it.
        for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit)
            if (x >= hit->rect.left && x < hit->rect.right && y >= hit->rect.top && y < hit->rect.bottom)
                return hit->target;
        return {};
    }
private:
    std::uint64_t update_detail_revision{~std::uint64_t{0}};
    std::wstring update_detail_language;
    std::vector<std::wstring> update_detail_lines;
    bool manager_targets{};
    float manager_offset{};
    float manager_bottom{GameOptionsLayout::ListBottom};
    bool manager_clip{};
    void Target(UiTarget kind, std::size_t index, float x, float y, float w, float h) override {
        if (!manager_targets) return;
        const auto hit = kind == UiTarget::ManagerTab ? LibraryHit::ManagerTab :
                         kind == UiTarget::ManagerRow ? LibraryHit::ManagerRow : LibraryHit::ManagerCard;
        y -= manager_offset;
        if (manager_clip) {
            const float bottom = std::min(y + h, manager_bottom);
            y = std::max(y, GameOptionsLayout::BodyTop - manager_offset);
            h = bottom - y;
            if (h <= 0) return;
        }
        AddHit(x, y, w, h, hit, index);
    }
    void AddHit(float x, float y, float w, float h, LibraryHit hit, std::size_t index = 0) {
        hits.push_back({D2D1::RectF(x, y, x + w, y + h), {hit, index}});
    }
    /// A segment's place in a segmented control.
    struct Segment {
        float x, width;
    };
    static constexpr float SegmentPad = Canvas::Unit18, SegmentInset = Canvas::Unit4;
    float SegmentedWidth(const std::vector<std::wstring_view>& labels) {
        float width = Canvas::Unit2 * SegmentInset;
        for (const auto label : labels) width += MeasureType(label, TypeRole::LabelLarge) + 2 * SegmentPad;
        return width;
    }
    /// Capsules with labels in a graphite track, the white one at `slide` (a fractional segment,
    /// from its tween) and ringed when `focused`. Each segment is a press target.
    void Segmented(const std::vector<std::wstring_view>& labels, float x, float y, float height, float slide,
                   std::size_t active, bool focused, LibraryHit hit) {
        std::vector<Segment> places;
        float cursor = x + SegmentInset;
        for (const auto label : labels) {
            const float width =
                MeasureType(label, TypeRole::LabelLarge) + Canvas::Unit2 * SegmentPad;
            places.push_back({cursor, width});
            cursor += width;
        }
        const float total = cursor + SegmentInset - x;
        Round(x, y, total, height, height / Canvas::Unit2, Palette::TabTrack, Theme().tab_opacity);
        Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, total - Canvas::Unit1,
                height - Canvas::Unit1, height / Canvas::Unit2 - Canvas::Unit0_5F, Palette::White,
                focused ? Canvas::Unit0_85F : Canvas::Unit0_12F,
                focused ? Canvas::Unit1_5F : Canvas::Unit1_0F);
        // The capsule between the two segments it is passing.
        const std::size_t from = std::min(static_cast<std::size_t>(std::max(slide, 0.0f)), places.size() - 1);
        const std::size_t to = std::min(from + 1, places.size() - 1);
        const float t =
            std::clamp(slide - static_cast<float>(from), Canvas::Unit0_0F, Canvas::Unit1_0F);
        const float capsule_x = places[from].x + (places[to].x - places[from].x) * t;
        const float capsule_w = places[from].width + (places[to].width - places[from].width) * t;
        constexpr float inset = SegmentInset;
        Round(capsule_x, y + inset, capsule_w, height - Canvas::Unit2 * inset,
              (height - Canvas::Unit2 * inset) / Canvas::Unit2, Palette::Accent);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            Type(labels[i], TypeRole::LabelLarge, places[i].x,
                 y + (height - Canvas::Unit20) / Canvas::Unit2, places[i].width,
                 i == active ? Palette::OnAccent : Palette::TextSecondary, Canvas::Unit1,
                 TextAlign::Center);
            AddHit(places[i].x, y, places[i].width, height, hit, i);
        }
    }
    /// The wordmark; the tabs in the middle; the controller, the time and the user at the right.
    void DrawHeader(const LibraryView& view, Tween::Clock::time_point now) {
        using namespace LibraryLayout;
        Type(L"J A D Y X", TypeRole::HeadlineSmall, Margin, HeaderTop + Canvas::Unit6,
             Canvas::Unit200, Palette::Text);
        const float mark =
            Margin + MeasureType(L"J A D Y X", TypeRole::HeadlineSmall) + Canvas::Unit14;
        Type(view.panel.xbox ? L"SERIES X|S" : L"PC", TypeRole::Caps, mark,
             HeaderTop + Canvas::Unit13, Canvas::Unit160, Palette::TextSecondary);
        // The tabs, with the bumpers that switch them at their ends.
        const std::vector<std::wstring_view> tabs{Tr(T::TabHome), Tr(T::TabLibrary), Tr(T::TabSettings)};
        const auto active = static_cast<std::size_t>(view.tab);
        if (header_shown) tab_slide.Set(static_cast<float>(active), now);
        else tab_slide.Snap(static_cast<float>(active));
        header_shown = true;
        const float width = SegmentedWidth(tabs);
        const float left = (Canvas::Unit1280 - width) / Canvas::Unit2;
        Segmented(tabs, left, HeaderTop, HeaderButtonHeight, tab_slide.Value(now), active, false, LibraryHit::Tab);
        const float middle = HeaderTop + HeaderButtonHeight / Canvas::Unit2;
        const bool keyboard = prompt_family == PromptFamily::Keyboard;
        const bool nintendo = prompt_family == PromptFamily::Nintendo;
        const std::wstring_view previous = keyboard ? L"Q" : nintendo ? L"L" : L"LB";
        const std::wstring_view next = keyboard ? L"E" : nintendo ? L"R" : L"RB";
        // On a management page bumpers belong to its categories, as in the reference.
        if (!view.game_options.game && !view.game_options.content) {
            Badge(previous, left - Canvas::Unit10 - BadgeWidth(previous) / Canvas::Unit2, middle,
                  Palette::TextSecondary);
            Badge(next, left + width + Canvas::Unit10 + BadgeWidth(next) / Canvas::Unit2, middle,
                  Palette::TextSecondary);
        }
        // Right to left: the user, a hairline, the time and the controller (pressing it opens its panel).
        const std::wstring_view name = view.system.settings.user_name.empty()
                                           ? DefaultUserName
                                           : std::wstring_view{view.system.settings.user_name};
        const float name_width =
            std::min(MeasureType(name, TypeRole::LabelLarge) + Canvas::Unit2, Canvas::Unit150_0F);
        float right = Canvas::Unit1280 - Margin - name_width;
        Type(name, TypeRole::LabelLarge, right, middle - Canvas::Unit10, name_width, Palette::Text);
        right -= Canvas::Unit10 + Canvas::Unit30;
        Round(right, middle - Canvas::Unit15, Canvas::Unit30, Canvas::Unit30, Canvas::Unit15,
              Palette::Graphite);
        Outline(right + Canvas::Unit0_5F, middle - Canvas::Unit14_5F, Canvas::Unit29,
                Canvas::Unit29, Canvas::Unit14_5F, Palette::White, Canvas::Unit0_2F);
        Type(Upper(name.substr(0, 1)), TypeRole::LabelLarge, right, middle - Canvas::Unit10,
             Canvas::Unit30, Palette::Text, Canvas::Unit1, TextAlign::Center);
        right -= Canvas::Unit16;
        Round(right, middle - Canvas::Unit11, Canvas::Unit1, Canvas::Unit22, Canvas::Unit0,
              Palette::White, Canvas::Unit0_18F);
        wchar_t clock[32]{};
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, clock, 32) > 0) {
            const float clock_width = MeasureType(clock, TypeRole::Clock) + Canvas::Unit2;
            right -= Canvas::Unit16 + clock_width;
            Type(clock, TypeRole::Clock, right, middle - Canvas::Unit10, clock_width,
                 Palette::Text);
        }
        const float status_right = right - Canvas::Unit16;
        if (view.battery.known) {
            right -= Canvas::Unit16 + BatteryWidth;
            DrawBattery(view.battery, right, middle);
            right += Canvas::Unit8; // The battery sits close to its controller.
        }
        right -= Canvas::Unit16 + PadWidth;
        const bool connected = view.panel.xbox || !view.panel.devices.empty();
        DrawPadGlyph(right, middle, connected ? Palette::Text : Palette::TextSecondary, connected ? 1.0f : 0.7f);
        AddHit(right - Canvas::Unit6, HeaderTop, status_right - right + Canvas::Unit12,
               HeaderButtonHeight, LibraryHit::Controller);
    }
    /// The header's controller: an Xbox pad's silhouette, drawn as a line icon in a 32x22 box.
    static constexpr float PadWidth = Canvas::Unit30;
    void DrawPadGlyph(float x, float middle, unsigned color, float alpha) {
        const float scale = PadWidth / Canvas::Unit32;
        if (!pad_outline) {
            ComPtr<ID2D1Factory> factory;
            target->GetFactory(&factory);
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&pad_outline)) || FAILED(pad_outline->Open(&sink))) {
                pad_outline.Reset();
                return;
            }
            const auto bezier = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x3, y3)));
            };
            sink->BeginFigure(D2D1::Point2F(9.5f, 1.6f), D2D1_FIGURE_BEGIN_FILLED);
            bezier(13, 0.9f, 19, 0.9f, 22.5f, 1.6f);             // The top edge, under the bumpers.
            bezier(26.5f, 1.6f, 29.2f, 3, 30.2f, 6.5f);          // The right shoulder.
            sink->AddLine(D2D1::Point2F(31.7f, 14));
            bezier(32.4f, 18, 31, 21.4f, 28.4f, 21.6f);          // The right grip's end.
            bezier(26.6f, 21.7f, 25.4f, 20.4f, 24, 18.4f);
            bezier(22.6f, 16.4f, 19.8f, 15.2f, 16, 15.2f);       // The arch between the grips.
            bezier(12.2f, 15.2f, 9.4f, 16.4f, 8, 18.4f);
            bezier(6.6f, 20.4f, 5.4f, 21.7f, 3.6f, 21.6f);       // The left grip's end.
            bezier(1, 21.4f, -0.4f, 18, 0.3f, 14);
            sink->AddLine(D2D1::Point2F(1.8f, 6.5f));
            bezier(2.8f, 3, 5.5f, 1.6f, 9.5f, 1.6f);             // The left shoulder.
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) {
                pad_outline.Reset();
                return;
            }
        }
        D2D1_MATRIX_3X2_F previous;
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(x, middle - 11 * scale) * previous);
        brush->SetColor(Ink(color, Canvas::Unit0_10F * alpha));
        target->FillGeometry(pad_outline.Get(), brush.Get());
        brush->SetColor(Ink(color, alpha));
        target->DrawGeometry(pad_outline.Get(), brush.Get(), 1.7f);
        // As on an Xbox Series pad: the left stick high and the face buttons' diamond across
        // from it, the d-pad and the right stick low and inward, the Xbox button between the
        // bumpers with View and Menu under it.
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(Canvas::Unit8_4F, Canvas::Unit7),
                                          Canvas::Unit2_4F, Canvas::Unit2_4F),
                            brush.Get(), 1.4f);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(Canvas::Unit20, Canvas::Unit11_8F),
                                          Canvas::Unit2_1F, Canvas::Unit2_1F),
                            brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(10.3f, 11.8f), D2D1::Point2F(13.7f, 11.8f), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(12, 10.1f), D2D1::Point2F(12, 13.5f), brush.Get(), 1.4f);
        constexpr D2D1_POINT_2F face[] = {{23.6f, 4.8f}, {23.6f, 9.2f}, {21.4f, 7}, {25.8f, 7}};
        for (const auto button : face)
            target->FillEllipse(D2D1::Ellipse(button, Canvas::Unit1_05F, Canvas::Unit1_05F),
                                brush.Get());
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(Canvas::Unit16, Canvas::Unit4_4F),
                                          Canvas::Unit1_7F, Canvas::Unit1_7F),
                            brush.Get(), 1.2f);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(Canvas::Unit16, Canvas::Unit4_4F),
                                          Canvas::Unit0_8F, Canvas::Unit0_8F),
                            brush.Get());
        for (const float side : {13.8f, 18.2f})
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(side, Canvas::Unit8), Canvas::Unit0_6F,
                                              Canvas::Unit0_6F),
                                brush.Get());
        target->SetTransform(previous);
    }
    /// The battery: four bars inside a rounded case, amber when low, a bolt while charging.
    static constexpr float BatteryWidth = Canvas::Unit24;
    void DrawBattery(const ControllerBattery& battery, float x, float middle) {
        const float height = Canvas::Unit12, top = middle - height / Canvas::Unit2,
                    body = BatteryWidth - Canvas::Unit3;
        const bool low = battery.level < 0.2f && !battery.charging;
        const unsigned color = low ? Palette::Warning : Palette::Text;
        Outline(x + Canvas::Unit0_5F, top + Canvas::Unit0_5F, body - Canvas::Unit1,
                height - Canvas::Unit1, Canvas::Unit3, color, Canvas::Unit0_75F, Canvas::Unit1_2F);
        Round(x + body, middle - Canvas::Unit2_5F, Canvas::Unit2, Canvas::Unit5, Canvas::Unit1,
              color, Canvas::Unit0_75F);
        // Bars from the level, at least one while there is charge left.
        const int bars = battery.level <= 0 ? 0 : std::clamp(static_cast<int>(std::ceil(battery.level * 4 - 0.01f)), 1, 4);
        const float inner = body - Canvas::Unit6, gap = Canvas::Unit1_5F,
                    bar = (inner - Canvas::Unit3 * gap) / Canvas::Unit4;
        for (int i = 0; i < bars; ++i)
            Round(x + Canvas::Unit3 + i * (bar + gap), top + Canvas::Unit3, bar,
                  height - Canvas::Unit6, Canvas::Unit0_8F, color);
        if (battery.charging) {
            // A bolt over the case, cut out of it with the canvas colour.
            const float cx = x + body / Canvas::Unit2, cy = middle;
            ComPtr<ID2D1Factory> factory;
            target->GetFactory(&factory);
            ComPtr<ID2D1PathGeometry> bolt;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory->CreatePathGeometry(&bolt)) && SUCCEEDED(bolt->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(cx + 1.5f, cy - 7), D2D1_FIGURE_BEGIN_FILLED);
                const D2D1_POINT_2F points[] = {{cx - 3.5f, cy + 1}, {cx - 0.2f, cy + 1}, {cx - 1.5f, cy + 7},
                                                {cx + 3.5f, cy - 1}, {cx + 0.2f, cy - 1}};
                sink->AddLines(points, 5);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(sink->Close())) {
                    brush->SetColor(Ink(Palette::Canvas));
                    target->DrawGeometry(bolt.Get(), brush.Get(), 2.4f);
                    brush->SetColor(Ink(Palette::Text));
                    target->FillGeometry(bolt.Get(), brush.Get());
                }
            }
        }
    }
    static std::wstring TitlesCount(std::size_t count) {
        return I18n::Format(count == 1 ? T::TitlesCountOne : T::TitlesCountOther, {std::to_wstring(count)});
    }
    /// A tile: an entry, in the rail or in the collection.
    struct TileKey {
        std::size_t index{static_cast<std::size_t>(-1)};
        bool rail{};
        bool operator==(const TileKey&) const = default;
    };
    /// The focus moving to a tile: the one it leaves eases out while the new one eases in.
    void FocusTile(std::size_t index, bool rail, Tween::Clock::time_point now) {
        const TileKey key{index, rail};
        if (!rows_shown) {
            focused_tile = key;
            left_tile = {};
            focus_in.Snap(1);
            focus_out.Snap(0);
        } else if (key != focused_tile) {
            left_tile = focused_tile;
            focused_tile = key;
            focus_out.Snap(focus_in.Value(now));
            focus_out.Set(0, now);
            focus_in.Snap(0);
            focus_in.Set(1, now);
        }
    }
    float TileAmount(TileKey tile, Tween::Clock::time_point now) const {
        return tile == focused_tile ? focus_in.Value(now)
               : tile == left_tile  ? focus_out.Value(now)
                                    : Canvas::Unit0_0F;
    }
    const Compatibility::Report& CompatibilityFor(const LibraryView& view,
                                                  const LibraryEntry& entry) const {
        static const Compatibility::Report untested;
        return view.compatibility ? Compatibility::Lookup(*view.compatibility, entry.program_id)
                                  : untested;
    }
    static T CompatibilityText(Compatibility::Status status) {
        switch (status) {
        case Compatibility::Status::Playable:
            return T::CompatibilityPlayable;
        case Compatibility::Status::Issues:
            return T::CompatibilityIssues;
        case Compatibility::Status::Broken:
            return T::CompatibilityBroken;
        default:
            return T::CompatibilityUntested;
        }
    }
    std::wstring CompatibilityLabel(const Compatibility::Report& report) const {
        if (report.console.empty())
            return std::wstring{Tr(T::CompatibilityUntested)};
        return I18n::Format(T::CompatibilityReport,
                            {Tr(CompatibilityText(report.status)),
                             std::wstring{winrt::to_hstring(report.fps_note)},
                             std::wstring{winrt::to_hstring(report.console)}});
    }
    void DrawCompatibility(Compatibility::Status status, float cx, float cy, float alpha) {
        unsigned color = Palette::TextSecondary;
        if (status == Compatibility::Status::Playable)
            color = Palette::CompatibilityPlayable;
        else if (status == Compatibility::Status::Issues)
            color = Palette::Warning;
        else if (status == Compatibility::Status::Broken)
            color = Palette::CompatibilityBroken;
        Round(cx - 12, cy - 12, 24, 24, 12, Palette::Canvas, 0.96f * alpha);
        brush->SetColor(Ink(color, alpha));
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9, 9), brush.Get(), 1.7f);
        const auto line = [&](float ax, float ay, float bx, float by) {
            target->DrawLine(D2D1::Point2F(cx + ax, cy + ay), D2D1::Point2F(cx + bx, cy + by),
                             brush.Get(), 1.7f);
        };
        if (status == Compatibility::Status::Playable) {
            line(-5, 0, -1, 4);
            line(-1, 4, 5, -4);
        } else if (status == Compatibility::Status::Broken) {
            line(-4, -4, 4, 4);
            line(-4, 4, 4, -4);
        } else {
            if (status == Compatibility::Status::Issues)
                line(0, -5, 0, 1);
            else {
                // A small question hook, drawn from line segments rather than a font glyph.
                line(-4, -3, -2, -5);
                line(-2, -5, 2, -5);
                line(2, -5, 4, -3);
                line(4, -3, 4, -1);
                line(4, -1, 0, 1);
                line(0, 1, 0, 2);
            }
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 5), 1, 1), brush.Get());
        }
    }
    /// The focused game, large: who makes it, its title, what is known about it, and its actions.
    void DrawHero(const LibraryView& view, const LibraryEntry& game, Tween::Clock::time_point now) {
        using namespace LibraryLayout;
        if (game.launch_path != hero_path) {
            hero_path = game.launch_path;
            hero.Snap(0);
            hero.Set(1, now);
        }
        const float shown = hero.Value(now);
        const float lift = (Canvas::Unit1 - shown) * Canvas::Unit10;
        constexpr float width = Canvas::Unit860;
        const unsigned lines = MeasureType(game.name, TypeRole::Hero) > width ? 2 : 1;
        const float title = Canvas::Unit304 - Canvas::Unit64_0F * lines + lift;
        std::wstring kicker{L"NINTENDO SWITCH"};
        if (!game.developer.empty()) kicker += L"  ·  " + Upper(game.developer);
        Type(kicker, TypeRole::Caps, Margin, title - Canvas::Unit24, width, Palette::TextSecondary,
             shown);
        Type(game.name, TypeRole::Hero, Margin, title, width, Palette::Text, shown, TextAlign::Leading, lines);
        // Only what the game and the play history say; a chip that would not fit is left out.
        float x = Margin;
        const float y = Canvas::Unit314 + lift;
        const auto chip = [&](std::wstring_view text, unsigned color = Palette::TextHint) {
            if (x + MeasureType(text, TypeRole::Label) + 24 > 1280 - Margin) return;
            x += Chip(text, x, y, color, shown) + 8;
        };
        if (!view.configuration.status.keys_ready) chip(Tr(T::ImportKeysFirst), Palette::Warning);
        if (!game.display_version.empty()) chip(L"v" + game.display_version);
        if (game.last_played > 0) {
            if (view.system.settings.track_play_time && game.play_seconds > 0)
                chip(FormatPlayTime(game.play_seconds));
            chip(I18n::Format(T::LastPlayed, {FormatLastPlayed(game.last_played, UnixNow(), LocalUtcOffset())}));
        }
        const auto extension = game.relative_path.extension().wstring();
        if (extension.size() > 1) chip(Upper(std::wstring_view{extension}.substr(1)));
        if (!game.source_name.empty()) chip(game.source_name);
        if (!game.pulse_saved.empty())
            chip(I18n::Format(T::PulseBadge, {game.pulse_saved}),
                 game.pulse_current ? Palette::Text : Palette::TextSecondary);
        Type(CompatibilityLabel(CompatibilityFor(view, game)), TypeRole::Label, Margin,
             342 + lift, 1152, Palette::TextHint, shown);
        const auto play = PromptKey(Navigation::Play);
        const auto menu = PromptKey(Navigation::Options);
        const float play_width = PillButton(Tr(T::Play), play.letters, play.color, Margin, PillTop, true, PillHeight);
        AddHit(Margin, PillTop, play_width, PillHeight, LibraryHit::Play);
        const float options_x = Margin + play_width + Canvas::Unit14;
        const float options_width = PillButton(Tr(T::Options), menu.letters, menu.color, options_x, PillTop,
                                               false, PillHeight);
        AddHit(options_x, PillTop, options_width, PillHeight, LibraryHit::Options);
        if (!view.notice.empty()) {
            const float notice_x = options_x + options_width + Canvas::Unit24;
            Type(view.notice, TypeRole::Body, notice_x, PillTop + Canvas::Unit11,
                 Canvas::Unit1280 - Margin - notice_x, Palette::Warning);
        }
    }
    /// The rows of covers: the focused one where the hero ends, the rows above fading under it.
    void DrawRows(const LibraryView& view, Tween::Clock::time_point now) {
        using namespace LibraryLayout;
        const auto& entries = view.scan.entries;
        const std::size_t count = entries.size();
        const std::size_t rail = RailCount(entries);
        const Focus focus{view.selected, view.rail && view.selected < rail};
        const float settled = RowContentTop(RowOf(focus, rail), rail);
        if (rows_shown) scroll.Set(settled, now);
        else scroll.Snap(settled);
        FocusTile(focus.selected, focus.rail, now);
        rows_shown = true;
        const float offset = scroll.Value(now);
        const std::size_t rows = (rail ? 1 : 0) + (count + Columns - 1) / Columns;
        struct Lifted {
            const LibraryEntry* entry;
            float x, y, amount, alpha;
        };
        std::array<Lifted, 2> lifted{}; // The cover leaving focus, then the focused one.
        for (std::size_t row = 0; row < rows; ++row) {
            const float top = RowTop + RowContentTop(row, rail) - offset;
            if (top > 720 || top < RowTop - 2 * RowHeight) continue;
            const float alpha = RowAlpha(top);
            if (alpha <= 0) continue;
            const bool in_rail = rail && row == 0;
            const std::size_t first = in_rail ? 0 : (row - (rail ? 1 : 0)) * Columns;
            const std::size_t end = in_rail ? rail : std::min(count, first + Columns);
            if (in_rail || first == 0) {
                Type(Tr(in_rail ? T::RecentAndPinned : T::FullCollection), TypeRole::HeadlineSmall,
                     Margin, top - HeadingHeight, Canvas::Unit700, Palette::Text, alpha);
                const auto kicker = in_rail ? Upper(TitlesCount(rail))
                                  : focus.rail ? TitlesCount(count)
                                               : std::to_wstring(focus.selected + 1) + L" / " + std::to_wstring(count);
                Type(kicker, in_rail ? TypeRole::Caps : TypeRole::Label,
                     Canvas::Unit1280 - Margin - Canvas::Unit240,
                     top - HeadingHeight + Canvas::Unit8, Canvas::Unit240, Palette::TextSecondary,
                     alpha, TextAlign::Trailing);
            }
            for (std::size_t i = first; i < end; ++i) {
                const float x = TileLeft(i - first);
                const TileKey tile{i, in_rail};
                const float amount = TileAmount(tile, now);
                if (amount > 0.001f) lifted[tile == focused_tile ? 1 : 0] = {&entries[i], x, top, amount, alpha};
                else DrawTile(view, entries[i], x, top, 0, alpha);
                // Rows fading under the hero do not take presses.
                if (alpha > 0.5f)
                    AddHit(x, top, Tile, Tile + Canvas::Unit44,
                           in_rail ? LibraryHit::RailTile : LibraryHit::Tile, i);
            }
        }
        for (const auto& tile : lifted)
            if (tile.entry) DrawTile(view, *tile.entry, tile.x, tile.y, tile.amount, tile.alpha);
        // The last row's covers end under the prompts: say there is more down there.
        if (rows && RowTop + RowContentTop(rows - 1, rail) - offset + Tile > FooterTop)
            MoreBelow(Canvas::Unit1280 - Margin / Canvas::Unit2, FooterTop - Canvas::Unit20,
                      Canvas::Unit0_8F);
    }
    /// Rows settle at RowTop; above it they fade out before reaching the hero's actions.
    static float RowAlpha(float top) {
        return std::clamp((top - Canvas::Unit330) / Canvas::Unit144, Canvas::Unit0_0F,
                          Canvas::Unit1_0F);
    }
    /// A cover, its title and its meta line; `amount` (0..1) of the way into focus: larger,
    /// lifted, ringed and with a brighter title.
    void DrawTile(const LibraryView& view, const LibraryEntry& entry, float x, float y, float amount, float alpha) {
        using LibraryLayout::Tile;
        const float size = Tile * (Canvas::Unit1 + Canvas::Unit0_06F * amount);
        const float left = x + (Tile - size) / Canvas::Unit2;
        const float top = y + (Tile - size) / Canvas::Unit2 - Canvas::Unit8 * amount;
        FocusGlow(left, top, size, size, Theme().cover_radius, amount * alpha);
        Cover(entry, left, top, size, alpha);
        DrawCompatibility(CompatibilityFor(view, entry).status, left + 18, top + 18, alpha);
        if (entry.pinned) {
            // A pin's head: a white dot in a graphite disc, at the top right.
            const float cx = left + size - Canvas::Unit18, cy = top + Canvas::Unit18;
            Round(cx - Canvas::Unit10, cy - Canvas::Unit10, Canvas::Unit20, Canvas::Unit20,
                  Canvas::Unit10, Palette::Graphite, Canvas::Unit0_9F * alpha);
            Round(cx - Canvas::Unit4, cy - Canvas::Unit4, Canvas::Unit8, Canvas::Unit8,
                  Canvas::Unit4, Palette::Text, alpha);
        }
        const float text = y + Tile + Canvas::Unit10 + Canvas::Unit6 * amount;
        Type(entry.name, TypeRole::CoverTitle, x, text, Tile,
             amount > 0.5f ? Palette::Text : Palette::TextSecondary, alpha);
        std::wstring under = entry.display_version.empty() ? L"" : L"v" + entry.display_version;
        if (view.system.settings.track_play_time && entry.play_seconds >= 60) {
            if (!under.empty()) under += L" · ";
            under += FormatPlayTimeShort(entry.play_seconds);
        }
        if (!entry.pulse_saved.empty()) under += under.empty() ? L"Pulse" : L" · Pulse";
        if (!under.empty())
            Type(under, TypeRole::Label, x, text + Canvas::Unit20, Tile, Palette::TextSecondary,
                 alpha * (Canvas::Unit0_7F + Canvas::Unit0_3F * amount));
    }
    /// The Library tab: the heading and the filter on one line, the filtered collection under them.
    void DrawShelf(const LibraryView& view, Tween::Clock::time_point now) {
        using namespace LibraryLayout;
        const auto& entries = view.scan.entries;
        static const std::vector<std::size_t> none;
        const auto& shelf = view.shelf ? *view.shelf : none;
        const bool import = HasImportTile(view.filter);
        const std::size_t count = shelf.size() + (import ? 1 : 0);
        Type(Tr(T::FullCollection), TypeRole::HeadlineLarge, Margin,
             ShelfHeadingTop - Canvas::Unit6, Canvas::Unit560, Palette::Text);
        Type(TitlesCount(shelf.size()), TypeRole::Body, Margin, ShelfHeadingTop + Canvas::Unit46,
             Canvas::Unit400, Palette::TextSecondary);
        if (!view.filter_focus && view.shelf_focus < shelf.size()) {
            Type(CompatibilityLabel(CompatibilityFor(view, entries[shelf[view.shelf_focus]])),
                 TypeRole::Label, Margin + 240, ShelfHeadingTop + 50, 900, Palette::TextHint);
        }
        const std::vector<std::wstring_view> filters{Tr(T::FilterAll),    Tr(T::FilterPlayed), Tr(T::FilterAlphabetical),
                                                     Tr(T::FilterPinned), Tr(T::FilterRecent), Tr(T::FilterPulse)};
        const auto active = static_cast<std::size_t>(view.filter);
        if (rows_shown) filter_slide.Set(static_cast<float>(active), now);
        else filter_slide.Snap(static_cast<float>(active));
        Segmented(filters, 1280 - Margin - SegmentedWidth(filters), FilterTop, FilterHeight, filter_slide.Value(now),
                  active, view.filter_focus, LibraryHit::Filter);
        const std::size_t focus = std::min(view.shelf_focus, count ? count - 1 : 0);
        const float settled = ShelfScroll(focus);
        if (rows_shown) scroll.Set(settled, now);
        else scroll.Snap(settled);
        // On the filter bar no cover is lifted.
        FocusTile(view.filter_focus ? static_cast<std::size_t>(-1) : focus, false, now);
        rows_shown = true;
        if (count == 0) {
            const auto message = view.filter == Filter::Pinned
                                     ? I18n::Format(T::FilterPinnedEmpty, {std::wstring{PromptKey(Navigation::Pin).letters}})
                                     : std::wstring{Tr(T::FilterEmpty)};
            Type(message, TypeRole::BodyLarge, Margin, ShelfTop + Canvas::Unit20, Canvas::Unit900,
                 Palette::TextSecondary, Canvas::Unit1, TextAlign::Leading, 2);
            return;
        }
        const float offset = scroll.Value(now);
        struct Lifted {
            const LibraryEntry* entry;
            float x, y, amount, alpha;
        };
        std::array<Lifted, 2> lifted{}; // The cover leaving focus, then the focused one.
        for (std::size_t i = 0; i < count; ++i) {
            const float x = TileLeft(i % Columns);
            const float top = ShelfTop + static_cast<float>(i / Columns) * ShelfRowHeight - offset;
            if (top < ShelfTop - ShelfRowHeight || top > FooterTop) continue;
            // Rows fade out under the heading and into the prompts.
            const float alpha = std::clamp((top - (ShelfTop - Canvas::Unit150)) / Canvas::Unit150,
                                           Canvas::Unit0_0F, Canvas::Unit1_0F) *
                                std::clamp((FooterTop - Canvas::Unit8 - top) / Canvas::Unit130,
                                           Canvas::Unit0_0F, Canvas::Unit1_0F);
            if (alpha <= 0) continue;
            const TileKey tile{i, false};
            const float amount = TileAmount(tile, now);
            if (i == shelf.size()) DrawImportTile(x, top, amount, alpha);
            else if (amount > 0.001f) lifted[tile == focused_tile ? 1 : 0] = {&entries[shelf[i]], x, top, amount, alpha};
            else DrawTile(view, entries[shelf[i]], x, top, 0, alpha);
            if (alpha > 0.5f)
                AddHit(x, top, Tile, Tile + Canvas::Unit44,
                       i == shelf.size() ? LibraryHit::Import : LibraryHit::ShelfTile, i);
        }
        for (const auto& tile : lifted)
            if (tile.entry) DrawTile(view, *tile.entry, tile.x, tile.y, tile.amount, tile.alpha);
        // The last row's covers end under the prompts: say there is more down there.
        if (ShelfTop + static_cast<float>((count - 1) / Columns) * ShelfRowHeight - offset + Tile > FooterTop)
            MoreBelow(Canvas::Unit1280 - Margin / Canvas::Unit2, FooterTop - Canvas::Unit20,
                      Canvas::Unit0_8F);
    }
    /// The collection's last tile: opens the file manager to add games.
    void DrawImportTile(float x, float y, float amount, float alpha) {
        using LibraryLayout::Tile;
        const float size = Tile * (Canvas::Unit1 + Canvas::Unit0_06F * amount);
        const float left = x + (Tile - size) / Canvas::Unit2;
        const float top = y + (Tile - size) / Canvas::Unit2 - Canvas::Unit8 * amount;
        FocusGlow(left, top, size, size, Theme().cover_radius, amount * alpha);
        Round(left, top, size, size, Canvas::Unit12, Palette::Graphite, Canvas::Unit0_55F * alpha);
        Outline(left + Canvas::Unit0_5F, top + Canvas::Unit0_5F, size - Canvas::Unit1,
                size - Canvas::Unit1, Theme().cover_radius - Canvas::Unit0_5F, Palette::White,
                Canvas::Unit0_16F * alpha);
        const float cx = left + size / Canvas::Unit2,
                    cy = top + size / Canvas::Unit2 - Canvas::Unit8;
        Round(cx - Canvas::Unit14, cy - Canvas::Unit1_5F, Canvas::Unit28, Canvas::Unit3,
              Canvas::Unit1_5F, Palette::Text, alpha);
        Round(cx - Canvas::Unit1_5F, cy - Canvas::Unit14, Canvas::Unit3, Canvas::Unit28,
              Canvas::Unit1_5F, Palette::Text, alpha);
        Type(L"NSP · XCI", TypeRole::Caps, left, cy + Canvas::Unit26, size, Palette::TextSecondary,
             alpha, TextAlign::Center);
        const float text = y + Tile + Canvas::Unit10 + Canvas::Unit6 * amount;
        Type(Tr(T::ImportTitle), TypeRole::LabelLarge, x, text, Tile,
             amount > 0.5f ? Palette::Text : Palette::TextSecondary, alpha);
        Type(Tr(T::ImportTitleHint), TypeRole::Label, x, text + Canvas::Unit20, Tile,
             Palette::TextSecondary, alpha * (Canvas::Unit0_7F + Canvas::Unit0_3F * amount));
    }
    /// What the tab's buttons do, along the bottom; the installed version at the right.
    void DrawFooter(const LibraryView& view) {
        using namespace LibraryLayout;
        const float y = FooterTop + Canvas::Unit12;
        float x = Margin;
        const auto& entries = view.scan.entries;
        const bool game = !view.loading && view.selected < entries.size();
        // Only what a press does here: moving with the d-pad and the tabs' bumpers (shown on
        // the header) go without saying.
        if (view.tab == LibraryTab::Settings) {
            const auto& configuration = view.configuration;
            const bool change = view.system.open || view.controller_panel;
            x += ControlPrompt(Navigation::Play, Tr(change ? T::Change : T::Open), x, y);
            ControlPrompt(Navigation::Back, Tr(configuration.busy ? T::Cancel : T::Back), x, y);
            // The version is on the page's kicker; its notices take the corner.
            std::wstring notice;
            unsigned color = Palette::TextHint;
            if (configuration.busy) {
                notice = I18n::Format(T::Importing, {std::to_wstring(configuration.completed),
                                                     std::to_wstring(configuration.total)});
            } else if (view.system.open && !view.system.notice.empty()) {
                notice = view.system.notice;
            } else if (view.controller_panel && !view.panel.notice.empty()) {
                notice = view.panel.notice;
            } else if (!configuration.notice.empty()) {
                notice = configuration.notice;
                color = Palette::Warning;
            }
            Type(notice, TypeRole::Label, Canvas::Unit1280 - Margin - Canvas::Unit600,
                 y + Canvas::Unit8, Canvas::Unit600, color, Canvas::Unit1, TextAlign::Trailing);
            return;
        } else {
            const bool shelf = view.tab == LibraryTab::Library;
            const bool import = shelf && view.shelf && view.shelf_focus >= view.shelf->size();
            if (!(shelf && view.filter_focus)) {
                // On Home the hero's pills already show Play and Options with their buttons.
                if (import) x += ControlPrompt(Navigation::Play, Tr(T::Import), x, y);
                else if (game && shelf) x += ControlPrompt(Navigation::Play, Tr(T::Play), x, y);
                if (game && !import) {
                    if (shelf) x += ControlPrompt(Navigation::Options, Tr(T::Options), x, y);
                    x += ControlPrompt(Navigation::Pin, Tr(entries[view.selected].pinned ? T::Unpin : T::Pin), x, y);
                }
            }
            x += ControlPrompt(Navigation::Rescan, Tr(T::Refresh), x, y);
            ControlPrompt(Navigation::Back, Tr(shelf ? T::Back : T::Exit), x, y);
        }
        Type(version_text, TypeRole::Label, Canvas::Unit1280 - Margin - Canvas::Unit160,
             y + Canvas::Unit8, Canvas::Unit160, Palette::TextSecondary, Canvas::Unit0_7F,
             TextAlign::Trailing);
    }
    void DrawControllerPanel(const LibraryView& view) {
        const auto& options = view.options;
        const auto& panel = view.panel;
        brush->SetColor(Ink(Palette::Black, Canvas::Unit0_72F));
        target->FillRectangle(
            D2D1::RectF(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720),
            brush.Get());
        Round(Canvas::Unit620, Canvas::Unit133, Canvas::Unit586, Canvas::Unit487, Canvas::Unit24,
              Palette::Surface);
        Text(Tr(T::ControllerTitle), Canvas::Unit658, Canvas::Unit168, Canvas::Unit34,
             Palette::Text, Canvas::Unit490, Canvas::Unit55, true);
        Text(Tr(T::ControllerSubtitle), Canvas::Unit658, Canvas::Unit227, Canvas::Unit21,
             Palette::TextSecondary, Canvas::Unit490);
        const unsigned offset = panel.xbox ? 0 : 1;
        const auto active = SelectController(panel.devices, options.controller_id);
        const bool nintendo = active && panel.devices[*active].nintendo;
        for (unsigned row = 0; row < 3 + offset; ++row) {
            const float y = ControllerRowTop + row * ControllerRowHeight;
            Round(Canvas::Unit648, y, Canvas::Unit530, Canvas::Unit56, Canvas::Unit14,
                  row == view.controller_row ? Palette::RowActive : Palette::Row);
            const bool device = offset && row == 0;
            const bool type = row == offset;
            const bool face = row == offset + 1;
            Text(Tr(device ? T::ControllerDevice
                    : type ? T::ControllerSeenAs
                    : face ? T::FaceButtons
                           : T::DeadZone),
                 Canvas::Unit669, y + Canvas::Unit2, Canvas::Unit19, Palette::Text, Canvas::Unit400,
                 Canvas::Unit28, true);
            if (device || type)
                Text(device ? L"v" : L"<  >", Canvas::Unit1080, y + Canvas::Unit2, Canvas::Unit19,
                     Palette::Text, Canvas::Unit80, Canvas::Unit28, true, false, false, true);
            std::wstring value;
            if (device) {
                const auto label = ControllerLabel(panel.devices, options.controller_id);
                value = options.controller_id.empty() ? I18n::Format(T::AutomaticValue, {label}) : label;
            } else if (type) {
                value = Tr(ControllerStyleText(options.style));
            } else if (face) {
                value = Tr(nintendo ? T::NintendoFaceButtons :
                           options.swap_face_buttons ? T::ByPosition : T::ByLetter);
            } else {
                value = std::to_wstring(static_cast<int>(options.deadzone * 100)) + L"%";
            }
            Text(value, Canvas::Unit669, y + Canvas::Unit28, Canvas::Unit18, Palette::Accent,
                 Canvas::Unit460, Canvas::Unit26);
        }
        if (!panel.xbox) {
            Round(Canvas::Unit648, Canvas::Unit510, Canvas::Unit530, Canvas::Unit34, Canvas::Unit10,
                  view.controller_row == 4 ? Palette::RowActive : Palette::Row);
            Text(std::wstring{Tr(T::SetUpKeyboard)} + L"   >", Canvas::Unit669, Canvas::Unit512,
                 Canvas::Unit19, Palette::Accent, Canvas::Unit460, Canvas::Unit30, true);
        }
        ControlPrompt(Navigation::Play, Tr(T::Change), Canvas::Unit658, Canvas::Unit551);
        ControlPrompt(Navigation::Back, Tr(T::Back), Canvas::Unit1018, Canvas::Unit551);
        if (!panel.notice.empty())
            Text(panel.notice, Canvas::Unit658, Canvas::Unit594, Canvas::Unit16, Palette::Accent,
                 Canvas::Unit515, Canvas::Unit25);
        if (panel.expanded) DrawControllerChoices(view, false);
    }
    /// The controllers to choose from; `dim` darkens what is under it.
    void DrawControllerChoices(const LibraryView& view, bool dim) {
        const auto& panel = view.panel;
        if (dim)
            Round(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720, Canvas::Unit0,
                  Palette::Canvas, Canvas::Unit0_6F);
        {
            const size_t first = ControllerChoiceFirst(panel);
            const size_t count = panel.devices.size() + 2;
            const size_t end = std::min(count, first + ControllerVisibleChoices);
            Round(Canvas::Unit648, ControllerChoiceTop - Canvas::Unit4, Canvas::Unit530,
                  (end - first) * ControllerChoiceHeight + Canvas::Unit8, Canvas::Unit12,
                  Palette::RowActive);
            for (size_t i = first; i < end; ++i) {
                const float y = ControllerChoiceTop + (i - first) * ControllerChoiceHeight;
                Round(Canvas::Unit652, y, Canvas::Unit522, Canvas::Unit40, Canvas::Unit8,
                      i == panel.choice ? Palette::RowActive : Palette::Row);
                const bool session = i >= 2 && panel.devices[i - 2].id.starts_with(L"session:");
                const bool supported = i < 2 || (bool(panel.devices[i - 2]) && !session);
                std::wstring label;
                if (i == 0) label = Tr(T::AutoFirstController);
                else if (i == 1) label = Tr(T::Keyboard);
                else label = panel.devices[i - 2].name + L" · " +
                    std::wstring{Tr(supported ? (panel.devices[i - 2].wireless ? T::Wireless : T::Usb) :
                                    session ? T::AutomaticOnly : T::NoCompatibleMapping)};
                Text(label, Canvas::Unit665, y + Canvas::Unit6, Canvas::Unit18,
                     supported ? Palette::Text : Palette::TextSecondary, Canvas::Unit480,
                     Canvas::Unit30);
            }
            ScrollIndicator(Canvas::Unit1187, ControllerChoiceTop,
                            ControllerVisibleChoices * ControllerChoiceHeight, first,
                            ControllerVisibleChoices, count);
        }
    }
    void DrawKeyboard(const KeyboardEditor& editor) {
        brush->SetColor(Ink(Palette::Black, Canvas::Unit0_86F));
        target->FillRectangle(
            D2D1::RectF(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720),
            brush.Get());
        Round(Canvas::Unit80, Canvas::Unit88, Canvas::Unit1120, Canvas::Unit584, Canvas::Unit22,
              Palette::Surface);
        Text(Tr(T::KeyboardTitle), Canvas::Unit120, Canvas::Unit118, Canvas::Unit28, Palette::Text,
             Canvas::Unit900, Canvas::Unit44, true);
        Text(Tr(T::KeyboardSubtitle), Canvas::Unit120, Canvas::Unit166, Canvas::Unit17,
             Palette::TextSecondary, Canvas::Unit650, Canvas::Unit30);
        Round(Canvas::Unit1120, Canvas::Unit112, Canvas::Unit48, Canvas::Unit40, Canvas::Unit10,
              Palette::Graphite);
        brush->SetColor(Ink(Palette::TextHint));
        target->DrawLine(D2D1::Point2F(1137, 125), D2D1::Point2F(1151, 139), brush.Get(), 2);
        target->DrawLine(D2D1::Point2F(1151, 125), D2D1::Point2F(1137, 139), brush.Get(), 2);
        Round(Canvas::Unit120, Canvas::Unit210, Canvas::Unit480, Canvas::Unit342, Canvas::Unit16,
              Palette::Row);
        Text(Tr(T::ControllerView), Canvas::Unit143, Canvas::Unit225, Canvas::Unit13,
             Palette::TextSecondary, Canvas::Unit220, Canvas::Unit24, true);
        // Reuse Eden Qt's original Pro Controller contours and button coordinates.
        // Create paths once; fills/strokes/highlights share cached GPU resources.
        if (!controller_paths[0]) {
            using namespace ProControllerGeometry;
            ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
            auto polygon = [&](size_t index, const auto& points, bool mirror, bool symmetric) {
                winrt::check_hresult(factory->CreatePathGeometry(&controller_paths[index]));
                ComPtr<ID2D1GeometrySink> sink;
                winrt::check_hresult(controller_paths[index]->Open(&sink));
                auto point = [&](size_t i, bool reflected) {
                    const float x = points[i * Canvas::Unit2] *
                                    (reflected ? -Canvas::Unit1_0F : Canvas::Unit1_0F);
                    return D2D1::Point2F(350 + x, 390 + points[i * 2 + 1]);
                };
                sink->BeginFigure(point(0, mirror), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t i = 1; i < points.size() / 2; ++i) sink->AddLine(point(i, mirror));
                if (symmetric)
                    for (size_t i = points.size() / 2; i-- > 0;) sink->AddLine(point(i, !mirror));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                winrt::check_hresult(sink->Close());
            };
            polygon(0, pro_body, false, true);
            polygon(1, pro_left_handle, false, false); polygon(2, pro_left_handle, true, false);
            polygon(3, pro_left_trigger, false, false); polygon(4, pro_left_trigger, true, false);
        }
        auto path = [&](size_t i, unsigned color) {
            brush->SetColor(Ink(color));
            target->FillGeometry(controller_paths[i].Get(), brush.Get());
            brush->SetColor(Ink(Palette::ControllerOutline));
            target->DrawGeometry(controller_paths[i].Get(), brush.Get(), 1.2f);
        };
        const size_t action = editor.selected;
        path(1, Palette::Graphite); path(2, Palette::Graphite); path(0, Palette::Graphite);
        path(3, action == 6 ? Palette::Accent : Palette::RowActive); path(4, action == 7 ? Palette::Accent : Palette::RowActive);
        auto circle = [&](float x, float y, float radius, const wchar_t* label, bool active,
                          float font = Canvas::Unit16) {
            const auto shape = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
            brush->SetColor(Ink(active ? Palette::Accent : Palette::Row));
            target->FillEllipse(shape, brush.Get());
            brush->SetColor(Ink(active ? Palette::Text : Palette::ControllerOutline));
            target->DrawEllipse(shape, brush.Get(), 1.4f);
            Text(label, x - radius, y - radius, font,
                 active ? Palette::OnAccent : Palette::TextHint, radius * Canvas::Unit2,
                 radius * Canvas::Unit2, true, false, true);
        };
        auto stick = [&](float x, float y, const wchar_t* label, bool active) {
            circle(x, y, 32, L"", active);
            circle(x, y, 22, label, false, 12);
        };
        stick(239, 335, L"L3", action == 4 || (action >= 20 && action < 24));
        stick(401, 390, L"R3", action == 5 || action >= 24);
        circle(486, 334, 15, L"A", action == 0); circle(455, 365, 15, L"B", action == 1);
        circle(455, 303, 15, L"X", action == 2); circle(424, 334, 15, L"Y", action == 3);
        Round(Canvas::Unit278, Canvas::Unit359, Canvas::Unit22, Canvas::Unit62, Canvas::Unit4,
              Palette::Row);
        Round(Canvas::Unit258, Canvas::Unit379, Canvas::Unit62, Canvas::Unit22, Canvas::Unit4,
              Palette::Row);
        for (size_t i = 12; i <= 15; ++i) {
            const auto tile = KeyboardDiagramTile(i);
            Round(tile.x, tile.y, tile.w, tile.h, Canvas::Unit3,
                  action == i ? Palette::Accent : Palette::RowActive);
        }
        circle(300, 304, 10, L"-", action == 11, 14); circle(400, 304, 10, L"+", action == 10, 14);
        circle(379, 334, 10, L"", action == 18);
        brush->SetColor(Ink(action == 18 ? Palette::OnAccent : Palette::TextHint));
        target->DrawLine(D2D1::Point2F(374, 334), D2D1::Point2F(379, 330), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(379, 330), D2D1::Point2F(384, 334), brush.Get(), 1.4f);
        target->DrawRectangle(
            D2D1::RectF(Canvas::Unit376, Canvas::Unit334, Canvas::Unit382, Canvas::Unit339),
            brush.Get(), 1.2f);
        Round(Canvas::Unit314, Canvas::Unit327, Canvas::Unit14, Canvas::Unit14, Canvas::Unit3,
              action == 19 ? Palette::Accent : Palette::Row);
        brush->SetColor(Ink(Palette::TextSecondary));
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(Canvas::Unit321, Canvas::Unit334),
                                          Canvas::Unit4, Canvas::Unit4),
                            brush.Get(), 1.2f);
        for (const auto i : {size_t{8}, size_t{9}}) {
            const auto tile = KeyboardDiagramTile(i);
            Round(tile.x, tile.y, tile.w, tile.h, Canvas::Unit8,
                  action == i ? Palette::Accent : Palette::Graphite);
            Text(i == 8 ? L"ZL" : L"ZR", tile.x, tile.y, Canvas::Unit14,
                 action == i ? Palette::OnAccent : Palette::TextHint, tile.w, tile.h, true, false,
                 true);
        }
        Text(L"L", Canvas::Unit220, Canvas::Unit252, Canvas::Unit14,
             action == 6 ? Palette::OnAccent : Palette::TextHint, Canvas::Unit60, Canvas::Unit22,
             true, false, true);
        Text(L"R", Canvas::Unit450, Canvas::Unit252, Canvas::Unit14,
             action == 7 ? Palette::OnAccent : Palette::TextHint, Canvas::Unit40, Canvas::Unit22,
             true, false, true);
        Round(Canvas::Unit120, Canvas::Unit564, Canvas::Unit480, Canvas::Unit67, Canvas::Unit12,
              Palette::RowActive);
        Text(Tr(T::SelectedBinding), Canvas::Unit140, Canvas::Unit574, Canvas::Unit11,
             Palette::TextSecondary, Canvas::Unit300, Canvas::Unit19, true);
        Text(KeyboardActionName(action), Canvas::Unit140, Canvas::Unit594, Canvas::Unit20,
             Palette::Text, Canvas::Unit285, Canvas::Unit30, true);
        Round(Canvas::Unit436, Canvas::Unit579, Canvas::Unit142, Canvas::Unit38, Canvas::Unit8,
              Palette::Row);
        Text(editor.capturing ? std::wstring{Tr(T::PressAKey)}
                              : KeyboardKeyName(editor.bindings[action]),
             Canvas::Unit436, Canvas::Unit579, Canvas::Unit16, Palette::Accent, Canvas::Unit142,
             Canvas::Unit38, true, false, true);
        const std::array<std::wstring_view, 3> pages{Tr(T::KeyPageButtons), Tr(T::KeyPageSystem), Tr(T::KeyPageSticks)};
        for (unsigned page = 0; page < pages.size(); ++page) {
            const float x = Canvas::Unit635 + page * Canvas::Unit174_0F;
            Round(x, Canvas::Unit203, Canvas::Unit166, Canvas::Unit36, Canvas::Unit9,
                  page == editor.page ? Palette::RowActive : Palette::Graphite);
            Text(pages[page], x, Canvas::Unit203, Canvas::Unit15,
                 page == editor.page ? Palette::Text : Palette::TextSecondary, Canvas::Unit166,
                 Canvas::Unit36, true, false, true);
        }
        for (const size_t i : KeyboardPageActions(editor.page)) {
            const auto tile = KeyboardTileFor(i);
            Round(tile.x, tile.y, tile.w, tile.h, Canvas::Unit9,
                  i == action ? Palette::RowActive : Palette::Row);
            Text(KeyboardActionName(i), tile.x + Canvas::Unit14, tile.y, Canvas::Unit15,
                 Palette::TextHint, Canvas::Unit136, tile.h, false, false, true);
            Round(tile.x + Canvas::Unit153, tile.y + Canvas::Unit9, Canvas::Unit86, Canvas::Unit27,
                  Canvas::Unit6, Palette::Row);
            Text(i == action && editor.capturing ? std::wstring{L"..."}
                                                 : KeyboardKeyName(editor.bindings[i]),
                 tile.x + Canvas::Unit153, tile.y + Canvas::Unit9, Canvas::Unit14,
                 i == action ? Palette::Accent : Palette::TextSecondary, Canvas::Unit86,
                 Canvas::Unit27, true, false, true);
        }
        Round(Canvas::Unit635, Canvas::Unit603, Canvas::Unit155, Canvas::Unit36, Canvas::Unit8,
              Palette::Graphite);
        Text(Tr(T::ClearBinding), Canvas::Unit635, Canvas::Unit603, Canvas::Unit15,
             Palette::TextHint, Canvas::Unit155, Canvas::Unit36, false, false, true);
        Round(Canvas::Unit806, Canvas::Unit603, Canvas::Unit170, Canvas::Unit36, Canvas::Unit8,
              Palette::Graphite);
        Text(Tr(T::RestoreDefaults), Canvas::Unit806, Canvas::Unit603, Canvas::Unit15,
             Palette::TextHint, Canvas::Unit170, Canvas::Unit36, false, false, true);
        if (!editor.notice.empty())
            Text(editor.notice, Canvas::Unit990, Canvas::Unit609, Canvas::Unit13,
                 Palette::TextSecondary, Canvas::Unit172, Canvas::Unit30);
        Text(Tr(editor.capturing ? T::WaitingForKey : T::KeyboardHelp), Canvas::Unit120,
             Canvas::Unit644, Canvas::Unit13, Palette::TextSecondary,
             prompt_family == PromptFamily::Keyboard ? Canvas::Unit1030_0F : Canvas::Unit860_0F,
             Canvas::Unit22);
        if (prompt_family != PromptFamily::Keyboard)
            ControlPrompt(Navigation::Back, Tr(T::Back), Canvas::Unit1010, Canvas::Unit637);
    }
    ComPtr<ID2D1Bitmap1> LoadAssetBitmap(const std::filesystem::path& relative) {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (asset_root.empty()) return bitmap;
        try {
            ComPtr<IWICBitmapDecoder> decoder;
            const auto path = asset_root / relative;
            winrt::check_hresult(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, &frame));
            ComPtr<IWICFormatConverter> converter;
            winrt::check_hresult(wic->CreateFormatConverter(&converter));
            winrt::check_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            winrt::check_hresult(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
        } catch (const winrt::hresult_error&) {}
        return bitmap;
    }
    enum class PromptFamily : unsigned { Keyboard, Xbox, Nintendo };
    /// Space is Y on a pad; it has no packaged glyph and is drawn natively like AddFolder (X).
    /// The first five have packaged glyphs; the others are drawn natively (UiPainter::ButtonGlyph).
    /// Pin and Rescan are the library's Y and X (Refresh and AddFolder elsewhere).
    enum class Navigation : unsigned { Play, Explore, Settings, Options, Back, AddFolder, Space, Refresh, Tabs, Pin, Rescan };
    using ConfigurationIcon = UiIcon;
    /// One row of a settings category, as the right side of the Settings tab shows it.
    struct SettingsRow {
        enum class Kind { Open, Choice, Toggle, Segments, Value } kind{};
        std::wstring title, hint, value;
        bool on{};                          ///< Toggle.
        std::vector<std::wstring> segments; ///< Segments, with `on_segment` the chosen one.
        std::size_t on_segment{};
        bool warning{};                     ///< The value needs the player's attention.
    };
    /// The category the Settings tab shows: the open one, or the focused one in the list.
    static std::size_t SettingsCategory(const LibraryView& view) {
        if (view.system.open) return HomeSystem;
        if (view.configuration.files) return HomeFiles;
        if (view.controller_panel) return HomeControllers;
        return std::min<std::size_t>(view.configuration.selected, HomeRowCount - 1);
    }
    std::vector<SettingsRow> SettingsRows(const LibraryView& view, std::size_t category) {
        using Kind = SettingsRow::Kind;
        std::vector<SettingsRow> rows;
        if (category == HomeSystem) {
            for (std::size_t i = 0; i < SystemPage::RowCount; ++i) {
                const auto row = static_cast<SystemPage::Row>(i);
                const auto& settings = view.system.settings;
                SettingsRow item{Kind::Choice, std::wstring{Tr(SystemRowTitle(row))},
                                 std::wstring{Tr(SystemRowHint(row))}, SystemRowValue(view.system, row)};
                switch (row) {
                case SystemPage::UserName: item.kind = Kind::Open; break;
                case SystemPage::Updates: item.kind = Kind::Open; break;
                case SystemPage::ClearCache: item.kind = Kind::Open; break;
                case SystemPage::AdultMods: item.kind = Kind::Toggle; item.on = settings.show_adult_mods; break;
                case SystemPage::PerformanceStats: item.kind = Kind::Toggle; item.on = settings.show_performance_stats; break;
                case SystemPage::PulseSessions: item.kind = Kind::Toggle; item.on = settings.pulse_sessions; break;
                case SystemPage::PlayTime: item.kind = Kind::Toggle; item.on = settings.track_play_time; break;
                case SystemPage::MultipleControllers: item.kind = Kind::Toggle; item.on = settings.multiple_controllers; break;
                default: break;
                }
                rows.push_back(std::move(item));
            }
        } else if (category == HomeControllers) {
            // As the controller panel: [device (PC)], type, A/B/X/Y, dead zone, [keyboard (PC)].
            const auto& options = view.options;
            const auto& panel = view.panel;
            if (!panel.xbox) {
                const auto label = ControllerLabel(panel.devices, options.controller_id);
                rows.push_back({Kind::Open, std::wstring{Tr(T::ControllerDevice)}, {},
                                options.controller_id.empty() ? I18n::Format(T::AutomaticValue, {label}) : label});
            }
            rows.push_back({Kind::Choice, std::wstring{Tr(T::ControllerSeenAs)}, {},
                            std::wstring{Tr(ControllerStyleText(options.style))}});
            const auto active = SelectController(panel.devices, options.controller_id);
            if (active && panel.devices[*active].nintendo) {
                rows.push_back({Kind::Value, std::wstring{Tr(T::FaceButtons)}, {},
                                std::wstring{Tr(T::NintendoFaceButtons)}});
            } else {
                SettingsRow face{Kind::Segments, std::wstring{Tr(T::FaceButtons)}};
                face.segments = {std::wstring{Tr(T::ByLetter)}, std::wstring{Tr(T::ByPosition)}};
                face.on_segment = options.swap_face_buttons ? 1 : 0;
                rows.push_back(std::move(face));
            }
            // The dead zone's three steps (uwp_library.cpp, Zone).
            SettingsRow zone{Kind::Segments, std::wstring{Tr(T::DeadZone)}};
            zone.segments = {L"8%", L"12%", L"18%"};
            zone.on_segment = options.deadzone < 0.10f ? 0 : options.deadzone < 0.15f ? 1 : 2;
            rows.push_back(std::move(zone));
            if (!panel.xbox) rows.push_back({Kind::Open, std::wstring{Tr(T::SetUpKeyboard)}});
        } else {
            const auto& status = view.configuration.status;
            rows.push_back({Kind::Open, std::wstring{Tr(T::AddGamesFolder)}});
            SettingsRow keys{Kind::Open, std::wstring{Tr(T::ImportKeys)}, {},
                             std::wstring{Tr(status.keys_ready ? T::KeysReady : T::KeysMissing)}};
            keys.warning = !status.keys_ready;
            rows.push_back(std::move(keys));
            rows.push_back({Kind::Open, std::wstring{Tr(T::ImportFirmware)}, {},
                            I18n::Format(T::FirmwareFiles, {std::to_wstring(status.firmware_files)})});
            rows.push_back({Kind::Open, std::wstring{Tr(T::ContentUpdates)}});
            rows.push_back({Kind::Open, std::wstring{Tr(T::ContentDlc)}});
            const auto& network = view.configuration.network;
            rows.push_back({Kind::Open, std::wstring{Tr(T::NetworkConnection)},
                network.running ? std::wstring{winrt::to_hstring(network.url)} : std::wstring{Tr(T::NetworkStopped)},
                std::wstring{Tr(network.running ? T::Open : T::NetworkStart)}});
            const auto& addresses = view.configuration.network_addresses;
            const auto adapter = view.configuration.network_selected;
            // A 169.254 address means the router gave none: phones cannot reach it, so it warns.
            const bool link_local = !addresses.empty() && addresses[adapter].link_local;
            SettingsRow address{Kind::Open, std::wstring{Tr(T::NetworkAdapter)},
                addresses.empty() ? std::wstring{Tr(T::NetworkUnavailable)} :
                    std::wstring{winrt::to_hstring(addresses[adapter].name)} +
                    (link_local ? L" · " + std::wstring{Tr(T::NetworkLinkLocal)} : std::wstring{}),
                addresses.empty() ? std::wstring{} : std::wstring{winrt::to_hstring(addresses[adapter].ipv4)}};
            address.warning = addresses.empty() || link_local;
            rows.push_back(std::move(address));
            rows.push_back({Kind::Open, std::wstring{Tr(T::NetworkCode)},
                std::wstring{Tr(!network.running ? T::NetworkStopped : network.paired ? T::NetworkPaired :
                    network.code_expired ? T::NetworkCodeExpired : T::NetworkCodeHint)},
                network.running && !network.code_expired ? std::wstring{winrt::to_hstring(network.code)} : std::wstring{L"—"}});
            rows.push_back({Kind::Open, std::wstring{Tr(T::FtpTitle)},
                            std::wstring{Tr(T::FtpSummary)}, std::wstring{Tr(T::NetworkStart)}});
            for (const auto& source : status.sources)
                rows.push_back({Kind::Open, I18n::Format(T::RemoveFolder, {source.name})});
        }
        return rows;
    }
    static constexpr float SettingsListX = LibraryLayout::Margin,
                           SettingsListWidth = Canvas::Unit360;
    static constexpr float SettingsListTop = Canvas::Unit132, SettingsListStride = Canvas::Unit66,
                           SettingsListHeight = Canvas::Unit58;
    static constexpr float SettingsPaneX = SettingsListX + SettingsListWidth + Canvas::Unit40;
    static constexpr float SettingsPaneWidth =
        Canvas::Unit1280 - LibraryLayout::Margin - SettingsPaneX;
    static constexpr float SettingsRowsTop = Canvas::Unit226, SettingsRowStride = Canvas::Unit58,
                           SettingsRowHeight = Canvas::Unit52;
    static constexpr std::size_t SettingsVisibleRows = 7;
    void DrawFtpConnection(const ConfigurationPanel& configuration) {
        Type(Tr(T::FtpTitle), TypeRole::HeadlineLarge, 80, 100, 1120, Palette::Text);
        Type(configuration.ftp_address, TypeRole::HeadlineMedium, 80, 176, 1120, Palette::Accent);
        Type(Tr(T::FtpCredentials), TypeRole::Body, 80, 240, 1120, Palette::TextSecondary);
        Type(configuration.ftp_password, TypeRole::HeadlineLarge, 80, 285, 1120, Palette::Text);
        Type(Tr(T::FtpFolders), TypeRole::Body, 80, 365, 1120, Palette::TextSecondary, 1,
             TextAlign::Leading, 3);
        Type(Tr(T::FtpHint), TypeRole::Body, 80, 475, 1120, Palette::TextHint, 1,
             TextAlign::Leading, 3);
        for (size_t i = 0; i < 2; ++i) {
            const float x = 80 + static_cast<float>(i) * 570;
            const bool focused = configuration.network_action == i;
            Round(x, 594, 540, 60, 16, focused ? Palette::Accent : Palette::Surface);
            Type(Tr(i == 0 ? T::Back : T::NetworkStop), TypeRole::LabelLarge, x + 20, 614, 500,
                 focused ? Palette::OnAccent : Palette::Text);
            AddHit(x, 594, 540, 60, LibraryHit::NetworkAction, i);
        }
    }
    void DrawNetworkConnection(const ConfigurationPanel& configuration) {
        const auto& network = configuration.network;
        Type(L"J A D Y X", TypeRole::HeadlineSmall, Canvas::Unit80, Canvas::Unit44, Canvas::Unit400,
             Palette::Text);
        if (network.paired)
            Type(Tr(T::NetworkPaired), TypeRole::LabelLarge, Canvas::Unit720, Canvas::Unit52,
                 Canvas::Unit480, Palette::TextHint, Canvas::Unit1, TextAlign::Trailing);
        Type(Tr(T::NetworkConnection), TypeRole::HeadlineLarge, Canvas::Unit80, Canvas::Unit100,
             Canvas::Unit1120, Palette::Text, Canvas::Unit1, TextAlign::Center);
        Type(winrt::to_hstring(network.url).c_str(), TypeRole::HeadlineMedium, Canvas::Unit80,
             Canvas::Unit176, Canvas::Unit1120, Palette::Text, Canvas::Unit1, TextAlign::Center);
        Type(Tr(T::NetworkOpenHint), TypeRole::Body, Canvas::Unit80, Canvas::Unit222,
             Canvas::Unit1120, Palette::TextSecondary, Canvas::Unit1, TextAlign::Center);
        Card(Canvas::Unit80, Canvas::Unit274, Canvas::Unit1120, Canvas::Unit210, Canvas::Unit20);
        Type(Tr(T::NetworkCodeLabel), TypeRole::Caps, Canvas::Unit100, Canvas::Unit296,
             Canvas::Unit1080, Palette::TextSecondary, Canvas::Unit1, TextAlign::Center);
        if (network.running && !network.code_expired && network.code.size() == 8) {
            const auto digits = winrt::to_hstring(network.code.substr(0, 4) + " " + network.code.substr(4));
            D2D1_MATRIX_3X2_F transform;
            target->GetTransform(&transform);
            // Hero is 56px at the reference resolution: 112px here, independent of settings rows.
            target->SetTransform(D2D1::Matrix3x2F::Scale(2, 2, D2D1::Point2F(640, 334)) * transform);
            Type(digits.c_str(), TypeRole::Hero, Canvas::Unit360, Canvas::Unit334, Canvas::Unit560,
                 Palette::Text, Canvas::Unit1, TextAlign::Center);
            target->SetTransform(transform);
        } else {
            Type(Tr(network.paired ? T::NetworkPaired : T::NetworkCodeExpired),
                 TypeRole::HeadlineMedium, Canvas::Unit110, Canvas::Unit366, Canvas::Unit1060,
                 Palette::Text, Canvas::Unit1, TextAlign::Center);
        }
        std::wstring progress;
        const auto& transfer=network.transfer;
        const auto& system=network.system;
        if (system.busy) {
            const auto& file=system.file;
            progress=system.phase=="receiving"&&file.busy ? I18n::Format(T::NetworkUploading,
                {winrt::to_hstring(file.name).c_str(),std::to_wstring(file.received >> 20),std::to_wstring(file.length >> 20)}) :
                system.phase=="review" ? std::wstring{Tr(T::SystemReviewReady)} : std::wstring{Tr(T::NetworkValidating)};
            progress+=L" · "+std::to_wstring(system.completed)+L" / "+std::to_wstring(system.count);
            const float fraction =
                file.length ? static_cast<float>(static_cast<double>(file.received) / file.length)
                            : Canvas::Unit0;
            Round(Canvas::Unit100, Canvas::Unit560, Canvas::Unit1080, Canvas::Unit4, Canvas::Unit2,
                  Palette::Graphite);
            Round(Canvas::Unit100, Canvas::Unit560,
                  Canvas::Unit1080 * std::clamp(fraction, Canvas::Unit0_0F, Canvas::Unit1_0F),
                  Canvas::Unit4, Canvas::Unit2, Palette::Accent);
        } else if (transfer.busy) {
            progress=transfer.phase == "receiving" ? I18n::Format(T::NetworkUploading,
                {winrt::to_hstring(transfer.name).c_str(),std::to_wstring(transfer.received >> 20),std::to_wstring(transfer.length >> 20)}) :
                std::wstring{Tr(T::NetworkValidating)};
            const float fraction =
                transfer.length
                    ? static_cast<float>(static_cast<double>(transfer.received) / transfer.length)
                    : Canvas::Unit0;
            Round(Canvas::Unit100, Canvas::Unit560, Canvas::Unit1080, Canvas::Unit4, Canvas::Unit2,
                  Palette::Graphite);
            Round(Canvas::Unit100, Canvas::Unit560,
                  Canvas::Unit1080 * std::clamp(fraction, Canvas::Unit0_0F, Canvas::Unit1_0F),
                  Canvas::Unit4, Canvas::Unit2, Palette::Accent);
        }
        Type(!progress.empty()              ? std::wstring_view{progress}
             : configuration.notice.empty() ? Tr(T::NetworkCodeHint)
                                            : std::wstring_view{configuration.notice},
             TypeRole::Body, Canvas::Unit100, Canvas::Unit510, Canvas::Unit1080,
             Palette::TextSecondary, Canvas::Unit1, TextAlign::Center, 2);
        const std::array<T, 3> labels{T::Back, T::NetworkCode, T::NetworkStop};
        for (size_t i = 0; i < labels.size(); ++i) {
            const float x = Canvas::Unit80 + static_cast<float>(i) * Canvas::Unit380;
            const bool focused = configuration.network_action == i;
            if (focused)
                FocusGlow(x, Canvas::Unit594, Canvas::Unit360, Canvas::Unit60, Canvas::Unit14,
                          Canvas::Unit1);
            Round(x, Canvas::Unit594, Canvas::Unit360, Canvas::Unit60, Canvas::Unit14,
                  focused ? Palette::Accent : Palette::Surface);
            Type(Tr(labels[i]), TypeRole::LabelLarge, x + Canvas::Unit14, Canvas::Unit614,
                 Canvas::Unit332, focused ? Palette::OnAccent : Palette::Text, Canvas::Unit1,
                 TextAlign::Center);
            AddHit(x, Canvas::Unit594, Canvas::Unit360, Canvas::Unit60, LibraryHit::NetworkAction,
                   i);
        }
        ControlPrompt(Navigation::Play, Tr(T::Select), Canvas::Unit80, Canvas::Unit676);
        ControlPrompt(Navigation::Back, Tr(T::Back), Canvas::Unit400, Canvas::Unit676);
    }
    /// The guided app update: where to point a browser and its code at the left, the three steps
    /// at the right over the warning that keeps the user's data. Laid out like the portal screen.
    void DrawUpdateGuide(const ConfigurationPanel& configuration, const Updates::Snapshot& update) {
        const auto& network = configuration.network;
        const bool code = network.running && !network.code_expired && network.code.size() == 8;
        Type(L"J A D Y X", TypeRole::HeadlineSmall, Canvas::Unit80, Canvas::Unit44, Canvas::Unit400,
             Palette::Text);
        const std::wstring installed{winrt::to_hstring(Updates::FormatVersion(update.installed))};
        const std::wstring incoming = update.release ?
            std::wstring{winrt::to_hstring(Updates::FormatVersion(update.release->version))} : installed;
        if (network.paired) {
            const float width = MeasureType(Tr(T::NetworkPaired), TypeRole::LabelLarge);
            Round(Canvas::Unit1200 - width - Canvas::Unit18, Canvas::Unit58, Canvas::Unit8,
                  Canvas::Unit8, Canvas::Unit4, Palette::Accent);
            Type(Tr(T::NetworkPaired), TypeRole::LabelLarge, Canvas::Unit720, Canvas::Unit52,
                 Canvas::Unit480, Palette::TextHint, Canvas::Unit1, TextAlign::Trailing);
        } else {
            Type(installed + L"  \u2192  " + incoming, TypeRole::LabelLarge, Canvas::Unit720,
                 Canvas::Unit52, Canvas::Unit480, Palette::TextHint, Canvas::Unit1,
                 TextAlign::Trailing);
        }
        Type(I18n::Format(T::UpdateGuideTitle, {incoming}), TypeRole::HeadlineLarge, Canvas::Unit80,
             Canvas::Unit96, Canvas::Unit1120, Palette::Text);
        Type(Tr(T::UpdateGuideIntro), TypeRole::BodyLarge, Canvas::Unit80, Canvas::Unit150,
             Canvas::Unit1120, Palette::TextSecondary);
        // The address and the code, drawn as large as the portal screen draws them.
        constexpr float card_x = Canvas::Unit80, card_y = Canvas::Unit200, card_w = Canvas::Unit560,
                        card_h = Canvas::Unit330, inner = card_x + Canvas::Unit28,
                        inner_w = card_w - Canvas::Unit56;
        Card(card_x, card_y, card_w, card_h, Canvas::Unit20);
        Type(Upper(Tr(T::UpdateGuideAddress)), TypeRole::Caps, inner, card_y + Canvas::Unit24,
             inner_w, Palette::TextSecondary);
        const std::wstring url = network.running ? std::wstring{winrt::to_hstring(network.url)} : std::wstring{L"\u2014"};
        Type(url, TypeRole::HeadlineMedium, inner, card_y + Canvas::Unit50, inner_w, Palette::Text);
        Round(inner, card_y + Canvas::Unit106, inner_w, Canvas::Unit1, Canvas::Unit0_5F,
              Palette::Graphite);
        Type(Upper(Tr(T::NetworkCodeLabel)), TypeRole::Caps, inner, card_y + Canvas::Unit128,
             inner_w, Palette::TextSecondary);
        if (code) {
            const auto digits = winrt::to_hstring(network.code.substr(0, 4) + " " + network.code.substr(4));
            D2D1_MATRIX_3X2_F transform;
            target->GetTransform(&transform);
            // Hero is 56px at the reference resolution: 84px here, centred in the card.
            const float center = card_x + card_w / Canvas::Unit2, top = card_y + Canvas::Unit156;
            target->SetTransform(D2D1::Matrix3x2F::Scale(1.5f, 1.5f, D2D1::Point2F(center, top)) * transform);
            Type(digits.c_str(), TypeRole::Hero, inner, top, inner_w, Palette::Text, Canvas::Unit1,
                 TextAlign::Center);
            target->SetTransform(transform);
        } else {
            Type(Tr(network.paired    ? T::NetworkPaired
                    : network.running ? T::NetworkCodeExpired
                                      : T::NetworkStopped),
                 TypeRole::HeadlineMedium, inner, card_y + Canvas::Unit180, inner_w, Palette::Text,
                 Canvas::Unit1, TextAlign::Center);
        }
        const bool problem = !configuration.notice.empty();
        Type(problem ? std::wstring_view{configuration.notice} : Tr(T::NetworkCodeHint),
             TypeRole::Body, inner, card_y + Canvas::Unit270, inner_w,
             problem ? Palette::Warning : Palette::TextSecondary, Canvas::Unit1, TextAlign::Center,
             2);
        // The steps: the done and current ones lit, the rest waiting.
        constexpr float steps_x = Canvas::Unit680, steps_w = Canvas::Unit520;
        const std::array<std::pair<T, T>, 3> steps{{{T::UpdateGuideConnect, T::UpdateGuideConnectBody},
            {T::UpdateGuideDownload, T::UpdateGuideDownloadBody}, {T::UpdateGuidePortal, T::UpdateGuidePortalBody}}};
        const std::size_t current = network.paired ? 1 : 0;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const float y = card_y + Canvas::Unit8 + static_cast<float>(i) * Canvas::Unit84;
            const bool lit = i <= current;
            if (i + 1 < steps.size())
                Round(steps_x + Canvas::Unit17, y + Canvas::Unit44, Canvas::Unit2, Canvas::Unit36,
                      Canvas::Unit1, i < current ? Palette::Accent : Palette::Graphite);
            if (i == current)
                FocusGlow(steps_x, y, Canvas::Unit36, Canvas::Unit36, Canvas::Unit18,
                          Canvas::Unit1);
            Round(steps_x, y, Canvas::Unit36, Canvas::Unit36, Canvas::Unit18,
                  lit ? Palette::Accent : Palette::Surface);
            Type(std::to_wstring(i + 1), TypeRole::LabelLarge, steps_x, y + Canvas::Unit8,
                 Canvas::Unit36, lit ? Palette::OnAccent : Palette::TextSecondary, Canvas::Unit1,
                 TextAlign::Center);
            Type(Tr(steps[i].first), TypeRole::LabelLarge, steps_x + Canvas::Unit56,
                 y + Canvas::Unit2, steps_w - Canvas::Unit56,
                 lit ? Palette::Text : Palette::TextHint);
            Type(Tr(steps[i].second), TypeRole::Body, steps_x + Canvas::Unit56, y + Canvas::Unit28,
                 steps_w - Canvas::Unit56, Palette::TextSecondary, Canvas::Unit1,
                 TextAlign::Leading, 2);
        }
        Round(steps_x, card_y + card_h - Canvas::Unit66, steps_w, Canvas::Unit66, Canvas::Unit14,
              Palette::Warning, Canvas::Unit0_12F);
        Type(Tr(T::UpdateGuideWarning), TypeRole::Body, steps_x + Canvas::Unit20,
             card_y + card_h - Canvas::Unit56, steps_w - Canvas::Unit40, Palette::Warning,
             Canvas::Unit1, TextAlign::Leading, 2);
        const std::array<T, 3> labels{T::UpdaterBack, T::NetworkCode, T::NetworkStop};
        for (size_t i = 0; i < labels.size(); ++i) {
            const float x = Canvas::Unit80 + static_cast<float>(i) * Canvas::Unit380;
            const bool focused = configuration.network_action == i;
            if (focused)
                FocusGlow(x, Canvas::Unit594, Canvas::Unit360, Canvas::Unit60, Canvas::Unit14,
                          Canvas::Unit1);
            Round(x, Canvas::Unit594, Canvas::Unit360, Canvas::Unit60, Canvas::Unit14,
                  focused ? Palette::Accent : Palette::Surface);
            Type(Tr(labels[i]), TypeRole::LabelLarge, x + Canvas::Unit14, Canvas::Unit614,
                 Canvas::Unit332, focused ? Palette::OnAccent : Palette::Text, Canvas::Unit1,
                 TextAlign::Center);
            AddHit(x, Canvas::Unit594, Canvas::Unit360, Canvas::Unit60, LibraryHit::NetworkAction,
                   i);
        }
        ControlPrompt(Navigation::Play, Tr(T::Select), Canvas::Unit80, Canvas::Unit676);
        ControlPrompt(Navigation::Back, Tr(T::Back), Canvas::Unit400, Canvas::Unit676);
    }
    /// The Settings tab: the categories at the left with the emulator's data under them, and the
    /// rows of the focused category at the right, live while the list has the focus.
    void DrawSettings(const LibraryView& view) {
        using namespace LibraryLayout;
        const auto& configuration = view.configuration;
        const auto& status = configuration.status;
        const std::size_t category = SettingsCategory(view);
        const bool in_pane = view.system.open || configuration.files || view.controller_panel;
        // The page's kicker: what it is and which build is installed.
        const auto kicker = Upper(Tr(T::SettingsTitle));
        const float kicker_width = MeasureType(kicker, TypeRole::Caps);
        Type(kicker, TypeRole::Caps, Margin, Canvas::Unit92, kicker_width + Canvas::Unit4,
             Palette::TextSecondary);
        Type(L"·", TypeRole::Label, Margin + kicker_width + Canvas::Unit12, Canvas::Unit92,
             Canvas::Unit12, Palette::TextSecondary, Canvas::Unit0_6F);
        Type(L"Jadyx " + version_text, TypeRole::Label, Margin + kicker_width + Canvas::Unit28,
             Canvas::Unit92, Canvas::Unit300, Palette::TextHint);
        // The categories.
        static constexpr std::array<ConfigurationIcon, HomeRowCount> icons{
            ConfigurationIcon::Settings, ConfigurationIcon::Controller, ConfigurationIcon::Folder};
        static constexpr std::array<T, HomeRowCount> titles{T::System, T::ControllersAndKeyboard, T::FileManager};
        for (std::size_t i = 0; i < HomeRowCount; ++i) {
            const float x = SettingsListX, y = SettingsListTop + i * SettingsListStride;
            const float w = SettingsListWidth, h = SettingsListHeight;
            const bool chosen = i == category;
            if (chosen && !in_pane) {
                FocusGlow(x, y, w, h, Theme().settings_category_radius, Canvas::Unit1);
                Round(x, y, w, h, Theme().settings_category_radius, Palette::FocusSurface);
            } else if (chosen) {
                Round(x, y, w, h, Theme().settings_category_radius, Palette::RowActive,
                      Canvas::Unit0_9F);
            }
            std::wstring summary;
            if (i == HomeSystem) {
                summary = SystemRowValue(view.system, SystemPage::UserName) + L" · " +
                          SystemRowValue(view.system, SystemPage::InterfaceLanguage);
            } else if (i == HomeControllers) {
                const auto label = ControllerLabel(view.panel.devices, view.options.controller_id);
                summary = view.options.controller_id.empty() ? I18n::Format(T::AutomaticValue, {label}) : label;
            } else {
                summary = std::wstring{Tr(status.keys_ready ? T::KeysReady : T::KeysMissing)} + L" · " +
                          I18n::Format(T::FirmwareFiles, {std::to_wstring(status.firmware_files)});
            }
            const unsigned color = chosen ? Palette::Text : Palette::TextSecondary;
            Icon(icons[i], x + Canvas::Unit18, y + (h - Canvas::Unit22) / Canvas::Unit2, color,
                 Canvas::Unit22);
            // Focus only brightens the entry: its text keeps its size and place.
            Type(Tr(titles[i]), TypeRole::LabelLarge, x + Canvas::Unit56, y + Canvas::Unit10,
                 w - Canvas::Unit92, chosen ? Palette::Text : Palette::TextHint);
            Type(summary, TypeRole::Label, x + Canvas::Unit56, y + Canvas::Unit31,
                 w - Canvas::Unit92, chosen ? Palette::TextHint : Palette::TextSecondary,
                 chosen ? Canvas::Unit0_85F : Canvas::Unit0_7F);
            Chevron(x + w - Canvas::Unit22, y + h / Canvas::Unit2, color);
            AddHit(x, y, w, h, LibraryHit::Category, i);
        }
        // The emulator's data: what the files on the console add up to.
        {
            const float x = SettingsListX, w = SettingsListWidth;
            const float y = SettingsListTop +
                            static_cast<float>(HomeRowCount) * SettingsListStride + Canvas::Unit18;
            Round(x, y, w, Canvas::Unit176, Canvas::Unit18, Palette::Surface, Canvas::Unit0_82F);
            Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, w - Canvas::Unit1, Canvas::Unit175,
                    Canvas::Unit18, Palette::White, Canvas::Unit0_06F);
            Type(Tr(T::EmulatorData), TypeRole::Caps, x + Canvas::Unit20, y + Canvas::Unit20,
                 Canvas::Unit200, Palette::TextSecondary);
            const auto keys = Tr(status.keys_ready ? T::KeysReady : T::KeysMissing);
            const float chip = MeasureType(keys, TypeRole::Label) + Canvas::Unit24;
            Chip(keys, x + w - Canvas::Unit20 - chip, y + Canvas::Unit15,
                 status.keys_ready ? Palette::Text : Palette::Warning);
            const float tile_w = (w - Canvas::Unit40 - Canvas::Unit12) / Canvas::Unit2;
            const auto tile = [&](float tx, std::wstring_view label, std::size_t count) {
                Round(tx, y + Canvas::Unit54, tile_w, Canvas::Unit68, Canvas::Unit12,
                      Palette::Canvas, Canvas::Unit0_7F);
                Type(label, TypeRole::Label, tx + Canvas::Unit12, y + Canvas::Unit64,
                     tile_w - Canvas::Unit24, Palette::TextSecondary);
                Type(std::to_wstring(count), TypeRole::HeadlineSmall, tx + Canvas::Unit12,
                     y + Canvas::Unit84, tile_w - Canvas::Unit24, Palette::Text);
            };
            tile(x + 20, Tr(T::SettingsFirmwareFiles), status.firmware_files);
            tile(x + 20 + tile_w + 12, Tr(T::SettingsGameFolders), status.sources.size());
            Type(Tr(T::SettingsLibraryGames), TypeRole::Label, x + Canvas::Unit20,
                 y + Canvas::Unit140, w - Canvas::Unit120, Palette::TextSecondary);
            Type(view.loading ? std::wstring{L"…"} : std::to_wstring(view.scan.entries.size()),
                 TypeRole::Label, x + w - Canvas::Unit120, y + Canvas::Unit140, Canvas::Unit100,
                 Palette::Text, Canvas::Unit1, TextAlign::Trailing);
        }
        // The category's page.
        static constexpr std::array<T, HomeRowCount> pages{T::SystemTitle, T::ControllersAndKeyboard, T::FileManager};
        static constexpr std::array<T, HomeRowCount> about{T::SystemSubtitle, T::ControllerSubtitle, T::FilesSubtitle};
        const float px = SettingsPaneX, pw = SettingsPaneWidth;
        Type(Tr(pages[category]), TypeRole::HeadlineLarge, px, Canvas::Unit122, pw, Palette::Text);
        Type(Tr(about[category]), TypeRole::Body, px, Canvas::Unit176, pw, Palette::TextSecondary);
        const auto rows = SettingsRows(view, category);
        if (rows.empty()) return;
        const std::size_t selected = !in_pane ? SIZE_MAX :
            category == HomeSystem ? view.system.selected :
            category == HomeFiles ? configuration.selected : view.controller_row;
        const std::size_t visible = std::min(rows.size(), SettingsVisibleRows);
        const std::size_t first = selected != SIZE_MAX && selected >= visible ? selected - visible + 1 : 0;
        const float card_h =
            visible * SettingsRowStride - (SettingsRowStride - SettingsRowHeight) + Canvas::Unit24;
        if (Theme().settings_groups) {
            Round(px, SettingsRowsTop - Canvas::Unit12, pw, card_h, Canvas::Unit18,
                  Palette::Surface, Canvas::Unit0_82F);
            Outline(px + Canvas::Unit0_5F, SettingsRowsTop - Canvas::Unit11_5F, pw - Canvas::Unit1,
                    card_h - Canvas::Unit1, Canvas::Unit18, Palette::White, Canvas::Unit0_06F);
        }
        for (std::size_t i = first; i < first + visible; ++i) {
            const float y = SettingsRowsTop + (i - first) * SettingsRowStride;
            DrawSettingsRow(rows[i], px + Canvas::Unit12, y, pw - Canvas::Unit24, SettingsRowHeight,
                            i == selected, in_pane ? Canvas::Unit1_0F : Canvas::Unit0_75F);
            AddHit(px + Canvas::Unit12, y, pw - Canvas::Unit24, SettingsRowHeight,
                   LibraryHit::SettingRow, i);
        }
        ScrollIndicator(px + pw + Canvas::Unit8, SettingsRowsTop, card_h - Canvas::Unit24, first,
                        visible, rows.size());
    }
    void DrawSettingsRow(const SettingsRow& row, float x, float y, float w, float h, bool focused, float alpha) {
        using Kind = SettingsRow::Kind;
        if (focused) {
            FocusGlow(x, y, w, h, Theme().settings_row_radius, Canvas::Unit1);
            Round(x, y, w, h, Theme().settings_row_radius, Palette::FocusSurface);
        } else {
            Round(x, y, w, h, Theme().settings_row_radius, Palette::Canvas,
                  Theme().settings_idle_opacity);
        }
        const float middle = y + h / Canvas::Unit2;
        float right = x + w - Canvas::Unit16;
        // The control at the right, drawn first: the texts take the room it leaves.
        switch (row.kind) {
        case Kind::Toggle:
            right -= Canvas::Unit52;
            Switch(right, middle - Canvas::Unit14, row.on, focused);
            break;
        case Kind::Segments: {
            constexpr float pad = Canvas::Unit14, inset = Canvas::Unit3, height = Canvas::Unit32;
            float width = Canvas::Unit2 * inset;
            for (const auto& label : row.segments) width += MeasureType(label, TypeRole::LabelLarge) + 2 * pad;
            right -= width;
            Round(right, middle - height / Canvas::Unit2, width, height, Canvas::Unit9,
                  Palette::Graphite, Canvas::Unit0_9F);
            float cursor = right + inset;
            for (std::size_t i = 0; i < row.segments.size(); ++i) {
                const float segment =
                    MeasureType(row.segments[i], TypeRole::LabelLarge) + Canvas::Unit2 * pad;
                const bool on = i == row.on_segment;
                if (on)
                    Round(cursor, middle - height / Canvas::Unit2 + inset, segment,
                          height - Canvas::Unit2 * inset, Canvas::Unit7, Palette::Accent);
                Type(row.segments[i], TypeRole::LabelLarge, cursor, middle - Canvas::Unit10,
                     segment, on ? Palette::OnAccent : Palette::TextSecondary, alpha,
                     TextAlign::Center);
                cursor += segment;
            }
            break;
        }
        case Kind::Choice: {
            // A capsule with the value; the arrows say left and right change it.
            const float text =
                std::min(MeasureType(row.value, TypeRole::LabelLarge) + Canvas::Unit4,
                         w * Canvas::Unit0_45F);
            const float width = text + Canvas::Unit2 * Canvas::Unit30;
            right -= width;
            Round(right, middle - Canvas::Unit16, width, Canvas::Unit32, Canvas::Unit9,
                  Palette::Graphite, Canvas::Unit0_9F);
            Type(row.value, TypeRole::LabelLarge, right + Canvas::Unit30, middle - Canvas::Unit10,
                 text, Palette::Text, alpha);
            const unsigned arrows = focused ? Palette::Text : Palette::TextSecondary;
            Chevron(right + Canvas::Unit16, middle, arrows, true);
            Chevron(right + width - Canvas::Unit14, middle, arrows);
            break;
        }
        case Kind::Open:
        case Kind::Value: {
            if (row.kind == Kind::Open) {
                Chevron(right - Canvas::Unit4, middle,
                        focused ? Palette::Text : Palette::TextSecondary);
                right -= Canvas::Unit22;
            }
            if (!row.value.empty()) {
                const float text =
                    std::min(MeasureType(row.value, TypeRole::LabelLarge) + Canvas::Unit4,
                             w * Canvas::Unit0_5F);
                right -= text;
                Type(row.value, TypeRole::LabelLarge, right, middle - Canvas::Unit10, text,
                     row.warning ? Palette::Warning
                     : focused   ? Palette::Text
                                 : Palette::TextHint,
                     alpha);
            }
            break;
        }
        }
        const float text_w = right - (x + Canvas::Unit18) - Canvas::Unit20;
        if (row.hint.empty()) {
            Type(row.title, TypeRole::SettingsTitle, x + Canvas::Unit18, middle - Canvas::Unit10,
                 text_w, Palette::Text, alpha);
        } else {
            Type(row.title, TypeRole::SettingsTitle, x + Canvas::Unit18, y + Canvas::Unit7, text_w,
                 Palette::Text, alpha);
            Type(row.hint, TypeRole::SettingsDetail, x + Canvas::Unit18, y + Canvas::Unit28, text_w,
                 focused ? Palette::TextHint : Palette::TextSecondary, alpha);
        }
    }
    void DrawNameEditor(const SystemPage& page) {
        using L = SystemPageLayout;
        using K = TextEntryPanelLayout;
        brush->SetColor(Ink(Palette::Black, Canvas::Unit0_86F));
        target->FillRectangle(
            D2D1::RectF(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720),
            brush.Get());
        Round(L::EditorX, L::EditorY, K::Width, K::Height, Canvas::Unit22, Palette::Surface);
        DrawTextEntryPanel(*this, {*page.name_editor, Tr(T::UserName), Tr(T::NameEditorHint), page.editor_notice},
                           L::EditorX, L::EditorY);
        const float left = L::EditorX + K::Left;
        const float footer = L::EditorY + K::Footer;
        if (prompt_family == PromptFamily::Keyboard) {
            Text(Tr(T::NameEditorKeyboardHelp), left, footer + Canvas::Unit6, Canvas::Unit15,
                 Palette::TextSecondary, K::BoxWidth, Canvas::Unit24);
        } else {
            ControlPrompt(Navigation::Play, Tr(T::TypeKey), left, footer);
            ControlPrompt(Navigation::AddFolder, Tr(T::DeleteKey), left + Canvas::Unit190, footer);
            ControlPrompt(Navigation::Space, Tr(T::SpaceKey), left + Canvas::Unit380, footer);
            ControlPrompt(Navigation::Options, Tr(T::DoneKey), left + Canvas::Unit570, footer);
            ControlPrompt(Navigation::Back, Tr(T::Cancel), left + Canvas::Unit760, footer);
        }
    }
    struct KeyFace {
        std::wstring_view letters;
        unsigned color;
    };
    /// The key or button that does `action` for the controller in use, and its letter's color.
    KeyFace PromptKey(Navigation action) const {
        if (prompt_family == PromptFamily::Keyboard) {
            switch (action) {
            case Navigation::Play: return {L"Enter", Palette::Text};
            case Navigation::Settings: return {L"F1", Palette::Text};
            case Navigation::Options: return {L"F2", Palette::Text};
            case Navigation::Back: return {L"Esc", Palette::Text};
            case Navigation::AddFolder: return {L"O", Palette::Text};
            case Navigation::Space: return {L"Y", Palette::Text};
            case Navigation::Refresh:
            case Navigation::Rescan: return {L"R", Palette::Text};
            case Navigation::Pin: return {L"F", Palette::Text};
            default: return {L"Q", Palette::Text};
            }
        }
        // Xbox letters keep their colors; Nintendo's are monochrome, and its A/B never swap.
        const bool xbox = prompt_family == PromptFamily::Xbox;
        const auto face = [&](std::wstring_view letter, unsigned color) {
            return KeyFace{letter, xbox ? color : ButtonColor::Neutral};
        };
        const bool swap = swap_prompt_faces;
        switch (action) {
        case Navigation::Play:
            return xbox && swap ? face(L"B", ButtonColor::B) : face(L"A", ButtonColor::A);
        case Navigation::Back:
            return xbox && swap ? face(L"A", ButtonColor::A) : face(L"B", ButtonColor::B);
        case Navigation::Settings: return {xbox ? L"View" : L"−", ButtonColor::Neutral};
        case Navigation::Options: return {xbox ? L"Menu" : L"+", ButtonColor::Neutral};
        case Navigation::AddFolder:
        case Navigation::Rescan: return swap ? face(L"Y", ButtonColor::Y) : face(L"X", ButtonColor::X);
        case Navigation::Space:
        case Navigation::Pin:
        case Navigation::Refresh: return swap ? face(L"X", ButtonColor::X) : face(L"Y", ButtonColor::Y);
        default: return {xbox ? L"LB" : L"L", ButtonColor::Neutral};
        }
    }
    Content::Keys ContentKeys() const {
        const auto key = [&](Navigation action) {
            const auto face = PromptKey(action);
            return Content::Key{face.letters, face.color};
        };
        return {key(Navigation::Play), key(Navigation::Back), key(Navigation::AddFolder), key(Navigation::Refresh)};
    }
    /// A prompt's label after its badges; returns the label's width.
    float PromptLabel(std::wstring_view label, float x, float y) {
        const float width = MeasureType(label, TypeRole::ControlLabel);
        Type(label, TypeRole::ControlLabel, x, y + Canvas::Unit6, width + Canvas::Unit4,
             Palette::TextSecondary);
        return width;
    }
    /// The badge of the button that does `action`, and its label, on a 32 px line; returns the
    /// width it took with the gap to the next prompt.
    float ControlPrompt(Navigation action, std::wstring_view label, float x, float y) {
        if (action == Navigation::Explore) {
            // The d-pad (the arrow keys on a keyboard): a cross in the badge.
            Round(x, y + Canvas::Unit5, Canvas::Unit22, Canvas::Unit22, Canvas::Unit11,
                  Palette::Graphite);
            Outline(x + Canvas::Unit0_5F, y + Canvas::Unit5_5F, Canvas::Unit21, Canvas::Unit21,
                    Canvas::Unit10_5F, Palette::White, Canvas::Unit0_15F);
            Round(x + Canvas::Unit9, y + Canvas::Unit10, Canvas::Unit4, Canvas::Unit12,
                  Canvas::Unit1, Palette::Text);
            Round(x + Canvas::Unit5, y + Canvas::Unit14, Canvas::Unit12, Canvas::Unit4,
                  Canvas::Unit1, Palette::Text);
            return Canvas::Unit30 + PromptLabel(label, x + Canvas::Unit30, y) + Canvas::Unit28;
        }
        if (action == Navigation::Tabs) {
            const bool keyboard = prompt_family == PromptFamily::Keyboard;
            const bool nintendo = prompt_family == PromptFamily::Nintendo;
            const std::wstring_view previous = keyboard ? L"Q" : nintendo ? L"L" : L"LB";
            const std::wstring_view next = keyboard ? L"E" : nintendo ? L"R" : L"RB";
            const float first = BadgeWidth(previous), second = BadgeWidth(next);
            Badge(previous, x + first / Canvas::Unit2, y + Canvas::Unit16, Palette::Text);
            Badge(next, x + first + Canvas::Unit6 + second / Canvas::Unit2, y + Canvas::Unit16,
                  Palette::Text);
            const float badges = first + Canvas::Unit6 + second + Canvas::Unit8;
            return badges + PromptLabel(label, x + badges, y) + Canvas::Unit28;
        }
        const auto key = PromptKey(action);
        return Prompt(key.letters, key.color, label, x, y);
    }
    /// The width ControlPrompt takes for a one-badge prompt, to lay it out before drawing it.
    float PromptWidth(Navigation action, std::wstring_view label) {
        return BadgeWidth(PromptKey(action).letters) + Canvas::Unit8 +
               MeasureType(label, TypeRole::ControlLabel) + Canvas::Unit28;
    }
    /// Eases `tween` toward the row `first`; a jump of more than a page (a new tab) lands at once.
    static float FollowRow(Tween& tween, std::size_t first, float page, Tween::Clock::time_point now) {
        const float target = static_cast<float>(first);
        if (std::abs(target - tween.Value(now)) > page) tween.Snap(target);
        else tween.Set(target, now);
        return tween.Value(now);
    }
    /// The game's options, and the option page open over them.
    void DrawGameInformation(const GameOptionsView& view) {
        const auto& game = *view.game;
        static const Compatibility::Report untested;
        const auto& report = view.compatibility
                                 ? Compatibility::Lookup(*view.compatibility, game.program_id)
                                 : untested;
        const auto data = BuildGameInformation(
            game, view.information ? *view.information : GameInformationFacts{}, report);
        const auto wide = [](const std::string& text) {
            return text.empty() ? std::wstring{Tr(T::GameInfoUnknown)}
                                : std::wstring{winrt::to_hstring(text)};
        };
        Cover(game, 56, 218, 300);
        Type(game.name, TypeRole::HeadlineMedium, 392, 214, 832, Palette::Text, 1,
             TextAlign::Leading, 2);
        const auto line = [&](std::wstring_view text, float top) {
            Type(text, TypeRole::Body, 392, top, 832, Palette::TextHint);
        };
        line(I18n::Format(T::GameInfoProgramId, {data.program_id}), 292);
        line(I18n::Format(T::GameInfoVersion, {data.version}), 318);
        line(I18n::Format(T::GameInfoFormat, {data.format, data.size}), 344);
        line(I18n::Format(T::GameInfoStorage, {data.storage}), 370);
        Type(data.path, TypeRole::Body, 392, 396, 832, Palette::TextHint, 1, TextAlign::Leading, 2);
        line(I18n::Format(T::GameInfoContent, {data.updates, data.dlc}), 448);
        DrawCompatibility(data.reported ? data.report.status : Compatibility::Status::Untested, 406,
                          488, 1);
        Type(Tr(data.compatibility), TypeRole::BodyLarge, 430, 478, 794, Palette::Text);
        if (data.reported) {
            Type(I18n::Format(T::GameInfoReport,
                              {wide(data.report.console), wide(data.report.fps_note)}),
                 TypeRole::Body, 392, 508, 832, Palette::TextHint, 1, TextAlign::Leading, 2);
            line(I18n::Format(T::GameInfoReportBuild, {wide(data.report.jadyx_version)}), 558);
            line(I18n::Format(T::GameInfoDate, {wide(data.report.date)}), 584);
        }
        if (!data.last_played.empty())
            line(I18n::Format(T::LastPlayed, {data.last_played}), 610);
        if (!data.play_time.empty())
            line(I18n::Format(T::GameInfoPlayTime, {data.play_time}), 636);
        if (CanRemoveGame(game)) {
            // Opening Remove never confirms it. Its existing pane explains the consequences
            // and still requires the same two presses before submitting a storage job.
            Round(56, 550, 300, 52, 26, Palette::RowActive);
            Prompt(PromptKey(Navigation::Play).letters, Palette::Text, Tr(T::GameRemove), 76, 560);
            const auto sections = GameOptions();
            const auto remove = std::find(sections.begin(), sections.end(), GameOption::Remove);
            AddHit(56, 550, 300, 52, LibraryHit::GameOptionSelect,
                   static_cast<std::size_t>(remove - sections.begin()));
        }
        ControlPrompt(Navigation::Back, Tr(T::Back), 1100, GameOptionsLayout::Footer);
    }

    void DrawGameOptions(const GameOptionsView& view) {
        hits.clear();
        manager_targets = (view.page.page == GameOption::Cheats && view.cheats) ||
            (view.page.page == GameOption::Mods && view.mods && view.mods_page &&
            view.mods_page->screen == Mods::ModsScreen::List && !view.mods->Busy());
        if (view.content && !view.game) {
            Content::DrawContentPanel(*this, *view.content, Tr(T::ContentOverview), ContentKeys());
            return;
        }
        const LibraryEntry& game = *view.game;
        const bool modal = view.page.page == GameOption::Mods && view.mods_page &&
                           view.mods_page->screen != Mods::ModsScreen::List;
        const bool expandable = view.page.page == GameOption::Mods || view.page.page == GameOption::Cheats;
        const float body_bottom = view.page.page == GameOption::Mods && view.mods_page &&
            view.mods_page->tab == Mods::ModsTab::Explore ? GameOptionsLayout::GridBottom : GameOptionsLayout::ListBottom;
        const auto now = Tween::Clock::now();
        management_collapse.Set(expandable && view.page.focus == GameOptionsFocus::Content ? GameOptionsLayout::HeroCollapse : 0.0f, now);
        const float offset = expandable && !modal ? management_collapse.Value(now) : Canvas::Unit0;
        const bool keyboard = prompt_family == PromptFamily::Keyboard;
        const bool nintendo = prompt_family == PromptFamily::Nintendo;
        const std::wstring_view previous_tab = keyboard ? L"PgUp" : nintendo ? L"ZL" : L"LT";
        const std::wstring_view next_tab = keyboard ? L"PgDn" : nintendo ? L"ZR" : L"RT";
        constexpr float footer = GameOptionsLayout::Footer;
        // B, the way out, always ends the bar at the right margin.
        const auto back_prompt = [&](std::wstring_view label) {
            const float width = BadgeWidth(PromptKey(Navigation::Back).letters) + Canvas::Unit8 +
                                MeasureType(label, TypeRole::LabelLarge);
            ControlPrompt(Navigation::Back, label, Canvas::Unit1224 - width, footer);
        };
        D2D1_MATRIX_3X2_F base_transform;
        target->GetTransform(&base_transform);
        const auto begin_body = [&] {
            if (modal) return;
            target->PushAxisAlignedClip(D2D1::RectF(Canvas::Unit0,
                                                    GameOptionsLayout::BodyTop - offset,
                                                    Canvas::Unit1280, body_bottom),
                                        D2D1_ANTIALIAS_MODE_ALIASED);
            target->SetTransform(D2D1::Matrix3x2F::Translation(0, -offset) * base_transform);
            manager_offset = offset;
            manager_bottom = body_bottom;
            manager_clip = true;
        };
        const auto end_body = [&] {
            if (modal) return;
            target->SetTransform(base_transform);
            target->PopAxisAlignedClip();
            manager_offset = 0;
            manager_clip = false;
        };
        // The triggers move between the page's own tabs; the bumpers between the game's options.
        const auto internal_prompt = [&](std::wstring_view label, float left) {
            const float first = BadgeWidth(previous_tab);
            Badge(previous_tab, left + first / Canvas::Unit2, footer + Canvas::Unit16,
                  Palette::Text);
            return first + 6 +
                   Prompt(next_tab, Palette::Text, label, left + first + Canvas::Unit6, footer);
        };

        Round(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720, Canvas::Unit0,
              Palette::Canvas, Canvas::Unit0_86F);
        if (view.page.page == GameOption::Information) {
            DrawGameInformation(view);
        } else if (view.page.page == GameOption::Cheats && view.cheats) {
            using L = Cheats::CheatsLayout;
            constexpr float x = (Canvas::Unit1280 - L::Width) / Canvas::Unit2,
                            y = (Canvas::Unit720 - L::Height) / Canvas::Unit2;
            const Cheats::CheatsView manager{*view.cheats, view.cheats_page, game.name, view.notice, false,
                view.page.focus == GameOptionsFocus::Content, view.page.focus == GameOptionsFocus::ManagerTabs,
                previous_tab, next_tab, FollowRow(list_scroll, view.cheats_page.first, L::VisibleRows, now)};
            begin_body();
            Cheats::DrawCheatsPanel(*this, manager, x, y);
            end_body();
            Round(Canvas::Unit0, body_bottom, Canvas::Unit1280, Canvas::Unit720 - body_bottom,
                  Canvas::Unit0, Palette::Canvas);
            Round(Canvas::Unit40, GameOptionsLayout::FooterLine, Canvas::Unit1200, Canvas::Unit1,
                  Canvas::Unit0, Palette::Graphite);
            Cheats::DrawCheatsStatus(*this, manager, x, y);
            if (view.page.focus == GameOptionsFocus::Content) {
                float next = Canvas::Unit56;
                next += ControlPrompt(Navigation::Play, Tr(T::ToggleCheat), next, footer);
                next += ControlPrompt(Navigation::AddFolder, Tr(T::AllCheats), next, footer);
                if (Cheats::CheatTabCount(view.cheats->Catalog()) > 1)
                    next += internal_prompt(Tr(T::Category), next);
                ControlPrompt(Navigation::Refresh, Tr(T::Refresh), next, footer);
                back_prompt(Tr(T::Back));
            }
        } else if (view.page.page == GameOption::Mods && view.mods && view.mods_page) {
            using L = Mods::ModsLayout;
            using Button = Mods::ModsPrompt::Button;
            constexpr float x = (Canvas::Unit1280 - L::Width) / Canvas::Unit2,
                            y = (Canvas::Unit720 - L::Height) / Canvas::Unit2;
            const Mods::ModsView manager{*view.mods, *view.mods_page, game.name, view.notice,
                view.page.focus == GameOptionsFocus::Content, view.page.focus == GameOptionsFocus::ManagerTabs,
                previous_tab, next_tab, PromptKey(Navigation::Play).letters, PromptKey(Navigation::Play).color,
                FollowRow(list_scroll, view.mods_page->first, L::VisibleRows, now),
                FollowRow(grid_scroll, view.mods_page->card_first / L::Columns, 2, now), body_bottom + offset};
            begin_body();
            Mods::DrawModsPanel(*this, manager, x, y);
            end_body();
            if (!modal) {
                Round(Canvas::Unit0, body_bottom, Canvas::Unit1280, Canvas::Unit720 - body_bottom,
                      Canvas::Unit0, Palette::Canvas);
                Round(Canvas::Unit40, GameOptionsLayout::FooterLine, Canvas::Unit1200,
                      Canvas::Unit1, Canvas::Unit0, Palette::Graphite);
                Mods::DrawModsStatus(*this, manager, x, y);
            }
            float next = Canvas::Unit56;
            for (const auto& prompt : Mods::ModsPrompts(*view.mods, *view.mods_page)) {
                // On the tab bars, the bars' own prompts below speak instead.
                if (!modal && view.page.focus != GameOptionsFocus::Content) break;
                const auto label = Tr(prompt.label);
                if (prompt.button == Button::B) {
                    back_prompt(label);
                    continue;
                }
                const Navigation action = prompt.button == Button::A      ? Navigation::Play
                                          : prompt.button == Button::X    ? Navigation::AddFolder
                                          : prompt.button == Button::Y    ? Navigation::Refresh
                                          : prompt.button == Button::Menu ? Navigation::Options
                                                                          : Navigation::Tabs;
                next += action == Navigation::Tabs ? internal_prompt(label, next) :
                    ControlPrompt(action, label, next, footer);
            }
        } else if (view.content) {
            Content::DrawContentPanel(*this, *view.content, game.name, ContentKeys());
        } else {
            ManagementSurface(0, 0, 1280, 720, GameOptionsLayout::Footer);
            // The panes that act in place: what A does, explained, and A once more confirms it.
            const auto in_place = [&](GameOption option, std::wstring_view body,
                                      std::wstring_view action) {
                Card(Canvas::Unit56, Canvas::Unit214, Canvas::Unit1168, Canvas::Unit264,
                     Canvas::Unit24);
                Icon(GameOptionIcon(option), Canvas::Unit80, Canvas::Unit240, Palette::Text);
                Type(Tr(GameOptionTitle(option)), TypeRole::HeadlineMedium, Canvas::Unit132,
                     Canvas::Unit234, Canvas::Unit1056, Palette::Text);
                Type(body, TypeRole::BodyLarge, Canvas::Unit80, Canvas::Unit298, Canvas::Unit1120,
                     Palette::TextHint, Canvas::Unit1, TextAlign::Leading, 3);
                if (!action.empty())
                    Prompt(PromptKey(Navigation::Play).letters, Palette::Text, action,
                           Canvas::Unit80, Canvas::Unit404);
            };
            const auto option = GameOptions()[view.page.selected];
            if (option == GameOption::Pulse) {
                const auto pulse = game.pulse_saved.empty() ? std::wstring{Tr(T::PulseNone)} :
                    I18n::Format(game.pulse_current ? T::PulseSavedOn : T::PulseSavedOther, {game.pulse_saved});
                const auto discard = I18n::Format(T::PulseDiscardConfirm, {PromptKey(Navigation::Play).letters});
                in_place(option, pulse, game.pulse_saved.empty() ? std::wstring_view{} : std::wstring_view{discard});
            } else if (option == GameOption::Remove) {
                const bool external = IsStoragePath(game.launch_path);
                in_place(option, Tr(external ? T::GameRemoveExternal :
                                    view.other_copies ? T::GameRemoveCopyWarning : T::GameRemoveWarning),
                         external ? std::wstring_view{} : Tr(T::GameRemove));
            } else {
                Type(view.notice.empty() ? Tr(T::CheckingGame) : view.notice, TypeRole::BodyLarge,
                     Canvas::Unit56, Canvas::Unit230, Canvas::Unit1168, Palette::TextHint,
                     Canvas::Unit1, TextAlign::Leading, 3);
            }
        }
        if (view.mods_page && view.page.page == GameOption::Mods &&
            view.mods_page->screen != Mods::ModsScreen::List) return; // A modal covers the game content.
        // The global header stays fixed; the game's line folds up behind it.
        using G = GameOptionsLayout;
        Round(Canvas::Unit0, G::GlobalHeaderBottom, Canvas::Unit1280,
              G::BodyTop - G::GlobalHeaderBottom - offset, Canvas::Unit0, Palette::Canvas);
        if (offset < G::HeroCollapse) {
            target->PushAxisAlignedClip(D2D1::RectF(Canvas::Unit0, G::GlobalHeaderBottom,
                                                    Canvas::Unit1280, G::HeroClipBottom - offset),
                                        D2D1_ANTIALIAS_MODE_ALIASED);
            const float top = G::HeroTop - offset;
            Cover(game, 56, top, G::HeroHeight);
            const float title_left = Canvas::Unit56 + G::HeroHeight + Canvas::Unit16;
            const float title_width =
                std::min(MeasureType(game.name, TypeRole::HeadlineMedium), Canvas::Unit640_0F);
            Type(game.name, TypeRole::HeadlineMedium, title_left, top + Canvas::Unit6,
                 title_width + Canvas::Unit4, Palette::Text);
            // Who made it, its version and its build, on the title's line.
            std::wstring about = game.developer;
            const auto add = [&](std::wstring_view part) {
                if (part.empty()) return;
                if (!about.empty()) about += L"  \u00b7  ";
                about += part;
            };
            add(game.display_version);
            if (std::any_of(game.build_id.begin(), game.build_id.end(), [](auto byte) { return byte != 0; })) {
                std::wstring build = L"Build ";
                constexpr wchar_t hex[] = L"0123456789ABCDEF";
                for (auto byte : game.build_id) { build += hex[byte >> 4]; build += hex[byte & 15]; }
                add(build);
            }
            const float about_left = title_left + title_width + Canvas::Unit20;
            Type(about, TypeRole::Label, about_left, top + Canvas::Unit17,
                 Canvas::Unit1224 - about_left, Palette::TextHint);
            target->PopAxisAlignedClip();
        }
        const auto options = GameOptions();
        const float sections = G::SectionsTop - offset;
        Round(Canvas::Unit56, sections, Canvas::Unit1168, G::SectionsHeight,
              G::SectionsHeight / Canvas::Unit2, Palette::Surface);
        Badge(keyboard   ? L"Q"
              : nintendo ? L"L"
                         : L"LB",
              Canvas::Unit80, sections + G::SectionsHeight / Canvas::Unit2, Palette::Text);
        Badge(keyboard   ? L"E"
              : nintendo ? L"R"
                         : L"RB",
              Canvas::Unit1200, sections + G::SectionsHeight / Canvas::Unit2, Palette::Text);
        // The bar spans the bumpers' gap, shared by however many options there are.
        const float tab_width = Canvas::Unit1080_0F / static_cast<float>(options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            const float left = Canvas::Unit100 + static_cast<float>(i) * tab_width;
            const bool active = i == view.page.selected;
            if (active)
                Round(left, sections + Canvas::Unit4, tab_width - Canvas::Unit4, Canvas::Unit36,
                      Canvas::Unit18,
                      (view.page.focus == GameOptionsFocus::Sections) ? Palette::Accent
                                                                      : Palette::RowActive);
            Type(Tr(GameOptionTitle(options[i])), TypeRole::LabelLarge, left + Canvas::Unit6,
                 sections + Canvas::Unit12, tab_width - Canvas::Unit16,
                 active && (view.page.focus == GameOptionsFocus::Sections) ? Palette::OnAccent
                 : active                                                  ? Palette::Text
                                                                           : Palette::TextHint,
                 Canvas::Unit1, TextAlign::Center);
            AddHit(left, sections + Canvas::Unit4, tab_width - Canvas::Unit4, Canvas::Unit36,
                   LibraryHit::GameOptionSelect, i);
        }
        // A confirmation names the key that confirms on this controller ("{0}").
        if (!view.notice.empty() && view.page.page != GameOption::Mods && view.page.page != GameOption::Cheats)
            Type(I18n::Format(view.notice, {PromptKey(Navigation::Play).letters}), TypeRole::Label,
                 Canvas::Unit56, G::Status, Canvas::Unit1168, Palette::Warning);
        // The panes that act in place have no prompts of their own; on the manager's tabs (a
        // pointer put the focus there), the bar's prompts speak over the pane's.
        if (view.page.focus != GameOptionsFocus::Content || !view.page.page) {
            Round(Canvas::Unit0, G::FooterLine + Canvas::Unit1, Canvas::Unit1280,
                  Canvas::Unit720 - G::FooterLine - Canvas::Unit1, Canvas::Unit0, Palette::Canvas);
            float next = Canvas::Unit56;
            next += view.page.focus == GameOptionsFocus::ManagerTabs ? internal_prompt(Tr(T::Category), next) :
                ControlPrompt(Navigation::Tabs, Tr(T::Category), next, footer);
            if (view.page.focus == GameOptionsFocus::ManagerTabs)
                Prompt(L"\u2193", Palette::Text, Tr(T::Select), next, footer);
            back_prompt(Tr(T::Back));
        }

    }
    /// The entry's icon decoded to 256x256, or nullptr while it is not read (or not a picture).
    ID2D1Bitmap1* CoverBitmap(const LibraryEntry& entry) {
        // The launch path is unique; relative paths repeat across internal and USB sources.
        auto found = covers.find(entry.launch_path);
        if (found == covers.end()) {
            if (entry.icon.empty()) return nullptr;
            ComPtr<ID2D1Bitmap1> bitmap;
            try {
                ComPtr<IWICStream> stream;
                winrt::check_hresult(wic->CreateStream(&stream));
                winrt::check_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(entry.icon.data()),
                                                                 static_cast<DWORD>(entry.icon.size())));
                ComPtr<IWICBitmapDecoder> decoder;
                winrt::check_hresult(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                    WICDecodeMetadataCacheOnDemand, &decoder));
                ComPtr<IWICBitmapFrameDecode> frame;
                winrt::check_hresult(decoder->GetFrame(0, &frame));
                UINT w, h;
                winrt::check_hresult(frame->GetSize(&w, &h));
                if (w == 0 || h == 0 || w > 4096 || h > 4096)
                    winrt::throw_hresult(E_INVALIDARG);
                ComPtr<IWICBitmapScaler> scaler;
                winrt::check_hresult(wic->CreateBitmapScaler(&scaler));
                winrt::check_hresult(scaler->Initialize(frame.Get(), 256, 256, WICBitmapInterpolationModeFant));
                ComPtr<IWICFormatConverter> converter;
                winrt::check_hresult(wic->CreateFormatConverter(&converter));
                winrt::check_hresult(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
                winrt::check_hresult(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
            } catch (const winrt::hresult_error&) {}
            // The rail and four rows of the collection: 256x256 BGRA, 256 KiB each.
            if (covers.size() >= MaxCovers) covers.clear();
            found = covers.emplace(entry.launch_path, std::move(bitmap)).first;
        }
        return found->second.Get();
    }
    void Cover(const LibraryEntry& entry, float x, float y, float size,
               float alpha = Canvas::Unit1) {
        Round(x, y, size, size, Theme().cover_radius, Palette::Graphite, alpha);
        if (auto* bitmap = CoverBitmap(entry)) {
            // One unit mask for every size: the layer's transform places and scales it, so a
            // cover growing into focus allocates nothing.
            if (!cover_mask) {
                ComPtr<ID2D1Factory> factory;
                target->GetFactory(&factory);
                winrt::check_hresult(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(
                        D2D1::RectF(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1, Canvas::Unit1),
                        Theme().cover_mask_radius, Theme().cover_mask_radius),
                    &cover_mask));
            }
            D2D1_LAYER_PARAMETERS1 layer{};
            layer.contentBounds = D2D1::RectF(x, y, x + size, y + size);
            layer.geometricMask = cover_mask.Get();
            layer.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
            layer.maskTransform = D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Translation(x, y);
            layer.opacity = alpha;
            target->PushLayer(&layer, nullptr);
            target->DrawBitmap(bitmap, D2D1::RectF(x, y, x + size, y + size), 1, D2D1_INTERPOLATION_MODE_LINEAR);
            target->PopLayer();
        } else {
            Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, size - Canvas::Unit1,
                    size - Canvas::Unit1, Theme().cover_radius - Canvas::Unit0_5F, Palette::White,
                    Canvas::Unit0_08F * alpha);
            const auto extension = entry.relative_path.extension().wstring();
            if (extension.size() > 1)
                Type(Upper(std::wstring_view{extension}.substr(1)), TypeRole::Caps, x,
                     y + size / Canvas::Unit2 - Canvas::Unit8, size, Palette::TextSecondary, alpha,
                     TextAlign::Center);
        }
    }
    /// The focused game's cover behind the screen: blurred, a touch more saturated, cropped to
    /// 16:9. Shown only once its bitmap exists, so a game whose icon is still being read fades in
    /// when it arrives instead of leaving the previous one up.
    void UpdateBackdrop(const LibraryEntry* game, Tween::Clock::time_point now) {
        std::string shown;
        if (game && BackdropOf(*game)) shown = game->launch_path;
        if (shown == backdrop_now) return;
        backdrop_before = std::move(backdrop_now);
        backdrop_now = std::move(shown);
        backdrop_fade.Snap(0);
        backdrop_fade.Set(1, now);
    }
    ID2D1Bitmap1* FindBackdrop(const std::string& key) {
        for (auto& [path, bitmap] : backdrops)
            if (path == key) return bitmap.Get();
        return nullptr;
    }
    /// A recently used backdrop, or a new one when the cover is decoded (least recent dropped).
    ID2D1Bitmap1* BackdropOf(const LibraryEntry& entry) {
        const auto found = std::find_if(backdrops.begin(), backdrops.end(),
                                        [&](const auto& backdrop) { return backdrop.first == entry.launch_path; });
        if (found != backdrops.end()) {
            std::rotate(found, found + 1, backdrops.end());
            return backdrops.back().second.Get();
        }
        auto* cover = CoverBitmap(entry);
        if (!cover) return nullptr;
        auto bitmap = RenderBackdrop(cover);
        if (!bitmap) return nullptr;
        if (backdrops.size() >= MaxBackdrops) backdrops.erase(backdrops.begin());
        backdrops.emplace_back(entry.launch_path, std::move(bitmap));
        return backdrops.back().second.Get();
    }
    static D2D1_MATRIX_5X4_F Saturation(float amount) {
        // Rec. 709 luma stays; chroma scales by `amount`.
        constexpr float r = Canvas::Unit0_2126F, g = Canvas::Unit0_7152F, b = Canvas::Unit0_0722F;
        const float k = Canvas::Unit1 - amount;
        return D2D1::Matrix5x4F(r * k + amount, r * k, r * k, 0,
                                g * k, g * k + amount, g * k, 0,
                                b * k, b * k, b * k + amount, 0,
                                0, 0, 0, 1,
                                0, 0, 0, 0);
    }
    /// Renders once, small: shown 4x larger, the blur hides the scaling. Costs one 320x180 pass
    /// per game focused, then a single bitmap draw per frame.
    ComPtr<ID2D1Bitmap1> RenderBackdrop(ID2D1Bitmap1* cover) {
        if (!backdrop_color) {
            // Edges clamp, so the blur does not darken toward the borders.
            if (FAILED(target->CreateEffect(CLSID_D2D1Border, &backdrop_border)) ||
                FAILED(target->CreateEffect(CLSID_D2D1GaussianBlur, &backdrop_blur)) ||
                FAILED(target->CreateEffect(CLSID_D2D1ColorMatrix, &backdrop_color))) {
                backdrop_border.Reset();
                backdrop_blur.Reset();
                backdrop_color.Reset();
                return {};
            }
            backdrop_border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
            backdrop_border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);
            backdrop_blur->SetInputEffect(0, backdrop_border.Get());
            backdrop_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 12.0f);
            backdrop_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
            backdrop_color->SetInputEffect(0, backdrop_blur.Get());
            backdrop_color->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, Saturation(1.18f));
        }
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target->CreateBitmap(D2D1::SizeU(BackdropWidth, BackdropHeight), nullptr, 0, properties, &bitmap)))
            return {};
        ComPtr<ID2D1Image> screen;
        target->GetTarget(&screen);
        D2D1_MATRIX_3X2_F transform;
        target->GetTransform(&transform);
        backdrop_border->SetInput(0, cover);
        target->SetTarget(bitmap.Get());
        target->BeginDraw();
        target->Clear(Ink(Palette::Black, Canvas::Unit0));
        // The square cover fills the width; its middle band shows.
        const auto size = cover->GetSize();
        const float scale = BackdropWidth / size.width;
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(0, (BackdropHeight - size.height * scale) / 2));
        target->DrawImage(backdrop_color.Get());
        const HRESULT drawn = target->EndDraw();
        target->SetTarget(screen.Get());
        target->SetTransform(transform);
        backdrop_border->SetInput(0, nullptr);
        if (FAILED(drawn)) return {};
        return bitmap;
    }
    /// The backdrops (crossfading), then the scrims that keep text legible over any art.
    void DrawBackdrop(Tween::Clock::time_point now) {
        const float t = backdrop_fade.Value(now);
        // A slow drift: the art breathes and wanders, and a larger, fainter copy of it flows the
        // other way. The periods share no factor, so the motion never visibly repeats; each copy
        // is scaled enough that its edges stay off screen. Two quads of the cached bitmap a frame.
        constexpr float Tau = Canvas::Unit6_2831853F;
        const float s = std::chrono::duration<float>(now - ambient_epoch).count();
        const auto rect = [](float scale, float dx, float dy) {
            const float w = Canvas::Unit640 * scale, h = Canvas::Unit360 * scale;
            return D2D1::RectF(Canvas::Unit640 - w + dx, Canvas::Unit360 - h + dy,
                               Canvas::Unit640 + w + dx, Canvas::Unit360 + h + dy);
        };
        const auto art = rect(1.2f + 0.08f * std::sin(Tau * s / 17), 60 * std::sin(Tau * s / 19),
                              36 * std::sin(Tau * s / 13 + 1.3f));
        const auto glow = rect(1.7f + 0.15f * std::sin(Tau * s / 23 + 2), -100 * std::sin(Tau * s / 17 + 0.7f),
                               60 * std::sin(Tau * s / 11));
        // The flowing copy also swells and fades, as light passing over the art.
        const float shine = Canvas::Unit0_3F +
                            Canvas::Unit0_2F * std::sin(Tau * s / Canvas::Unit9 + Canvas::Unit0_4F);
        const auto layer = [&](const std::string& key, float opacity) {
            if (key.empty() || opacity <= 0.002f) return;
            if (auto* bitmap = FindBackdrop(key)) {
                target->DrawBitmap(bitmap, art, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
                target->DrawBitmap(bitmap, glow, opacity * shine, D2D1_INTERPOLATION_MODE_LINEAR);
            }
        };
        layer(backdrop_before, BackdropOpacity * (1 - t));
        layer(backdrop_now, BackdropOpacity * t);
        if (!scrims[0]) MakeScrims();
        for (const auto& scrim : scrims)
            if (scrim)
                target->FillRectangle(
                    D2D1::RectF(Canvas::Unit0, Canvas::Unit0, Canvas::Unit1280, Canvas::Unit720),
                    scrim.Get());
    }
    void MakeScrims() {
        const auto canvas = [&](float alpha) { return Ink(Palette::Canvas, alpha); };
        const auto stops = [&](std::initializer_list<D2D1_GRADIENT_STOP> list) {
            ComPtr<ID2D1GradientStopCollection> collection;
            winrt::check_hresult(target->CreateGradientStopCollection(list.begin(), static_cast<UINT32>(list.size()),
                                                                      &collection));
            return collection;
        };
        const auto linear = [&](D2D1_POINT_2F from, D2D1_POINT_2F to, std::initializer_list<D2D1_GRADIENT_STOP> list) {
            ComPtr<ID2D1LinearGradientBrush> brush;
            winrt::check_hresult(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                                                   stops(list).Get(), &brush));
            return ComPtr<ID2D1Brush>{brush};
        };
        // The hero's side, the header and the bottom with the rows and the prompts.
        scrims[0] = linear({0, 0}, {820, 0},
                           {{0, canvas(Canvas::Unit0_85F)},
                            {0.5f, canvas(Canvas::Unit0_55F)},
                            {1, canvas(Canvas::Unit0)}});
        scrims[1] =
            linear({0, 0}, {0, 160}, {{0, canvas(Canvas::Unit0_7F)}, {1, canvas(Canvas::Unit0)}});
        scrims[2] = linear({0, 380}, {0, 720},
                           {{0, canvas(Canvas::Unit0)},
                            {0.5f, canvas(Canvas::Unit0_7F)},
                            {1, canvas(Canvas::Unit0_92F)}});
        ComPtr<ID2D1RadialGradientBrush> vignette;
        winrt::check_hresult(target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties({640, 320}, {0, 0}, 860, 520),
            stops({{0.6f, canvas(Canvas::Unit0)}, {1, canvas(Canvas::Unit0_55F)}}).Get(),
            &vignette));
        scrims[3] = vignette;
    }
    /// `text` in capitals by the user's locale (unchanged when that would change its length).
    static std::wstring Upper(std::wstring_view text) {
        std::wstring upper(text);
        if (!text.empty() &&
            LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, text.data(),
                          static_cast<int>(text.size()), upper.data(), static_cast<int>(upper.size()), nullptr,
                          nullptr, 0) == 0)
            upper = text;
        return upper;
    }
    /// The mods store's pictures (UiPainter::Picture): decoded once, at most 640 px wide.
    bool Picture(const std::filesystem::path& file, float x, float y, float w, float h) override {
        if (file.empty() || w < 1 || h < 1) return false;
        auto found = pictures.find(file.native());
        if (found == pictures.end()) {
            if (pictures.size() >= MaxPictures) pictures.clear();
            found = pictures.emplace(file.native(), DecodePicture(file, 640)).first;
        }
        const auto& bitmap = found->second;
        if (!bitmap) return false;
        // Cropped around the middle to the rectangle's shape.
        const auto size = bitmap->GetSize();
        float crop_w = size.width, crop_h = size.height;
        if (crop_w / crop_h > w / h) crop_w = crop_h * w / h;
        else crop_h = crop_w * h / w;
        const auto source = D2D1::RectF(
            (size.width - crop_w) / Canvas::Unit2, (size.height - crop_h) / Canvas::Unit2,
            (size.width + crop_w) / Canvas::Unit2, (size.height + crop_h) / Canvas::Unit2);
        auto& mask = picture_masks[(static_cast<unsigned>(w) << 16) | static_cast<unsigned>(h)];
        if (!mask) {
            ComPtr<ID2D1Factory> factory;
            target->GetFactory(&factory);
            if (FAILED(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(Canvas::Unit0, Canvas::Unit0, w, h),
                                      Canvas::Unit12, Canvas::Unit12),
                    &mask)))
                return false;
        }
        D2D1_LAYER_PARAMETERS1 layer{};
        layer.contentBounds = D2D1::RectF(x, y, x + w, y + h);
        layer.geometricMask = mask.Get();
        layer.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        layer.maskTransform = D2D1::Matrix3x2F::Translation(x, y);
        layer.opacity = 1;
        target->PushLayer(&layer, nullptr);
        target->DrawBitmap(bitmap.Get(), D2D1::RectF(x, y, x + w, y + h), 1, D2D1_INTERPOLATION_MODE_LINEAR,
                           &source);
        target->PopLayer();
        return true;
    }
    ComPtr<ID2D1Bitmap1> DecodePicture(const std::filesystem::path& file, unsigned max_width) {
        ComPtr<ID2D1Bitmap1> bitmap;
        try {
            ComPtr<IWICBitmapDecoder> decoder;
            winrt::check_hresult(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, &frame));
            UINT w, h;
            winrt::check_hresult(frame->GetSize(&w, &h));
            if (w == 0 || h == 0 || w > 8192 || h > 8192) winrt::throw_hresult(E_INVALIDARG);
            const UINT width = std::min(w, max_width);
            const UINT height = std::max(1U, static_cast<UINT>(static_cast<unsigned long long>(h) * width / w));
            ComPtr<IWICBitmapScaler> scaler;
            winrt::check_hresult(wic->CreateBitmapScaler(&scaler));
            winrt::check_hresult(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant));
            ComPtr<IWICFormatConverter> converter;
            winrt::check_hresult(wic->CreateFormatConverter(&converter));
            winrt::check_hresult(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            winrt::check_hresult(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
        } catch (const winrt::hresult_error&) {
            bitmap.Reset(); // Not a picture WIC reads: the placeholder stays.
        }
        return bitmap;
    }
    ThemeId canvas_theme{ThemeId::Jadyx};
    static constexpr size_t MaxCovers = 48;
    static constexpr size_t MaxBackdrops = 8;
    static constexpr unsigned BackdropWidth = 320, BackdropHeight = 180;
    static constexpr float BackdropOpacity = Canvas::Unit0_58F;
    /// The backdrop's drift moves a pixel or so a frame over blurred art: 30 frames a second show
    /// it as smooth as 60, at half the redraws of an idle library. Presses still draw at once.
    static constexpr auto AmbientFrame = std::chrono::milliseconds{30};
    /// Two screens of the grid and a mod's gallery: 640 px wide at most, ~0.6 MiB each.
    static constexpr size_t MaxPictures = 48;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID2D1DeviceContext> target;
    ComPtr<IWICImagingFactory> wic;
    std::array<ComPtr<ID2D1PathGeometry>, 5> controller_paths;
    ComPtr<ID2D1PathGeometry> pad_outline; ///< The header's controller (DrawPadGlyph).
    PromptFamily prompt_family{PromptFamily::Keyboard};
    HRESULT last_draw_error{S_OK}; ///< Logged once, not every frame.
    bool swap_prompt_faces{};
    std::filesystem::path asset_root;
    std::wstring version_text;
    /// Where the pointer can press, as last drawn.
    struct Hit {
        D2D1_RECT_F rect;
        LibraryTarget target;
    };
    std::vector<Hit> hits;
    LibraryLayout::LibraryTab shown_tab{LibraryLayout::LibraryTab::Count};
    bool header_shown{};
    TileKey focused_tile, left_tile;
    bool rows_shown{};
    bool animating{};
    bool ambient{}; ///< The backdrop shows, so it drifts.
    Tween::Clock::time_point ambient_epoch{Tween::Clock::now()}, last_frame{};
    std::string hero_path;
    Tween management_collapse{0, std::chrono::milliseconds{180}, EaseFocus};
    /// The first row of a manager's list and of the store's grid, gliding between rows.
    Tween list_scroll{0, std::chrono::milliseconds{160}, EaseFocus};
    Tween grid_scroll{0, std::chrono::milliseconds{220}, EaseFocus};
    Tween scroll{0, std::chrono::milliseconds{320}, EaseFocus};
    Tween focus_in{1, std::chrono::milliseconds{220}, EaseFocus};
    Tween focus_out{0, std::chrono::milliseconds{220}, EaseFocus};
    Tween hero{1, std::chrono::milliseconds{360}, EaseHero};
    Tween backdrop_fade{1, std::chrono::milliseconds{400}, EaseHero};
    Tween tab_slide{0, std::chrono::milliseconds{260}, EaseFocus};
    Tween filter_slide{0, std::chrono::milliseconds{260}, EaseFocus};
    ComPtr<ID2D1RoundedRectangleGeometry> cover_mask;
    /// Blurred covers, least recently used first.
    std::vector<std::pair<std::string, ComPtr<ID2D1Bitmap1>>> backdrops;
    std::string backdrop_now, backdrop_before;
    ComPtr<ID2D1Effect> backdrop_border, backdrop_blur, backdrop_color;
    std::array<ComPtr<ID2D1Brush>, 4> scrims;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap1>> covers;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> pictures;
    std::unordered_map<unsigned, ComPtr<ID2D1RoundedRectangleGeometry>> picture_masks;
};
LibraryCanvas::LibraryCanvas(void* window, unsigned width, unsigned height)
    : impl{std::make_unique<Impl>(window, width, height)} {}
LibraryCanvas::~LibraryCanvas() = default;
void LibraryCanvas::InvalidateCovers() { impl->InvalidateCovers(); }
void LibraryCanvas::Draw(const LibraryView& view) { impl->Draw(view); }
bool LibraryCanvas::Animating() const { return impl->Animating(); }
LibraryTarget LibraryCanvas::HitAt(float x, float y) const { return impl->HitAt(x, y); }
} // namespace Jadyx
