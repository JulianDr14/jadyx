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
namespace {
/// What settings show for the keys and the firmware: their versions once they are known.
std::wstring KeysValue(const FileSetupStatus& status) {
    if (!status.keys_ready) return std::wstring{Tr(T::KeysMissing)};
    return status.versions.key_generation < 0 ? std::wstring{Tr(T::KeysReady)} :
        I18n::Format(T::KeysGeneration, {std::to_wstring(status.versions.key_generation)});
}
std::wstring FirmwareValue(const FileSetupStatus& status) {
    return status.versions.firmware.empty() ? I18n::Format(T::FirmwareFiles, {std::to_wstring(status.firmware_files)}) :
        I18n::Format(T::FirmwareVersion, {std::wstring{winrt::to_hstring(status.versions.firmware)}});
}
} // namespace
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
        const float scale = std::min(width / 1280.0f, height / 720.0f);
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
        target->Clear(D2D1::ColorF(Palette::Canvas));
        if (!management && !configuration.network_screen) DrawBackdrop(now);
        // The backdrop moves on its own while it shows (AmbientDue).
        ambient = !management && !configuration.network_screen && !backdrop_now.empty();
        last_frame = now;
        if(configuration.confirm_import){
            rows_shown=false;
            Type(Tr(T::SystemImportReview),TypeRole::HeadlineLarge,80,80,1120,Palette::Text);
            Type(configuration.review_text,TypeRole::BodyLarge,80,190,1120,Palette::Warning,1,TextAlign::Leading,10);
            for(size_t i=0;i<2;++i){const float x=80+static_cast<float>(i)*570;const bool focused=configuration.network_action==i;
                Round(x,590,540,64,14,focused?Palette::Accent:Palette::Surface);
                Type(Tr(i==0?T::Cancel:T::Import),TypeRole::LabelLarge,x+20,611,500,focused?Palette::OnAccent:Palette::Text);
                AddHit(x,590,540,64,LibraryHit::NetworkAction,i);
            }
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
            Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.62f);
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
                 LibraryLayout::Margin, 220, 1000, Palette::Text, 1, TextAlign::Leading, 2);
            Type(Tr(view.loading ? T::LoadingBody : T::EmptyBody), TypeRole::BodyLarge,
                 LibraryLayout::Margin, 330, 900, Palette::TextSecondary, 1, TextAlign::Leading, 3);
            if (!view.notice.empty())
                Type(view.notice, TypeRole::Body, LibraryLayout::Margin, 430, 1000, Palette::Warning,
                     1, TextAlign::Leading, 2);
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
            Round(0, 0, 1280, 94, 0, Palette::Canvas);
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
        const float text_width = std::min(300.0f, MeasureType(text, TypeRole::LabelLarge) + 4);
        const float view_width = PromptWidth(Navigation::Pin, view_label);
        const float width = 20 + text_width + 24 + view_width + PromptWidth(Navigation::Rescan, hide_label) - 28 + 16;
        const float x = 1280 - LibraryLayout::Margin - width, y = 98;
        // Dot, text (20 px line) and prompts (32 px line) share the card's center line.
        const float center = y + 24;
        Card(x, y, width, 48, 14);
        Round(x + 12, center - 4, 8, 8, 4, Palette::Accent);
        Type(text, TypeRole::LabelLarge, x + 28, center - 10, text_width, Palette::Text);
        float next = x + 28 + text_width + 16;
        AddHit(next - 4, y, view_width - 20, 48, LibraryHit::UpdateNotice, 0);
        next += ControlPrompt(Navigation::Pin, view_label, next, center - 16);
        AddHit(next - 4, y, x + width - next + 4, 48, LibraryHit::UpdateNotice, 1);
        ControlPrompt(Navigation::Rescan, hide_label, next, center - 16);
    }
    /// The library's corner card after a guided install: this launch runs the new version.
    void DrawUpdatedNotice(const Updates::Version& installed) {
        const auto version = winrt::to_hstring(Updates::FormatVersion(installed));
        const auto text = I18n::Format(T::UpdaterUpdatedTo, {version.c_str()});
        const float text_width = std::min(360.0f, MeasureType(text, TypeRole::LabelLarge) + 4);
        const float width = 28 + text_width + 20;
        const float x = 1280 - LibraryLayout::Margin - width, y = 98, center = y + 24;
        Card(x, y, width, 48, 14);
        Round(x + 12, center - 4, 8, 8, 4, Palette::Accent);
        Type(text, TypeRole::LabelLarge, x + 28, center - 10, text_width, Palette::Text);
    }
    /// The update page's footer: A selects (when a row is there), B goes back.
    void DrawUpdatesFooter(bool select) {
        const float y = LibraryLayout::FooterTop + 12;
        float x = LibraryLayout::Margin;
        if (select) x += ControlPrompt(Navigation::Play, Tr(T::Select), x, y);
        const auto back = Tr(T::UpdaterBack);
        AddHit(x - 4, y - 6, PromptWidth(Navigation::Back, back) - 20, 44, LibraryHit::UpdateAction, UpdateBackHit);
        ControlPrompt(Navigation::Back, back, x, y);
    }
    void DrawUpdates(const SystemPage& page) {
        using namespace LibraryLayout;
        const auto& state = page.update;
        const auto release_version =
            state.release ? std::wstring{winrt::to_hstring(Updates::FormatVersion(state.release->version))} : std::wstring{};
        Round(0, 0, 1280, 720, 0, Palette::Canvas);
        if (page.update_detail && state.release) {
            // Every note, scrolled with the d-pad.
            Type(I18n::Format(T::UpdaterNotesTitle, {release_version}), TypeRole::HeadlineLarge, Margin, 40, 1150, Palette::Text);
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
                Type(lines[first + n], TypeRole::Body, Margin, 116 + static_cast<float>(n) * 32, 1150, Palette::Text);
            DrawUpdatesFooter(false);
            return;
        }
        Type(Tr(T::UpdaterTitle), TypeRole::HeadlineLarge, Margin, 40, 700, Palette::Text);
        // The state card: what is happening, the versions and what comes next.
        constexpr float card_x = Margin, card_y = 112, card_w = 660, card_h = 236, inner = card_x + 28, inner_w = card_w - 56;
        Card(card_x, card_y, card_w, card_h);
        Type(Tr(Updates::PhaseText(state)), TypeRole::HeadlineMedium, inner, card_y + 24, inner_w, Palette::Text);
        const auto installed = winrt::to_hstring(Updates::FormatVersion(state.installed));
        Type(state.release ? I18n::Format(T::UpdaterVersions, {installed.c_str(), release_version})
                           : I18n::Format(T::UpdaterInstalled, {installed.c_str()}),
             TypeRole::BodyLarge, inner, card_y + 70, inner_w, Palette::TextHint);
        if (state.release) {
            const auto published = winrt::to_hstring(state.release->published.substr(0, 10));
            Type(I18n::Format(T::UpdaterReleaseInfo, {std::to_wstring(state.release->size / (1024 * 1024)), published.c_str()}),
                 TypeRole::Body, inner, card_y + 100, inner_w, Palette::TextSecondary);
        }
        const bool progress = state.received && state.total &&
            (state.phase == Updates::Phase::Downloading || state.phase == Updates::Phase::Paused ||
             state.phase == Updates::Phase::Verifying || state.phase == Updates::Phase::Failed);
        float body_y = card_y + 140;
        if (progress) {
            const auto pct = std::min<std::uint64_t>(100, state.received * 100 / state.total);
            Round(inner, body_y, inner_w, 10, 5, Palette::Graphite);
            if (pct) Round(inner, body_y, inner_w * static_cast<float>(pct) / 100, 10, 5, Palette::Accent);
            auto eta = std::wstring{Tr(T::UpdaterEta)};
            if (state.bytes_per_second > 0 && state.total >= state.received) {
                const auto seconds = static_cast<unsigned>(std::min(3599999.0, (state.total - state.received) / state.bytes_per_second));
                eta = std::to_wstring(seconds / 3600) + L"h " + std::to_wstring(seconds / 60 % 60) + L"m " + std::to_wstring(seconds % 60) + L"s";
            }
            const auto speed = std::to_wstring(static_cast<unsigned>(state.bytes_per_second * 10 / (1024 * 1024)));
            const auto speed_text = speed.size() > 1 ? speed.substr(0, speed.size() - 1) + L"." + speed.back() : L"0." + speed;
            const bool moving = state.phase == Updates::Phase::Downloading;
            Type(moving ? I18n::Format(T::UpdaterProgress, {std::to_wstring(pct), std::to_wstring(state.received / (1024 * 1024)),
                              std::to_wstring(state.total / (1024 * 1024)), speed_text, eta})
                        : std::to_wstring(pct) + L"% · " + std::to_wstring(state.received / (1024 * 1024)) + L" / " +
                              std::to_wstring(state.total / (1024 * 1024)) + L" MiB",
                 TypeRole::Body, inner, body_y + 16, inner_w, Palette::TextSecondary);
            body_y += 46;
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
        if (body) Type(Tr(*body), TypeRole::Body, inner, body_y, inner_w, body_color, 1, TextAlign::Leading,
                       static_cast<unsigned>(std::max(1.0f, (card_y + card_h - 16 - body_y) / 22)));
        // The release notes, on the page: the full list is one row away.
        if (state.release && !state.release->notes.empty()) {
            Type(I18n::Format(T::UpdaterNotesTitle, {release_version}), TypeRole::HeadlineSmall, Margin, 376, card_w, Palette::Text);
            Type(winrt::to_hstring(state.release->notes).c_str(), TypeRole::Body, Margin, 414, card_w, Palette::TextHint, 1,
                 TextAlign::Leading, 9);
        }
        std::wstring checked = std::wstring{Tr(T::UpdaterNever)};
        if (state.last_check) {
            const std::time_t stamp = state.last_check; std::tm local{}; localtime_s(&local, &stamp);
            wchar_t date[64]{}; std::wcsftime(date, 64, L"%Y-%m-%d %H:%M", &local); checked = date;
        }
        Type(I18n::Format(T::UpdaterLastCheck, {checked}), TypeRole::Label, 1280 - Margin - 400, FooterTop + 20, 400,
             Palette::TextSecondary, 1, TextAlign::Trailing);
        // The rows: only what can be done now; the focused one explains itself underneath.
        const auto actions = Updates::Actions(state);
        constexpr float rows_x = 780, rows_w = 1280 - Margin - rows_x;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const bool focused = i == std::min(page.update_row, actions.size() - 1);
            const float y = card_y + static_cast<float>(i) * 64;
            Round(rows_x, y, rows_w, 54, 12, focused ? Palette::Accent : Palette::Surface);
            Type(Updates::ActionLabel(actions[i], state), TypeRole::BodyLarge, rows_x + 20, y + 14, rows_w - 40,
                 focused ? Palette::OnAccent : Palette::Text);
            AddHit(rows_x, y, rows_w, 54, LibraryHit::UpdateAction, i);
        }
        if (!actions.empty()) {
            const auto focused = actions[std::min(page.update_row, actions.size() - 1)];
            float y = card_y + static_cast<float>(actions.size()) * 64 + 8;
            y += Type(Tr(Updates::ActionHint(focused, state)), TypeRole::Body, rows_x + 4, y, rows_w - 8, Palette::TextSecondary, 1,
                      TextAlign::Leading, 5);
            if (focused == Updates::Action::Channel && state.channel == Updates::Channel::Preview)
                Type(Tr(T::UpdaterPreviewWarning), TypeRole::Body, rows_x + 4, y + 8, rows_w - 8, Palette::Warning, 1,
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
    static constexpr float SegmentPad = 18, SegmentInset = 4;
    float SegmentedWidth(const std::vector<std::wstring_view>& labels) {
        float width = 2 * SegmentInset;
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
            const float width = MeasureType(label, TypeRole::LabelLarge) + 2 * SegmentPad;
            places.push_back({cursor, width});
            cursor += width;
        }
        const float total = cursor + SegmentInset - x;
        Round(x, y, total, height, height / 2, Palette::Graphite, 0.78f);
        Outline(x + 0.5f, y + 0.5f, total - 1, height - 1, height / 2 - 0.5f, 0xffffff, focused ? 0.85f : 0.12f,
                focused ? 1.5f : 1.0f);
        // The capsule between the two segments it is passing.
        const std::size_t from = std::min(static_cast<std::size_t>(std::max(slide, 0.0f)), places.size() - 1);
        const std::size_t to = std::min(from + 1, places.size() - 1);
        const float t = std::clamp(slide - static_cast<float>(from), 0.0f, 1.0f);
        const float capsule_x = places[from].x + (places[to].x - places[from].x) * t;
        const float capsule_w = places[from].width + (places[to].width - places[from].width) * t;
        constexpr float inset = SegmentInset;
        Round(capsule_x, y + inset, capsule_w, height - 2 * inset, (height - 2 * inset) / 2, Palette::Accent);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            Type(labels[i], TypeRole::LabelLarge, places[i].x, y + (height - 20) / 2, places[i].width,
                 i == active ? Palette::OnAccent : Palette::TextSecondary, 1, TextAlign::Center);
            AddHit(places[i].x, y, places[i].width, height, hit, i);
        }
    }
    /// The wordmark; the tabs in the middle; the controller, the time and the user at the right.
    void DrawHeader(const LibraryView& view, Tween::Clock::time_point now) {
        using namespace LibraryLayout;
        Type(L"J A D Y X", TypeRole::HeadlineSmall, Margin, HeaderTop + 6, 200, Palette::Text);
        const float mark = Margin + MeasureType(L"J A D Y X", TypeRole::HeadlineSmall) + 14;
        Type(view.panel.xbox ? L"SERIES X|S" : L"PC", TypeRole::Caps, mark, HeaderTop + 13, 160, Palette::TextSecondary);
        // The tabs, with the bumpers that switch them at their ends.
        const std::vector<std::wstring_view> tabs{Tr(T::TabHome), Tr(T::TabLibrary), Tr(T::TabSettings)};
        const auto active = static_cast<std::size_t>(view.tab);
        if (header_shown) tab_slide.Set(static_cast<float>(active), now);
        else tab_slide.Snap(static_cast<float>(active));
        header_shown = true;
        const float width = SegmentedWidth(tabs);
        const float left = (1280 - width) / 2;
        Segmented(tabs, left, HeaderTop, HeaderButtonHeight, tab_slide.Value(now), active, false, LibraryHit::Tab);
        const float middle = HeaderTop + HeaderButtonHeight / 2;
        const bool keyboard = prompt_family == PromptFamily::Keyboard;
        const bool nintendo = prompt_family == PromptFamily::Nintendo;
        const std::wstring_view previous = keyboard ? L"Q" : nintendo ? L"L" : L"LB";
        const std::wstring_view next = keyboard ? L"E" : nintendo ? L"R" : L"RB";
        // On a management page bumpers belong to its categories, as in the reference.
        if (!view.game_options.game && !view.game_options.content) {
            Badge(previous, left - 10 - BadgeWidth(previous) / 2, middle, Palette::TextSecondary);
            Badge(next, left + width + 10 + BadgeWidth(next) / 2, middle, Palette::TextSecondary);
        }
        // Right to left: the user, a hairline, the time and the controller (pressing it opens its panel).
        const std::wstring_view name = view.system.settings.user_name.empty()
                                           ? DefaultUserName
                                           : std::wstring_view{view.system.settings.user_name};
        const float name_width = std::min(MeasureType(name, TypeRole::LabelLarge) + 2, 150.0f);
        float right = 1280 - Margin - name_width;
        Type(name, TypeRole::LabelLarge, right, middle - 10, name_width, Palette::Text);
        right -= 10 + 30;
        Round(right, middle - 15, 30, 30, 15, Palette::Graphite);
        Outline(right + 0.5f, middle - 14.5f, 29, 29, 14.5f, 0xffffff, 0.2f);
        Type(Upper(name.substr(0, 1)), TypeRole::LabelLarge, right, middle - 10, 30, Palette::Text, 1,
             TextAlign::Center);
        right -= 16;
        Round(right, middle - 11, 1, 22, 0, 0xffffff, 0.18f);
        wchar_t clock[32]{};
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, clock, 32) > 0) {
            const float clock_width = MeasureType(clock, TypeRole::LabelLarge) + 2;
            right -= 16 + clock_width;
            Type(clock, TypeRole::LabelLarge, right, middle - 10, clock_width, Palette::Text);
        }
        const float status_right = right - 16;
        if (view.battery.known) {
            right -= 16 + BatteryWidth;
            DrawBattery(view.battery, right, middle);
            right += 8; // The battery sits close to its controller.
        }
        right -= 16 + PadWidth;
        const bool connected = view.panel.xbox || !view.panel.devices.empty();
        DrawPadGlyph(right, middle, connected ? Palette::Text : Palette::TextSecondary, connected ? 1.0f : 0.7f);
        AddHit(right - 6, HeaderTop, status_right - right + 12, HeaderButtonHeight, LibraryHit::Controller);
    }
    /// The header's controller: an Xbox pad's silhouette, drawn as a line icon in a 32x22 box.
    static constexpr float PadWidth = 30;
    void DrawPadGlyph(float x, float middle, unsigned color, float alpha) {
        const float scale = PadWidth / 32;
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
        brush->SetColor(D2D1::ColorF(color, 0.10f * alpha));
        target->FillGeometry(pad_outline.Get(), brush.Get());
        brush->SetColor(D2D1::ColorF(color, alpha));
        target->DrawGeometry(pad_outline.Get(), brush.Get(), 1.7f);
        // As on an Xbox Series pad: the left stick high and the face buttons' diamond across
        // from it, the d-pad and the right stick low and inward, the Xbox button between the
        // bumpers with View and Menu under it.
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(8.4f, 7), 2.4f, 2.4f), brush.Get(), 1.4f);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(20, 11.8f), 2.1f, 2.1f), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(10.3f, 11.8f), D2D1::Point2F(13.7f, 11.8f), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(12, 10.1f), D2D1::Point2F(12, 13.5f), brush.Get(), 1.4f);
        constexpr D2D1_POINT_2F face[] = {{23.6f, 4.8f}, {23.6f, 9.2f}, {21.4f, 7}, {25.8f, 7}};
        for (const auto button : face) target->FillEllipse(D2D1::Ellipse(button, 1.05f, 1.05f), brush.Get());
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(16, 4.4f), 1.7f, 1.7f), brush.Get(), 1.2f);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(16, 4.4f), 0.8f, 0.8f), brush.Get());
        for (const float side : {13.8f, 18.2f})
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(side, 8), 0.6f, 0.6f), brush.Get());
        target->SetTransform(previous);
    }
    /// The battery: four bars inside a rounded case, amber when low, a bolt while charging.
    static constexpr float BatteryWidth = 24;
    void DrawBattery(const ControllerBattery& battery, float x, float middle) {
        const float height = 12, top = middle - height / 2, body = BatteryWidth - 3;
        const bool low = battery.level < 0.2f && !battery.charging;
        const unsigned color = low ? Palette::Warning : Palette::Text;
        Outline(x + 0.5f, top + 0.5f, body - 1, height - 1, 3, color, 0.75f, 1.2f);
        Round(x + body, middle - 2.5f, 2, 5, 1, color, 0.75f);
        // Bars from the level, at least one while there is charge left.
        const int bars = battery.level <= 0 ? 0 : std::clamp(static_cast<int>(std::ceil(battery.level * 4 - 0.01f)), 1, 4);
        const float inner = body - 6, gap = 1.5f, bar = (inner - 3 * gap) / 4;
        for (int i = 0; i < bars; ++i)
            Round(x + 3 + i * (bar + gap), top + 3, bar, height - 6, 0.8f, color);
        if (battery.charging) {
            // A bolt over the case, cut out of it with the canvas colour.
            const float cx = x + body / 2, cy = middle;
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
                    brush->SetColor(D2D1::ColorF(Palette::Canvas));
                    target->DrawGeometry(bolt.Get(), brush.Get(), 2.4f);
                    brush->SetColor(D2D1::ColorF(Palette::Text));
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
        return tile == focused_tile ? focus_in.Value(now) : tile == left_tile ? focus_out.Value(now) : 0.0f;
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
        brush->SetColor(D2D1::ColorF(color, alpha));
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
        const float lift = (1 - shown) * 10;
        constexpr float width = 860;
        const unsigned lines = MeasureType(game.name, TypeRole::Hero) > width ? 2 : 1;
        const float title = 304 - 64.0f * lines + lift;
        std::wstring kicker{L"NINTENDO SWITCH"};
        if (!game.developer.empty()) kicker += L"  ·  " + Upper(game.developer);
        Type(kicker, TypeRole::Caps, Margin, title - 24, width, Palette::TextSecondary, shown);
        Type(game.name, TypeRole::Hero, Margin, title, width, Palette::Text, shown, TextAlign::Leading, lines);
        // Only what the game and the play history say; a chip that would not fit is left out.
        float x = Margin;
        const float y = 314 + lift;
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
        const float options_x = Margin + play_width + 14;
        const float options_width = PillButton(Tr(T::Options), menu.letters, menu.color, options_x, PillTop,
                                               false, PillHeight);
        AddHit(options_x, PillTop, options_width, PillHeight, LibraryHit::Options);
        if (!view.notice.empty()) {
            const float notice_x = options_x + options_width + 24;
            Type(view.notice, TypeRole::Body, notice_x, PillTop + 11, 1280 - Margin - notice_x, Palette::Warning);
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
                Type(Tr(in_rail ? T::RecentAndPinned : T::FullCollection), TypeRole::HeadlineSmall, Margin,
                     top - HeadingHeight, 700, Palette::Text, alpha);
                const auto kicker = in_rail ? Upper(TitlesCount(rail))
                                  : focus.rail ? TitlesCount(count)
                                               : std::to_wstring(focus.selected + 1) + L" / " + std::to_wstring(count);
                Type(kicker, in_rail ? TypeRole::Caps : TypeRole::Label, 1280 - Margin - 240,
                     top - HeadingHeight + 8, 240, Palette::TextSecondary, alpha, TextAlign::Trailing);
            }
            for (std::size_t i = first; i < end; ++i) {
                const float x = TileLeft(i - first);
                const TileKey tile{i, in_rail};
                const float amount = TileAmount(tile, now);
                if (amount > 0.001f) lifted[tile == focused_tile ? 1 : 0] = {&entries[i], x, top, amount, alpha};
                else DrawTile(view, entries[i], x, top, 0, alpha);
                // Rows fading under the hero do not take presses.
                if (alpha > 0.5f) AddHit(x, top, Tile, Tile + 44, in_rail ? LibraryHit::RailTile : LibraryHit::Tile, i);
            }
        }
        for (const auto& tile : lifted)
            if (tile.entry) DrawTile(view, *tile.entry, tile.x, tile.y, tile.amount, tile.alpha);
        // The last row's covers end under the prompts: say there is more down there.
        if (rows && RowTop + RowContentTop(rows - 1, rail) - offset + Tile > FooterTop)
            MoreBelow(1280 - Margin / 2, FooterTop - 20, 0.8f);
    }
    /// Rows settle at RowTop; above it they fade out before reaching the hero's actions.
    static float RowAlpha(float top) {
        return std::clamp((top - 330) / 144, 0.0f, 1.0f);
    }
    /// A cover, its title and its meta line; `amount` (0..1) of the way into focus: larger,
    /// lifted, ringed and with a brighter title.
    void DrawTile(const LibraryView& view, const LibraryEntry& entry, float x, float y, float amount, float alpha) {
        using LibraryLayout::Tile;
        const float size = Tile * (1 + 0.06f * amount);
        const float left = x + (Tile - size) / 2;
        const float top = y + (Tile - size) / 2 - 8 * amount;
        FocusGlow(left, top, size, size, 12, amount * alpha);
        Cover(entry, left, top, size, alpha);
        DrawCompatibility(CompatibilityFor(view, entry).status, left + 18, top + 18, alpha);
        if (entry.pinned) {
            // A pin's head: a white dot in a graphite disc, at the top right.
            const float cx = left + size - 18, cy = top + 18;
            Round(cx - 10, cy - 10, 20, 20, 10, Palette::Graphite, 0.9f * alpha);
            Round(cx - 4, cy - 4, 8, 8, 4, Palette::Text, alpha);
        }
        const float text = y + Tile + 10 + 6 * amount;
        Type(entry.name, TypeRole::LabelLarge, x, text, Tile, amount > 0.5f ? Palette::Text : Palette::TextSecondary,
             alpha);
        std::wstring under = entry.display_version.empty() ? L"" : L"v" + entry.display_version;
        if (view.system.settings.track_play_time && entry.play_seconds >= 60) {
            if (!under.empty()) under += L" · ";
            under += FormatPlayTimeShort(entry.play_seconds);
        }
        if (!entry.pulse_saved.empty()) under += under.empty() ? L"Pulse" : L" · Pulse";
        if (!under.empty())
            Type(under, TypeRole::Label, x, text + 20, Tile, Palette::TextSecondary, alpha * (0.7f + 0.3f * amount));
    }
    /// The Library tab: the heading and the filter on one line, the filtered collection under them.
    void DrawShelf(const LibraryView& view, Tween::Clock::time_point now) {
        using namespace LibraryLayout;
        const auto& entries = view.scan.entries;
        static const std::vector<std::size_t> none;
        const auto& shelf = view.shelf ? *view.shelf : none;
        const bool import = HasImportTile(view.filter);
        const std::size_t count = shelf.size() + (import ? 1 : 0);
        Type(Tr(T::FullCollection), TypeRole::HeadlineLarge, Margin, ShelfHeadingTop - 6, 560, Palette::Text);
        Type(TitlesCount(shelf.size()), TypeRole::Body, Margin, ShelfHeadingTop + 46, 400, Palette::TextSecondary);
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
            Type(message, TypeRole::BodyLarge, Margin, ShelfTop + 20, 900, Palette::TextSecondary, 1,
                 TextAlign::Leading, 2);
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
            const float alpha = std::clamp((top - (ShelfTop - 150)) / 150, 0.0f, 1.0f) *
                                std::clamp((FooterTop - 8 - top) / 130, 0.0f, 1.0f);
            if (alpha <= 0) continue;
            const TileKey tile{i, false};
            const float amount = TileAmount(tile, now);
            if (i == shelf.size()) DrawImportTile(x, top, amount, alpha);
            else if (amount > 0.001f) lifted[tile == focused_tile ? 1 : 0] = {&entries[shelf[i]], x, top, amount, alpha};
            else DrawTile(view, entries[shelf[i]], x, top, 0, alpha);
            if (alpha > 0.5f)
                AddHit(x, top, Tile, Tile + 44, i == shelf.size() ? LibraryHit::Import : LibraryHit::ShelfTile, i);
        }
        for (const auto& tile : lifted)
            if (tile.entry) DrawTile(view, *tile.entry, tile.x, tile.y, tile.amount, tile.alpha);
        // The last row's covers end under the prompts: say there is more down there.
        if (ShelfTop + static_cast<float>((count - 1) / Columns) * ShelfRowHeight - offset + Tile > FooterTop)
            MoreBelow(1280 - Margin / 2, FooterTop - 20, 0.8f);
    }
    /// The collection's last tile: opens the file manager to add games.
    void DrawImportTile(float x, float y, float amount, float alpha) {
        using LibraryLayout::Tile;
        const float size = Tile * (1 + 0.06f * amount);
        const float left = x + (Tile - size) / 2;
        const float top = y + (Tile - size) / 2 - 8 * amount;
        FocusGlow(left, top, size, size, 12, amount * alpha);
        Round(left, top, size, size, 12, Palette::Graphite, 0.55f * alpha);
        Outline(left + 0.5f, top + 0.5f, size - 1, size - 1, 11.5f, 0xffffff, 0.16f * alpha);
        const float cx = left + size / 2, cy = top + size / 2 - 8;
        Round(cx - 14, cy - 1.5f, 28, 3, 1.5f, Palette::Text, alpha);
        Round(cx - 1.5f, cy - 14, 3, 28, 1.5f, Palette::Text, alpha);
        Type(L"NSP · XCI", TypeRole::Caps, left, cy + 26, size, Palette::TextSecondary, alpha, TextAlign::Center);
        const float text = y + Tile + 10 + 6 * amount;
        Type(Tr(T::ImportTitle), TypeRole::LabelLarge, x, text, Tile,
             amount > 0.5f ? Palette::Text : Palette::TextSecondary, alpha);
        Type(Tr(T::ImportTitleHint), TypeRole::Label, x, text + 20, Tile, Palette::TextSecondary,
             alpha * (0.7f + 0.3f * amount));
    }
    /// What the tab's buttons do, along the bottom; the installed version at the right.
    void DrawFooter(const LibraryView& view) {
        using namespace LibraryLayout;
        const float y = FooterTop + 12;
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
            Type(notice, TypeRole::Label, 1280 - Margin - 600, y + 8, 600, color, 1, TextAlign::Trailing);
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
        Type(version_text, TypeRole::Label, 1280 - Margin - 160, y + 8, 160, Palette::TextSecondary, 0.7f,
             TextAlign::Trailing);
    }
    void DrawControllerPanel(const LibraryView& view) {
        const auto& options = view.options;
        const auto& panel = view.panel;
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.72f));
        target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
        Round(620, 133, 586, 487, 24, Palette::Surface);
        Text(Tr(T::ControllerTitle), 658, 168, 34, Palette::Text, 490, 55, true);
        Text(Tr(T::ControllerSubtitle), 658, 227, 21, Palette::TextSecondary, 490);
        const unsigned offset = panel.xbox ? 0 : 1;
        const auto active = SelectController(panel.devices, options.controller_id);
        const bool nintendo = active && panel.devices[*active].nintendo;
        for (unsigned row = 0; row < 3 + offset; ++row) {
            const float y = ControllerRowTop + row * ControllerRowHeight;
            Round(648, y, 530, 56, 14, row == view.controller_row ? Palette::RowActive : Palette::Row);
            const bool device = offset && row == 0;
            const bool type = row == offset;
            const bool face = row == offset + 1;
            Text(Tr(device ? T::ControllerDevice : type ? T::ControllerSeenAs :
                    face ? T::FaceButtons : T::DeadZone),
                 669, y + 2, 19, Palette::Text, 400, 28, true);
            if (device || type)
                Text(device ? L"v" : L"<  >", 1080, y + 2, 19, Palette::Text, 80, 28, true, false, false, true);
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
            Text(value, 669, y + 28, 18, Palette::Accent, 460, 26);
        }
        if (!panel.xbox) {
            Round(648, 510, 530, 34, 10, view.controller_row == 4 ? Palette::RowActive : Palette::Row);
            Text(std::wstring{Tr(T::SetUpKeyboard)} + L"   >", 669, 512, 19, Palette::Accent, 460, 30, true);
        }
        ControlPrompt(Navigation::Play, Tr(T::Change), 658, 551);
        ControlPrompt(Navigation::Back, Tr(T::Back), 1018, 551);
        if (!panel.notice.empty()) Text(panel.notice, 658, 594, 16, Palette::Accent, 515, 25);
        if (panel.expanded) DrawControllerChoices(view, false);
    }
    /// The controllers to choose from; `dim` darkens what is under it.
    void DrawControllerChoices(const LibraryView& view, bool dim) {
        const auto& panel = view.panel;
        if (dim) Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.6f);
        {
            const size_t first = ControllerChoiceFirst(panel);
            const size_t count = panel.devices.size() + 2;
            const size_t end = std::min(count, first + ControllerVisibleChoices);
            Round(648, ControllerChoiceTop - 4, 530,
                  (end - first) * ControllerChoiceHeight + 8, 12, Palette::RowActive);
            for (size_t i = first; i < end; ++i) {
                const float y = ControllerChoiceTop + (i - first) * ControllerChoiceHeight;
                Round(652, y, 522, 40, 8, i == panel.choice ? Palette::RowActive : Palette::Row);
                const bool session = i >= 2 && panel.devices[i - 2].id.starts_with(L"session:");
                const bool supported = i < 2 || (bool(panel.devices[i - 2]) && !session);
                std::wstring label;
                if (i == 0) label = Tr(T::AutoFirstController);
                else if (i == 1) label = Tr(T::Keyboard);
                else label = panel.devices[i - 2].name + L" · " +
                    std::wstring{Tr(supported ? (panel.devices[i - 2].wireless ? T::Wireless : T::Usb) :
                                    session ? T::AutomaticOnly : T::NoCompatibleMapping)};
                Text(label, 665, y + 6, 18, supported ? Palette::Text : Palette::TextSecondary, 480, 30);
            }
            ScrollIndicator(1187, ControllerChoiceTop, ControllerVisibleChoices * ControllerChoiceHeight,
                            first, ControllerVisibleChoices, count);
        }
    }
    void DrawKeyboard(const KeyboardEditor& editor) {
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.86f));
        target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
        Round(80, 88, 1120, 584, 22, Palette::Surface);
        Text(Tr(T::KeyboardTitle), 120, 118, 28, Palette::Text, 900, 44, true);
        Text(Tr(T::KeyboardSubtitle), 120, 166, 17, Palette::TextSecondary, 650, 30);
        Round(1120, 112, 48, 40, 10, Palette::Graphite);
        brush->SetColor(D2D1::ColorF(Palette::TextHint));
        target->DrawLine(D2D1::Point2F(1137, 125), D2D1::Point2F(1151, 139), brush.Get(), 2);
        target->DrawLine(D2D1::Point2F(1151, 125), D2D1::Point2F(1137, 139), brush.Get(), 2);
        Round(120, 210, 480, 342, 16, Palette::Row);
        Text(Tr(T::ControllerView), 143, 225, 13, Palette::TextSecondary, 220, 24, true);
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
                    const float x = points[i * 2] * (reflected ? -1.0f : 1.0f);
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
            brush->SetColor(D2D1::ColorF(color)); target->FillGeometry(controller_paths[i].Get(), brush.Get());
            brush->SetColor(D2D1::ColorF(0x45484d)); target->DrawGeometry(controller_paths[i].Get(), brush.Get(), 1.2f);
        };
        const size_t action = editor.selected;
        path(1, Palette::Graphite); path(2, Palette::Graphite); path(0, Palette::Graphite);
        path(3, action == 6 ? Palette::Accent : Palette::RowActive); path(4, action == 7 ? Palette::Accent : Palette::RowActive);
        auto circle = [&](float x, float y, float radius, const wchar_t* label, bool active, float font = 16) {
            const auto shape = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
            brush->SetColor(D2D1::ColorF(active ? Palette::Accent : Palette::Row)); target->FillEllipse(shape, brush.Get());
            brush->SetColor(D2D1::ColorF(active ? Palette::Text : 0x45484d)); target->DrawEllipse(shape, brush.Get(), 1.4f);
            Text(label, x - radius, y - radius, font, active ? Palette::OnAccent : Palette::TextHint,
                 radius * 2, radius * 2, true, false, true);
        };
        auto stick = [&](float x, float y, const wchar_t* label, bool active) {
            circle(x, y, 32, L"", active);
            circle(x, y, 22, label, false, 12);
        };
        stick(239, 335, L"L3", action == 4 || (action >= 20 && action < 24));
        stick(401, 390, L"R3", action == 5 || action >= 24);
        circle(486, 334, 15, L"A", action == 0); circle(455, 365, 15, L"B", action == 1);
        circle(455, 303, 15, L"X", action == 2); circle(424, 334, 15, L"Y", action == 3);
        Round(278, 359, 22, 62, 4, Palette::Row); Round(258, 379, 62, 22, 4, Palette::Row);
        for (size_t i = 12; i <= 15; ++i) {
            const auto tile = KeyboardDiagramTile(i);
            Round(tile.x, tile.y, tile.w, tile.h, 3, action == i ? Palette::Accent : Palette::RowActive);
        }
        circle(300, 304, 10, L"-", action == 11, 14); circle(400, 304, 10, L"+", action == 10, 14);
        circle(379, 334, 10, L"", action == 18);
        brush->SetColor(D2D1::ColorF(action == 18 ? Palette::OnAccent : Palette::TextHint));
        target->DrawLine(D2D1::Point2F(374, 334), D2D1::Point2F(379, 330), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(379, 330), D2D1::Point2F(384, 334), brush.Get(), 1.4f);
        target->DrawRectangle(D2D1::RectF(376, 334, 382, 339), brush.Get(), 1.2f);
        Round(314, 327, 14, 14, 3, action == 19 ? Palette::Accent : Palette::Row);
        brush->SetColor(D2D1::ColorF(Palette::TextSecondary));
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(321, 334), 4, 4), brush.Get(), 1.2f);
        for (const auto i : {size_t{8}, size_t{9}}) {
            const auto tile = KeyboardDiagramTile(i);
            Round(tile.x, tile.y, tile.w, tile.h, 8, action == i ? Palette::Accent : Palette::Graphite);
            Text(i == 8 ? L"ZL" : L"ZR", tile.x, tile.y, 14, action == i ? Palette::OnAccent : Palette::TextHint,
                 tile.w, tile.h, true, false, true);
        }
        Text(L"L", 220, 252, 14, action == 6 ? Palette::OnAccent : Palette::TextHint, 60, 22, true, false, true);
        Text(L"R", 450, 252, 14, action == 7 ? Palette::OnAccent : Palette::TextHint, 40, 22, true, false, true);
        Round(120, 564, 480, 67, 12, Palette::RowActive);
        Text(Tr(T::SelectedBinding), 140, 574, 11, Palette::TextSecondary, 300, 19, true);
        Text(KeyboardActionName(action), 140, 594, 20, Palette::Text, 285, 30, true);
        Round(436, 579, 142, 38, 8, Palette::Row);
        Text(editor.capturing ? std::wstring{Tr(T::PressAKey)} : KeyboardKeyName(editor.bindings[action]),
             436, 579, 16, Palette::Accent, 142, 38, true, false, true);
        const std::array<std::wstring_view, 3> pages{Tr(T::KeyPageButtons), Tr(T::KeyPageSystem), Tr(T::KeyPageSticks)};
        for (unsigned page = 0; page < pages.size(); ++page) {
            const float x = 635 + page * 174.0f;
            Round(x, 203, 166, 36, 9, page == editor.page ? Palette::RowActive : Palette::Graphite);
            Text(pages[page], x, 203, 15, page == editor.page ? Palette::Text : Palette::TextSecondary,
                 166, 36, true, false, true);
        }
        for (const size_t i : KeyboardPageActions(editor.page)) {
            const auto tile = KeyboardTileFor(i);
            Round(tile.x, tile.y, tile.w, tile.h, 9, i == action ? Palette::RowActive : Palette::Row);
            Text(KeyboardActionName(i), tile.x + 14, tile.y, 15, Palette::TextHint, 136, tile.h, false, false, true);
            Round(tile.x + 153, tile.y + 9, 86, 27, 6, Palette::Row);
            Text(i == action && editor.capturing ? std::wstring{L"..."} : KeyboardKeyName(editor.bindings[i]),
                 tile.x + 153, tile.y + 9, 14, i == action ? Palette::Accent : Palette::TextSecondary, 86, 27, true, false, true);
        }
        Round(635, 603, 155, 36, 8, Palette::Graphite); Text(Tr(T::ClearBinding), 635, 603, 15, Palette::TextHint, 155, 36, false, false, true);
        Round(806, 603, 170, 36, 8, Palette::Graphite); Text(Tr(T::RestoreDefaults), 806, 603, 15, Palette::TextHint, 170, 36, false, false, true);
        if (!editor.notice.empty()) Text(editor.notice, 990, 609, 13, Palette::TextSecondary, 172, 30);
        Text(Tr(editor.capturing ? T::WaitingForKey : T::KeyboardHelp),
             120, 644, 13, Palette::TextSecondary, prompt_family == PromptFamily::Keyboard ? 1030.0f : 860.0f, 22);
        if (prompt_family != PromptFamily::Keyboard)
            ControlPrompt(Navigation::Back, Tr(T::Back), 1010, 637);
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
                case SystemPage::MiiEditor: item.kind = Kind::Open; break;
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
            SettingsRow keys{Kind::Open, std::wstring{Tr(T::ImportKeys)}, {}, KeysValue(status)};
            keys.warning = !status.keys_ready;
            rows.push_back(std::move(keys));
            rows.push_back({Kind::Open, std::wstring{Tr(T::ImportFirmware)}, {}, FirmwareValue(status)});
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
            for (const auto& source : status.sources)
                rows.push_back({Kind::Open, I18n::Format(T::RemoveFolder, {source.name})});
        }
        return rows;
    }
    static constexpr float SettingsListX = LibraryLayout::Margin, SettingsListWidth = 360;
    static constexpr float SettingsListTop = 132, SettingsListStride = 66, SettingsListHeight = 58;
    static constexpr float SettingsPaneX = SettingsListX + SettingsListWidth + 40;
    static constexpr float SettingsPaneWidth = 1280 - LibraryLayout::Margin - SettingsPaneX;
    static constexpr float SettingsRowsTop = 226, SettingsRowStride = 58, SettingsRowHeight = 52;
    static constexpr std::size_t SettingsVisibleRows = 7;
    /// The code spaced out so it reads at a distance, as the browser asks for it.
    static std::wstring PortalDigits(const std::string& code) {
        std::wstring digits;
        for (const char c : code) { if (!digits.empty()) digits += L"  "; digits += static_cast<wchar_t>(c); }
        return digits;
    }
    /// The portal screen, laid out like the update page: the address and the code in the card at
    /// the left, the two actions as rows at the right with the focused one explained underneath.
    void DrawNetworkConnection(const ConfigurationPanel& configuration) {
        using namespace LibraryLayout;
        const auto& network = configuration.network;
        Type(Tr(T::NetworkConnection), TypeRole::HeadlineLarge, Margin, 40, 700, Palette::Text);
        constexpr float card_x = Margin, card_y = 112, card_w = 660, card_h = 330, inner = card_x + 28, inner_w = card_w - 56;
        Card(card_x, card_y, card_w, card_h);
        Type(Upper(Tr(T::UpdateGuideAddress)), TypeRole::Caps, inner, card_y + 24, inner_w, Palette::TextSecondary);
        Type(winrt::to_hstring(network.url).c_str(), TypeRole::HeadlineMedium, inner, card_y + 50, inner_w, Palette::Text);
        Round(inner, card_y + 106, inner_w, 1, 0.5f, Palette::Graphite);
        Type(Upper(Tr(T::NetworkCodeLabel)), TypeRole::Caps, inner, card_y + 128, inner_w, Palette::TextSecondary);
        if (network.running && !network.code_expired && network.code.size() == 4) {
            D2D1_MATRIX_3X2_F transform;
            target->GetTransform(&transform);
            // Hero is 56px at the reference resolution: 84px here, centred in the card.
            const float center = card_x + card_w / 2, top = card_y + 156;
            target->SetTransform(D2D1::Matrix3x2F::Scale(1.5f, 1.5f, D2D1::Point2F(center, top)) * transform);
            Type(PortalDigits(network.code), TypeRole::Hero, inner, top, inner_w, Palette::Text, 1, TextAlign::Center);
            target->SetTransform(transform);
        } else {
            Type(Tr(network.paired ? T::NetworkPaired : T::NetworkCodeExpired), TypeRole::HeadlineMedium, inner,
                 card_y + 180, inner_w, Palette::Text, 1, TextAlign::Center);
        }
        const bool problem = !configuration.notice.empty();
        Type(problem ? std::wstring_view{configuration.notice} : Tr(T::NetworkCodeHint), TypeRole::Body, inner,
             card_y + 270, inner_w, problem ? Palette::Warning : Palette::TextSecondary, 1, TextAlign::Center, 2);
        // Under the card: what the browser is sending, with its bar.
        std::wstring progress;
        float fraction = 0;
        const auto& transfer = network.transfer;
        const auto& system = network.system;
        if (system.busy) {
            const auto& file = system.file;
            // Keys and firmware have their own phases; the NSP wording does not apply here.
            progress = system.phase == "receiving" && file.busy ? I18n::Format(T::NetworkUploading,
                {winrt::to_hstring(file.name).c_str(), std::to_wstring(file.received >> 20), std::to_wstring(file.length >> 20)}) :
                std::wstring{Tr(system.phase == "receiving" ? T::SystemWaiting :
                                system.phase == "review" ? T::SystemReviewReady :
                                system.phase == "failed" ? T::ImportFailedKept :
                                system.phase == "commit_pending" || system.phase == "publishing" ? T::SystemInstalling :
                                T::SystemValidating)};
            progress += L" · " + std::to_wstring(system.completed) + L" / " + std::to_wstring(system.count);
            fraction = file.length ? static_cast<float>(static_cast<double>(file.received) / file.length) : 0;
        } else if (transfer.busy) {
            progress = transfer.phase == "receiving" ? I18n::Format(T::NetworkUploading,
                {winrt::to_hstring(transfer.name).c_str(), std::to_wstring(transfer.received >> 20), std::to_wstring(transfer.length >> 20)}) :
                std::wstring{Tr(T::NetworkValidating)};
            fraction = transfer.length ? static_cast<float>(static_cast<double>(transfer.received) / transfer.length) : 0;
        }
        if (!progress.empty()) {
            const float y = card_y + card_h + 32;
            Round(card_x, y, card_w, 10, 5, Palette::Graphite);
            if (fraction > 0) Round(card_x, y, card_w * std::clamp(fraction, 0.0f, 1.0f), 10, 5, Palette::Accent);
            Type(progress, TypeRole::Body, card_x, y + 20, card_w, Palette::TextSecondary, 1, TextAlign::Leading, 2);
        } else {
            Type(Tr(T::NetworkOpenHint), TypeRole::Body, card_x, card_y + card_h + 28, card_w, Palette::TextHint, 1,
                 TextAlign::Leading, 2);
        }
        if (network.paired) {
            const auto paired = Tr(T::NetworkPaired);
            const float width = MeasureType(paired, TypeRole::Label);
            Round(1280 - Margin - width - 18, FooterTop + 26, 8, 8, 4, Palette::Accent);
            Type(paired, TypeRole::Label, 1280 - Margin - 400, FooterTop + 20, 400, Palette::TextSecondary, 1,
                 TextAlign::Trailing);
        }
        // The rows (network_action 1 and 2; 0 is going back).
        const std::array<std::pair<T, T>, 2> rows{{{T::NetworkCode, T::NetworkCodeRowHint}, {T::NetworkStop, T::NetworkStopRowHint}}};
        constexpr float rows_x = 780, rows_w = 1280 - Margin - rows_x;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const bool focused = configuration.network_action == i + 1;
            const float y = card_y + static_cast<float>(i) * 64;
            Round(rows_x, y, rows_w, 54, 12, focused ? Palette::Accent : Palette::Surface);
            Type(Tr(rows[i].first), TypeRole::BodyLarge, rows_x + 20, y + 14, rows_w - 40, focused ? Palette::OnAccent : Palette::Text);
            AddHit(rows_x, y, rows_w, 54, LibraryHit::NetworkAction, i + 1);
        }
        if (configuration.network_action >= 1 && configuration.network_action <= rows.size())
            Type(Tr(rows[configuration.network_action - 1].second), TypeRole::Body, rows_x + 4,
                 card_y + static_cast<float>(rows.size()) * 64 + 8, rows_w - 8, Palette::TextSecondary, 1, TextAlign::Leading, 5);
        const float y = FooterTop + 12;
        float x = Margin;
        x += ControlPrompt(Navigation::Play, Tr(T::Select), x, y);
        const auto back = Tr(T::UpdaterBack);
        AddHit(x - 4, y - 6, PromptWidth(Navigation::Back, back) - 20, 44, LibraryHit::NetworkAction, 0);
        ControlPrompt(Navigation::Back, back, x, y);
    }
    /// The guided app update: where to point a browser and its code at the left, the three steps
    /// at the right over the warning that keeps the user's data. Laid out like the portal screen.
    void DrawUpdateGuide(const ConfigurationPanel& configuration, const Updates::Snapshot& update) {
        const auto& network = configuration.network;
        const bool code = network.running && !network.code_expired && network.code.size() == 4;
        Type(L"J A D Y X", TypeRole::HeadlineSmall, 80, 44, 400, Palette::Text);
        const std::wstring installed{winrt::to_hstring(Updates::FormatVersion(update.installed))};
        const std::wstring incoming = update.release ?
            std::wstring{winrt::to_hstring(Updates::FormatVersion(update.release->version))} : installed;
        if (network.paired) {
            const float width = MeasureType(Tr(T::NetworkPaired), TypeRole::LabelLarge);
            Round(1200 - width - 18, 58, 8, 8, 4, Palette::Accent);
            Type(Tr(T::NetworkPaired), TypeRole::LabelLarge, 720, 52, 480, Palette::TextHint, 1, TextAlign::Trailing);
        } else {
            Type(installed + L"  \u2192  " + incoming, TypeRole::LabelLarge, 720, 52, 480, Palette::TextHint, 1,
                 TextAlign::Trailing);
        }
        Type(I18n::Format(T::UpdateGuideTitle, {incoming}), TypeRole::HeadlineLarge, 80, 96, 1120, Palette::Text);
        Type(Tr(T::UpdateGuideIntro), TypeRole::BodyLarge, 80, 150, 1120, Palette::TextSecondary);
        // The address and the code, drawn as large as the portal screen draws them.
        constexpr float card_x = 80, card_y = 200, card_w = 560, card_h = 330, inner = card_x + 28, inner_w = card_w - 56;
        Card(card_x, card_y, card_w, card_h, 20);
        Type(Upper(Tr(T::UpdateGuideAddress)), TypeRole::Caps, inner, card_y + 24, inner_w, Palette::TextSecondary);
        const std::wstring url = network.running ? std::wstring{winrt::to_hstring(network.url)} : std::wstring{L"\u2014"};
        Type(url, TypeRole::HeadlineMedium, inner, card_y + 50, inner_w, Palette::Text);
        Round(inner, card_y + 106, inner_w, 1, 0.5f, Palette::Graphite);
        Type(Upper(Tr(T::NetworkCodeLabel)), TypeRole::Caps, inner, card_y + 128, inner_w, Palette::TextSecondary);
        if (code) {
            D2D1_MATRIX_3X2_F transform;
            target->GetTransform(&transform);
            // Hero is 56px at the reference resolution: 84px here, centred in the card.
            const float center = card_x + card_w / 2, top = card_y + 156;
            target->SetTransform(D2D1::Matrix3x2F::Scale(1.5f, 1.5f, D2D1::Point2F(center, top)) * transform);
            Type(PortalDigits(network.code), TypeRole::Hero, inner, top, inner_w, Palette::Text, 1, TextAlign::Center);
            target->SetTransform(transform);
        } else {
            Type(Tr(network.paired ? T::NetworkPaired : network.running ? T::NetworkCodeExpired : T::NetworkStopped),
                 TypeRole::HeadlineMedium, inner, card_y + 180, inner_w, Palette::Text, 1, TextAlign::Center);
        }
        const bool problem = !configuration.notice.empty();
        Type(problem ? std::wstring_view{configuration.notice} : Tr(T::NetworkCodeHint), TypeRole::Body, inner,
             card_y + 270, inner_w, problem ? Palette::Warning : Palette::TextSecondary, 1, TextAlign::Center, 2);
        // The steps: the done and current ones lit, the rest waiting.
        constexpr float steps_x = 680, steps_w = 520;
        const std::array<std::pair<T, T>, 3> steps{{{T::UpdateGuideConnect, T::UpdateGuideConnectBody},
            {T::UpdateGuideDownload, T::UpdateGuideDownloadBody}, {T::UpdateGuidePortal, T::UpdateGuidePortalBody}}};
        const std::size_t current = network.paired ? 1 : 0;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const float y = card_y + 8 + static_cast<float>(i) * 84;
            const bool lit = i <= current;
            if (i + 1 < steps.size())
                Round(steps_x + 17, y + 44, 2, 36, 1, i < current ? Palette::Accent : Palette::Graphite);
            if (i == current) FocusGlow(steps_x, y, 36, 36, 18, 1);
            Round(steps_x, y, 36, 36, 18, lit ? Palette::Accent : Palette::Surface);
            Type(std::to_wstring(i + 1), TypeRole::LabelLarge, steps_x, y + 8, 36,
                 lit ? Palette::OnAccent : Palette::TextSecondary, 1, TextAlign::Center);
            Type(Tr(steps[i].first), TypeRole::LabelLarge, steps_x + 56, y + 2, steps_w - 56,
                 lit ? Palette::Text : Palette::TextHint);
            Type(Tr(steps[i].second), TypeRole::Body, steps_x + 56, y + 28, steps_w - 56, Palette::TextSecondary, 1,
                 TextAlign::Leading, 2);
        }
        Round(steps_x, card_y + card_h - 66, steps_w, 66, 14, Palette::Warning, 0.12f);
        Type(Tr(T::UpdateGuideWarning), TypeRole::Body, steps_x + 20, card_y + card_h - 56, steps_w - 40,
             Palette::Warning, 1, TextAlign::Leading, 2);
        const std::array<T, 3> labels{T::UpdaterBack, T::NetworkCode, T::NetworkStop};
        for (size_t i = 0; i < labels.size(); ++i) {
            const float x = 80 + static_cast<float>(i) * 380;
            const bool focused = configuration.network_action == i;
            if (focused) FocusGlow(x, 594, 360, 60, 14, 1);
            Round(x, 594, 360, 60, 14, focused ? Palette::Accent : Palette::Surface);
            Type(Tr(labels[i]), TypeRole::LabelLarge, x + 14, 614, 332,
                 focused ? Palette::OnAccent : Palette::Text, 1, TextAlign::Center);
            AddHit(x, 594, 360, 60, LibraryHit::NetworkAction, i);
        }
        ControlPrompt(Navigation::Play, Tr(T::Select), 80, 676);
        ControlPrompt(Navigation::Back, Tr(T::Back), 400, 676);
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
        Type(kicker, TypeRole::Caps, Margin, 92, kicker_width + 4, Palette::TextSecondary);
        Type(L"·", TypeRole::Label, Margin + kicker_width + 12, 92, 12, Palette::TextSecondary, 0.6f);
        Type(L"Jadyx " + version_text, TypeRole::Label, Margin + kicker_width + 28, 92, 300, Palette::TextHint);
        // The categories.
        static constexpr std::array<ConfigurationIcon, HomeRowCount> icons{
            ConfigurationIcon::Settings, ConfigurationIcon::Controller, ConfigurationIcon::Folder};
        static constexpr std::array<T, HomeRowCount> titles{T::System, T::ControllersAndKeyboard, T::FileManager};
        for (std::size_t i = 0; i < HomeRowCount; ++i) {
            const float x = SettingsListX, y = SettingsListTop + i * SettingsListStride;
            const float w = SettingsListWidth, h = SettingsListHeight;
            const bool chosen = i == category;
            if (chosen && !in_pane) {
                FocusGlow(x, y, w, h, 14, 1);
                Round(x, y, w, h, 14, 0x333537);
            } else if (chosen) {
                Round(x, y, w, h, 14, Palette::RowActive, 0.9f);
            }
            std::wstring summary;
            if (i == HomeSystem) {
                summary = SystemRowValue(view.system, SystemPage::UserName) + L" · " +
                          SystemRowValue(view.system, SystemPage::InterfaceLanguage);
            } else if (i == HomeControllers) {
                const auto label = ControllerLabel(view.panel.devices, view.options.controller_id);
                summary = view.options.controller_id.empty() ? I18n::Format(T::AutomaticValue, {label}) : label;
            } else {
                summary = KeysValue(status) + L" · " + FirmwareValue(status);
            }
            const unsigned color = chosen ? Palette::Text : Palette::TextSecondary;
            Icon(icons[i], x + 18, y + (h - 22) / 2, color, 22);
            // Focus only brightens the entry: its text keeps its size and place.
            Type(Tr(titles[i]), TypeRole::LabelLarge, x + 56, y + 10, w - 92, chosen ? Palette::Text : Palette::TextHint);
            Type(summary, TypeRole::Label, x + 56, y + 31, w - 92, chosen ? Palette::TextHint : Palette::TextSecondary,
                 chosen ? 0.85f : 0.7f);
            Chevron(x + w - 22, y + h / 2, color);
            AddHit(x, y, w, h, LibraryHit::Category, i);
        }
        // The emulator's data: what the files on the console add up to.
        {
            const float x = SettingsListX, w = SettingsListWidth;
            const float y = SettingsListTop + static_cast<float>(HomeRowCount) * SettingsListStride + 18;
            Round(x, y, w, 204, 18, Palette::Surface, 0.82f);
            Outline(x + 0.5f, y + 0.5f, w - 1, 203, 18, 0xffffff, 0.06f);
            Type(Tr(T::EmulatorData), TypeRole::Caps, x + 20, y + 20, 200, Palette::TextSecondary);
            const auto keys = Tr(status.keys_ready ? T::KeysReady : T::KeysMissing);
            const float chip = MeasureType(keys, TypeRole::Label) + 24;
            Chip(keys, x + w - 20 - chip, y + 15, status.keys_ready ? Palette::Text : Palette::Warning);
            const float tile_w = (w - 40 - 12) / 2;
            const auto tile = [&](float tx, std::wstring_view label, const std::wstring& value) {
                Round(tx, y + 54, tile_w, 68, 12, Palette::Canvas, 0.7f);
                Type(label, TypeRole::Label, tx + 12, y + 64, tile_w - 24, Palette::TextSecondary);
                Type(value, TypeRole::HeadlineSmall, tx + 12, y + 84, tile_w - 24, Palette::Text);
            };
            // The version once it has been read; until then (or when unreadable) the file count.
            const auto& firmware = status.versions.firmware;
            tile(x + 20, Tr(firmware.empty() ? T::SettingsFirmwareFiles : T::SettingsFirmwareVersion),
                 firmware.empty() ? std::to_wstring(status.firmware_files) : std::wstring{winrt::to_hstring(firmware)});
            const int generation = status.keys_ready ? status.versions.key_generation : -1;
            tile(x + 20 + tile_w + 12, Tr(T::SettingsKeyGeneration), generation < 0 ? std::wstring{L"—"} : std::to_wstring(generation));
            const auto line = [&](float ly, std::wstring_view label, const std::wstring& value) {
                Type(label, TypeRole::Label, x + 20, ly, w - 120, Palette::TextSecondary);
                Type(value, TypeRole::Label, x + w - 120, ly, 100, Palette::Text, 1, TextAlign::Trailing);
            };
            line(y + 140, Tr(T::SettingsGameFolders), std::to_wstring(status.sources.size()));
            line(y + 168, Tr(T::SettingsLibraryGames),
                 view.loading ? std::wstring{L"…"} : std::to_wstring(view.scan.entries.size()));
        }
        // The category's page.
        static constexpr std::array<T, HomeRowCount> pages{T::SystemTitle, T::ControllersAndKeyboard, T::FileManager};
        static constexpr std::array<T, HomeRowCount> about{T::SystemSubtitle, T::ControllerSubtitle, T::FilesSubtitle};
        const float px = SettingsPaneX, pw = SettingsPaneWidth;
        Type(Tr(pages[category]), TypeRole::HeadlineLarge, px, 122, pw, Palette::Text);
        Type(Tr(about[category]), TypeRole::Body, px, 176, pw, Palette::TextSecondary);
        const auto rows = SettingsRows(view, category);
        if (rows.empty()) return;
        const std::size_t selected = !in_pane ? SIZE_MAX :
            category == HomeSystem ? view.system.selected :
            category == HomeFiles ? configuration.selected : view.controller_row;
        const std::size_t visible = std::min(rows.size(), SettingsVisibleRows);
        const std::size_t first = selected != SIZE_MAX && selected >= visible ? selected - visible + 1 : 0;
        const float card_h = visible * SettingsRowStride - (SettingsRowStride - SettingsRowHeight) + 24;
        Round(px, SettingsRowsTop - 12, pw, card_h, 18, Palette::Surface, 0.82f);
        Outline(px + 0.5f, SettingsRowsTop - 11.5f, pw - 1, card_h - 1, 18, 0xffffff, 0.06f);
        for (std::size_t i = first; i < first + visible; ++i) {
            const float y = SettingsRowsTop + (i - first) * SettingsRowStride;
            DrawSettingsRow(rows[i], px + 12, y, pw - 24, SettingsRowHeight, i == selected, in_pane ? 1.0f : 0.75f);
            AddHit(px + 12, y, pw - 24, SettingsRowHeight, LibraryHit::SettingRow, i);
        }
        ScrollIndicator(px + pw + 8, SettingsRowsTop, card_h - 24, first, visible, rows.size());
    }
    void DrawSettingsRow(const SettingsRow& row, float x, float y, float w, float h, bool focused, float alpha) {
        using Kind = SettingsRow::Kind;
        if (focused) {
            FocusGlow(x, y, w, h, 12, 1);
            Round(x, y, w, h, 12, 0x333537);
        } else {
            Round(x, y, w, h, 12, Palette::Canvas, 0.55f);
        }
        const float middle = y + h / 2;
        float right = x + w - 16;
        // The control at the right, drawn first: the texts take the room it leaves.
        switch (row.kind) {
        case Kind::Toggle:
            right -= 52;
            Switch(right, middle - 14, row.on, focused);
            break;
        case Kind::Segments: {
            constexpr float pad = 14, inset = 3, height = 32;
            float width = 2 * inset;
            for (const auto& label : row.segments) width += MeasureType(label, TypeRole::LabelLarge) + 2 * pad;
            right -= width;
            Round(right, middle - height / 2, width, height, 9, Palette::Graphite, 0.9f);
            float cursor = right + inset;
            for (std::size_t i = 0; i < row.segments.size(); ++i) {
                const float segment = MeasureType(row.segments[i], TypeRole::LabelLarge) + 2 * pad;
                const bool on = i == row.on_segment;
                if (on) Round(cursor, middle - height / 2 + inset, segment, height - 2 * inset, 7, Palette::Accent);
                Type(row.segments[i], TypeRole::LabelLarge, cursor, middle - 10, segment,
                     on ? Palette::OnAccent : Palette::TextSecondary, alpha, TextAlign::Center);
                cursor += segment;
            }
            break;
        }
        case Kind::Choice: {
            // A capsule with the value; the arrows say left and right change it.
            const float text = std::min(MeasureType(row.value, TypeRole::LabelLarge) + 4, w * 0.45f);
            const float width = text + 2 * 30;
            right -= width;
            Round(right, middle - 16, width, 32, 9, Palette::Graphite, 0.9f);
            Type(row.value, TypeRole::LabelLarge, right + 30, middle - 10, text, Palette::Text, alpha);
            const unsigned arrows = focused ? Palette::Text : Palette::TextSecondary;
            Chevron(right + 16, middle, arrows, true);
            Chevron(right + width - 14, middle, arrows);
            break;
        }
        case Kind::Open:
        case Kind::Value: {
            if (row.kind == Kind::Open) {
                Chevron(right - 4, middle, focused ? Palette::Text : Palette::TextSecondary);
                right -= 22;
            }
            if (!row.value.empty()) {
                const float text = std::min(MeasureType(row.value, TypeRole::LabelLarge) + 4, w * 0.5f);
                right -= text;
                Type(row.value, TypeRole::LabelLarge, right, middle - 10, text,
                     row.warning ? Palette::Warning : focused ? Palette::Text : Palette::TextHint, alpha);
            }
            break;
        }
        }
        const float text_w = right - (x + 18) - 20;
        if (row.hint.empty()) {
            Type(row.title, TypeRole::LabelLarge, x + 18, middle - 10, text_w, Palette::Text, alpha);
        } else {
            Type(row.title, TypeRole::LabelLarge, x + 18, y + 7, text_w, Palette::Text, alpha);
            Type(row.hint, TypeRole::Label, x + 18, y + 28, text_w, focused ? Palette::TextHint : Palette::TextSecondary,
                 alpha);
        }
    }
    void DrawNameEditor(const SystemPage& page) {
        using L = SystemPageLayout;
        using K = TextEntryPanelLayout;
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.86f));
        target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
        Round(L::EditorX, L::EditorY, K::Width, K::Height, 22, Palette::Surface);
        DrawTextEntryPanel(*this, {*page.name_editor, Tr(T::UserName), Tr(T::NameEditorHint), page.editor_notice},
                           L::EditorX, L::EditorY);
        const float left = L::EditorX + K::Left;
        const float footer = L::EditorY + K::Footer;
        if (prompt_family == PromptFamily::Keyboard) {
            Text(Tr(T::NameEditorKeyboardHelp), left, footer + 6, 15, Palette::TextSecondary, K::BoxWidth, 24);
        } else {
            ControlPrompt(Navigation::Play, Tr(T::TypeKey), left, footer);
            ControlPrompt(Navigation::AddFolder, Tr(T::DeleteKey), left + 190, footer);
            ControlPrompt(Navigation::Space, Tr(T::SpaceKey), left + 380, footer);
            ControlPrompt(Navigation::Options, Tr(T::DoneKey), left + 570, footer);
            ControlPrompt(Navigation::Back, Tr(T::Cancel), left + 760, footer);
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
        const float width = MeasureType(label, TypeRole::LabelLarge);
        Type(label, TypeRole::LabelLarge, x, y + 6, width + 4, Palette::TextSecondary);
        return width;
    }
    /// The badge of the button that does `action`, and its label, on a 32 px line; returns the
    /// width it took with the gap to the next prompt.
    float ControlPrompt(Navigation action, std::wstring_view label, float x, float y) {
        if (action == Navigation::Explore) {
            // The d-pad (the arrow keys on a keyboard): a cross in the badge.
            Round(x, y + 5, 22, 22, 11, Palette::Graphite);
            Outline(x + 0.5f, y + 5.5f, 21, 21, 10.5f, 0xffffff, 0.15f);
            Round(x + 9, y + 10, 4, 12, 1, Palette::Text);
            Round(x + 5, y + 14, 12, 4, 1, Palette::Text);
            return 30 + PromptLabel(label, x + 30, y) + 28;
        }
        if (action == Navigation::Tabs) {
            const bool keyboard = prompt_family == PromptFamily::Keyboard;
            const bool nintendo = prompt_family == PromptFamily::Nintendo;
            const std::wstring_view previous = keyboard ? L"Q" : nintendo ? L"L" : L"LB";
            const std::wstring_view next = keyboard ? L"E" : nintendo ? L"R" : L"RB";
            const float first = BadgeWidth(previous), second = BadgeWidth(next);
            Badge(previous, x + first / 2, y + 16, Palette::Text);
            Badge(next, x + first + 6 + second / 2, y + 16, Palette::Text);
            const float badges = first + 6 + second + 8;
            return badges + PromptLabel(label, x + badges, y) + 28;
        }
        const auto key = PromptKey(action);
        return Prompt(key.letters, key.color, label, x, y);
    }
    /// The width ControlPrompt takes for a one-badge prompt, to lay it out before drawing it.
    float PromptWidth(Navigation action, std::wstring_view label) {
        return BadgeWidth(PromptKey(action).letters) + 8 + MeasureType(label, TypeRole::LabelLarge) + 28;
    }
    /// Eases `tween` toward the row `first`; a jump of more than a page (a new tab) lands at once.
    static float FollowRow(Tween& tween, std::size_t first, float page, Tween::Clock::time_point now) {
        const float target = static_cast<float>(first);
        if (std::abs(target - tween.Value(now)) > page) tween.Snap(target);
        else tween.Set(target, now);
        return tween.Value(now);
    }
    /// The game's options, and the option page open over them.
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
        const float offset = expandable && !modal ? management_collapse.Value(now) : 0;
        const bool keyboard = prompt_family == PromptFamily::Keyboard;
        const bool nintendo = prompt_family == PromptFamily::Nintendo;
        const std::wstring_view previous_tab = keyboard ? L"PgUp" : nintendo ? L"ZL" : L"LT";
        const std::wstring_view next_tab = keyboard ? L"PgDn" : nintendo ? L"ZR" : L"RT";
        constexpr float footer = GameOptionsLayout::Footer;
        // B, the way out, always ends the bar at the right margin.
        const auto back_prompt = [&](std::wstring_view label) {
            const float width = BadgeWidth(PromptKey(Navigation::Back).letters) + 8 +
                                MeasureType(label, TypeRole::LabelLarge);
            ControlPrompt(Navigation::Back, label, 1224 - width, footer);
        };
        D2D1_MATRIX_3X2_F base_transform;
        target->GetTransform(&base_transform);
        const auto begin_body = [&] {
            if (modal) return;
            target->PushAxisAlignedClip(D2D1::RectF(0, GameOptionsLayout::BodyTop - offset, 1280, body_bottom), D2D1_ANTIALIAS_MODE_ALIASED);
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
            Badge(previous_tab, left + first / 2, footer + 16, Palette::Text);
            return first + 6 + Prompt(next_tab, Palette::Text, label, left + first + 6, footer);
        };

        Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.86f);
        if (view.page.page == GameOption::Cheats && view.cheats) {
            using L = Cheats::CheatsLayout;
            constexpr float x = (1280 - L::Width) / 2, y = (720 - L::Height) / 2;
            const Cheats::CheatsView manager{*view.cheats, view.cheats_page, game.name, view.notice, false,
                view.page.focus == GameOptionsFocus::Content, view.page.focus == GameOptionsFocus::ManagerTabs,
                previous_tab, next_tab, FollowRow(list_scroll, view.cheats_page.first, L::VisibleRows, now)};
            begin_body();
            Cheats::DrawCheatsPanel(*this, manager, x, y);
            end_body();
            Round(0, body_bottom, 1280, 720 - body_bottom, 0, Palette::Canvas);
            Round(40, GameOptionsLayout::FooterLine, 1200, 1, 0, Palette::Graphite);
            Cheats::DrawCheatsStatus(*this, manager, x, y);
            if (view.page.focus == GameOptionsFocus::Content) {
                float next = 56;
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
            constexpr float x = (1280 - L::Width) / 2, y = (720 - L::Height) / 2;
            const Mods::ModsView manager{*view.mods, *view.mods_page, game.name, view.notice,
                view.page.focus == GameOptionsFocus::Content, view.page.focus == GameOptionsFocus::ManagerTabs,
                previous_tab, next_tab, PromptKey(Navigation::Play).letters, PromptKey(Navigation::Play).color,
                FollowRow(list_scroll, view.mods_page->first, L::VisibleRows, now),
                FollowRow(grid_scroll, view.mods_page->card_first / L::Columns, 2, now), body_bottom + offset};
            begin_body();
            Mods::DrawModsPanel(*this, manager, x, y);
            end_body();
            if (!modal) {
                Round(0, body_bottom, 1280, 720 - body_bottom, 0, Palette::Canvas);
                Round(40, GameOptionsLayout::FooterLine, 1200, 1, 0, Palette::Graphite);
                Mods::DrawModsStatus(*this, manager, x, y);
            }
            float next = 56;
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
            const auto in_place = [&](GameOption option, std::wstring_view body, std::wstring_view action) {
                Card(56, 214, 1168, 264, 24);
                Icon(GameOptionIcon(option), 80, 240, Palette::Text);
                Type(Tr(GameOptionTitle(option)), TypeRole::HeadlineMedium, 132, 234, 1056, Palette::Text);
                Type(body, TypeRole::BodyLarge, 80, 298, 1120, Palette::TextHint, 1, TextAlign::Leading, 3);
                if (!action.empty()) Prompt(PromptKey(Navigation::Play).letters, Palette::Text, action, 80, 404);
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
                Type(view.notice.empty() ? Tr(T::CheckingGame) : view.notice, TypeRole::BodyLarge, 56, 230, 1168, Palette::TextHint,
                     1, TextAlign::Leading, 3);
            }
        }
        if (view.mods_page && view.page.page == GameOption::Mods &&
            view.mods_page->screen != Mods::ModsScreen::List) return; // A modal covers the game content.
        // The global header stays fixed; the game's line folds up behind it.
        using G = GameOptionsLayout;
        Round(0, G::GlobalHeaderBottom, 1280, G::BodyTop - G::GlobalHeaderBottom - offset, 0, Palette::Canvas);
        if (offset < G::HeroCollapse) {
            target->PushAxisAlignedClip(D2D1::RectF(0, G::GlobalHeaderBottom, 1280, G::HeroClipBottom - offset),
                                        D2D1_ANTIALIAS_MODE_ALIASED);
            const float top = G::HeroTop - offset;
            Cover(game, 56, top, G::HeroHeight);
            const float title_left = 56 + G::HeroHeight + 16;
            const float title_width = std::min(MeasureType(game.name, TypeRole::HeadlineMedium), 640.0f);
            Type(game.name, TypeRole::HeadlineMedium, title_left, top + 6, title_width + 4, Palette::Text);
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
            const float about_left = title_left + title_width + 20;
            Type(about, TypeRole::Label, about_left, top + 17, 1224 - about_left, Palette::TextHint);
            target->PopAxisAlignedClip();
        }
        const auto options = GameOptions();
        const float sections = G::SectionsTop - offset;
        Round(56, sections, 1168, G::SectionsHeight, G::SectionsHeight / 2, Palette::Surface);
        Badge(keyboard ? L"Q" : nintendo ? L"L" : L"LB", 80, sections + G::SectionsHeight / 2, Palette::Text);
        Badge(keyboard ? L"E" : nintendo ? L"R" : L"RB", 1200, sections + G::SectionsHeight / 2, Palette::Text);
        // The bar spans the bumpers' gap, shared by however many options there are.
        const float tab_width = 1080.0f / static_cast<float>(options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            const float left = 100 + static_cast<float>(i) * tab_width;
            const bool active = i == view.page.selected;
            if (active) Round(left, sections + 4, tab_width - 4, 36, 18,
                (view.page.focus == GameOptionsFocus::Sections) ? Palette::Accent : Palette::RowActive);
            Type(Tr(GameOptionTitle(options[i])), TypeRole::LabelLarge, left + 6, sections + 12,
                tab_width - 16, active && (view.page.focus == GameOptionsFocus::Sections) ? Palette::OnAccent :
                active ? Palette::Text : Palette::TextHint, 1, TextAlign::Center);
            AddHit(left, sections + 4, tab_width - 4, 36, LibraryHit::GameOptionSelect, i);
        }
        // A confirmation names the key that confirms on this controller ("{0}").
        if (!view.notice.empty() && view.page.page != GameOption::Mods && view.page.page != GameOption::Cheats)
            Type(I18n::Format(view.notice, {PromptKey(Navigation::Play).letters}), TypeRole::Label, 56, G::Status,
                 1168, Palette::Warning);
        // The panes that act in place have no prompts of their own; on the manager's tabs (a
        // pointer put the focus there), the bar's prompts speak over the pane's.
        if (view.page.focus != GameOptionsFocus::Content || !view.page.page) {
            Round(0, G::FooterLine + 1, 1280, 720 - G::FooterLine - 1, 0, Palette::Canvas);
            float next = 56;
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
    void Cover(const LibraryEntry& entry, float x, float y, float size, float alpha = 1) {
        Round(x, y, size, size, 12, Palette::Graphite, alpha);
        if (auto* bitmap = CoverBitmap(entry)) {
            // One unit mask for every size: the layer's transform places and scales it, so a
            // cover growing into focus allocates nothing.
            if (!cover_mask) {
                ComPtr<ID2D1Factory> factory;
                target->GetFactory(&factory);
                winrt::check_hresult(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(0, 0, 1, 1), 1 / 12.0f, 1 / 12.0f), &cover_mask));
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
            Outline(x + 0.5f, y + 0.5f, size - 1, size - 1, 11.5f, 0xffffff, 0.08f * alpha);
            const auto extension = entry.relative_path.extension().wstring();
            if (extension.size() > 1)
                Type(Upper(std::wstring_view{extension}.substr(1)), TypeRole::Caps, x, y + size / 2 - 8, size,
                     Palette::TextSecondary, alpha, TextAlign::Center);
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
        constexpr float r = 0.2126f, g = 0.7152f, b = 0.0722f;
        const float k = 1 - amount;
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
        target->Clear(D2D1::ColorF(0, 0, 0, 0));
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
        constexpr float Tau = 6.2831853f;
        const float s = std::chrono::duration<float>(now - ambient_epoch).count();
        const auto rect = [](float scale, float dx, float dy) {
            const float w = 640 * scale, h = 360 * scale;
            return D2D1::RectF(640 - w + dx, 360 - h + dy, 640 + w + dx, 360 + h + dy);
        };
        const auto art = rect(1.2f + 0.08f * std::sin(Tau * s / 17), 60 * std::sin(Tau * s / 19),
                              36 * std::sin(Tau * s / 13 + 1.3f));
        const auto glow = rect(1.7f + 0.15f * std::sin(Tau * s / 23 + 2), -100 * std::sin(Tau * s / 17 + 0.7f),
                               60 * std::sin(Tau * s / 11));
        // The flowing copy also swells and fades, as light passing over the art.
        const float shine = 0.3f + 0.2f * std::sin(Tau * s / 9 + 0.4f);
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
            if (scrim) target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), scrim.Get());
    }
    void MakeScrims() {
        const auto canvas = [](float alpha) { return D2D1::ColorF(Palette::Canvas, alpha); };
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
        scrims[0] = linear({0, 0}, {820, 0}, {{0, canvas(0.85f)}, {0.5f, canvas(0.55f)}, {1, canvas(0)}});
        scrims[1] = linear({0, 0}, {0, 160}, {{0, canvas(0.7f)}, {1, canvas(0)}});
        scrims[2] = linear({0, 380}, {0, 720}, {{0, canvas(0)}, {0.5f, canvas(0.7f)}, {1, canvas(0.92f)}});
        ComPtr<ID2D1RadialGradientBrush> vignette;
        winrt::check_hresult(target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties({640, 320}, {0, 0}, 860, 520),
            stops({{0.6f, canvas(0)}, {1, canvas(0.55f)}}).Get(), &vignette));
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
        const auto source = D2D1::RectF((size.width - crop_w) / 2, (size.height - crop_h) / 2,
                                        (size.width + crop_w) / 2, (size.height + crop_h) / 2);
        auto& mask = picture_masks[(static_cast<unsigned>(w) << 16) | static_cast<unsigned>(h)];
        if (!mask) {
            ComPtr<ID2D1Factory> factory;
            target->GetFactory(&factory);
            if (FAILED(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), 12, 12), &mask)))
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
    static constexpr size_t MaxCovers = 48;
    static constexpr size_t MaxBackdrops = 8;
    static constexpr unsigned BackdropWidth = 320, BackdropHeight = 180;
    static constexpr float BackdropOpacity = 0.58f;
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
