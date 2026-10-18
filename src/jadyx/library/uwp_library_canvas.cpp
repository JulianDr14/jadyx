// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/uwp_library_canvas.h"
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
#include "jadyx/cheats/cheats_view.h"
#include "jadyx/library/game_library.h"
#include "jadyx/library/play_history.h"
#include "jadyx/library/game_options_page.h"
#include "jadyx/library/library_layout.h"
#include "jadyx/content/content_view.h"
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
        last_played = nullptr;
        if (const auto last = LibraryLayout::LastPlayed(scan.entries); !view.loading && last < scan.entries.size())
            last_played = &scan.entries[last];
        // The focused game's backdrop is rendered off-screen, before this frame starts.
        UpdateBackdrop(game, now);
        hits.clear();
        target->BeginDraw();
        target->Clear(D2D1::ColorF(Palette::Canvas));
        DrawBackdrop(now);
        if (view.tab == LibraryTab::Settings) {
            rows_shown = false;
            // Settings dims the art further: its own page, not a dialog over the library.
            Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.55f);
            if (view.system.open) DrawSystemPage(view.system, true);
            else DrawConfiguration(configuration, true);
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
        DrawHeader(view, now);
        DrawFooter(view);
        if (view.controller_panel) DrawControllerPanel(view);
        if (panel.keyboard.open) DrawKeyboard(panel.keyboard);
        if (configuration.open && view.system.open && view.system.name_editor) DrawNameEditor(view.system);
        if (view.game_options.game || view.game_options.content) DrawGameOptions(view.game_options);
        if (view.missing_content) Content::DrawMissingContent(*this);
        winrt::check_hresult(target->EndDraw());
        winrt::check_hresult(swapchain->Present(1, 0));
        animating = scroll.Running(now) || focus_in.Running(now) || focus_out.Running(now) ||
                    hero.Running(now) || backdrop_fade.Running(now) || tab_slide.Running(now) ||
                    filter_slide.Running(now);
    }
    void InvalidateCovers() { covers.clear(); pictures.clear(); backdrops.clear(); backdrop_now.clear(); backdrop_before.clear(); }
    bool Animating() const { return animating; }
    LibraryTarget HitAt(float x, float y) const {
        // Drawn last is on top: the header over the rows scrolling under it.
        for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit)
            if (x >= hit->rect.left && x < hit->rect.right && y >= hit->rect.top && y < hit->rect.bottom)
                return hit->target;
        return {};
    }
private:
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
        Badge(previous, left - 10 - BadgeWidth(previous) / 2, middle, Palette::TextSecondary);
        Badge(next, left + width + 10 + BadgeWidth(next) / 2, middle, Palette::TextSecondary);
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
            sink->BeginFigure(D2D1::Point2F(9, 1.4f), D2D1_FIGURE_BEGIN_FILLED);
            bezier(12, 0.6f, 20, 0.6f, 23, 1.4f);                // The top edge.
            bezier(27.6f, 2, 30.2f, 4.4f, 30.9f, 9);             // The right shoulder.
            sink->AddLine(D2D1::Point2F(31.6f, 15.6f));
            bezier(32.1f, 19.6f, 29.8f, 21.8f, 27.4f, 21.2f);    // The right grip.
            bezier(25.6f, 20.8f, 24.4f, 19.2f, 23, 17.4f);
            bezier(22.2f, 16.4f, 21.4f, 16, 20.2f, 16);
            sink->AddLine(D2D1::Point2F(11.8f, 16));
            bezier(10.6f, 16, 9.8f, 16.4f, 9, 17.4f);
            bezier(7.6f, 19.2f, 6.4f, 20.8f, 4.6f, 21.2f);       // The left grip.
            bezier(2.2f, 21.8f, -0.1f, 19.6f, 0.4f, 15.6f);
            sink->AddLine(D2D1::Point2F(1.1f, 9));
            bezier(1.8f, 4.4f, 4.4f, 2, 9, 1.4f);                // The left shoulder.
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
        // The left stick and the d-pad, the face buttons in their diamond, the guide button.
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(8.6f, 7.4f), 2.3f, 2.3f), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(11.2f, 12.2f), D2D1::Point2F(14.4f, 12.2f), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(12.8f, 10.6f), D2D1::Point2F(12.8f, 13.8f), brush.Get(), 1.4f);
        constexpr D2D1_POINT_2F face[] = {{23.4f, 5}, {23.4f, 9.6f}, {21.1f, 7.3f}, {25.7f, 7.3f}};
        for (const auto button : face) target->FillEllipse(D2D1::Ellipse(button, 1.15f, 1.15f), brush.Get());
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(16, 5.2f), 1.5f, 1.5f), brush.Get());
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
        if (&entry == last_played) {
            const auto badge = Upper(Tr(T::InProgress));
            const float width = MeasureType(badge, TypeRole::Caps) + 16;
            Round(left + 8, top + 8, width, 20, 10, Palette::Accent, alpha);
            Type(badge, TypeRole::Caps, left + 8, top + 10, width, Palette::OnAccent, alpha, TextAlign::Center);
        }
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
            x += ControlPrompt(Navigation::Play, Tr(view.system.open ? T::Change : T::Open), x, y);
            ControlPrompt(Navigation::Back, Tr(configuration.busy ? T::Cancel : T::Back), x, y);
        } else {
            const bool shelf = view.tab == LibraryTab::Library;
            const bool import = shelf && view.shelf && view.shelf_focus >= view.shelf->size();
            if (!(shelf && view.filter_focus)) {
                if (import) x += ControlPrompt(Navigation::Play, Tr(T::Import), x, y);
                else if (game) x += ControlPrompt(Navigation::Play, Tr(T::Play), x, y);
                if (game && !import) {
                    x += ControlPrompt(Navigation::Options, Tr(T::Options), x, y);
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
        if (panel.expanded) {
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
    /// The settings home or its file manager; as a `page` (the Settings tab) it leaves the
    /// background and the prompts to the screen.
    void DrawConfiguration(const ConfigurationPanel& panel, bool page = false) {
        if (!page) Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.86f);
        const float panel_alpha = page ? 0.82f : 1.0f;
        const auto layout = GetConfigurationLayout(panel);
        if (panel.files) {
            Round(90, 80, 1100, page ? 560.0f : 590.0f, 24, Palette::Surface, panel_alpha);
            Round(130, 108, 48, 48, 12, Palette::RowActive);
            Icon(ConfigurationIcon::Folder, 138, 116, Palette::Accent);
            Text(Tr(T::FileManager), 194, 108, 32, Palette::Text, 850, 50, true);
            Text(std::wstring{Tr(panel.status.keys_ready ? T::KeysReady : T::KeysMissing)} + L"  ·  " +
                 I18n::Format(T::FirmwareFiles, {std::to_wstring(panel.status.firmware_files)}),
                 130, 168, 18, Palette::Accent, 950);
            Text(Tr(T::FilesSubtitle), 130, 201, 16, Palette::TextSecondary, 950);
        } else {
            Round(140, 100, 1000, 540, 24, Palette::Surface, panel_alpha);
            PageHeader(180, 131, ConfigurationIcon::Settings, Tr(T::SettingsTitle), Tr(T::SettingsSubtitle));
        }
        const auto count = ConfigurationRowCount(panel);
        const auto first = ConfigurationFirstRow(panel);
        for (size_t i = first; i < std::min(first + 5, count); ++i) {
            const float y = layout.top + static_cast<float>(i - first) * layout.stride;
            const bool active = i == panel.selected;
            ConfigurationIcon icon;
            std::wstring title, hint;
            if (!panel.files) {
                icon = i == HomeFiles ? ConfigurationIcon::Folder :
                       i == HomeControllers ? ConfigurationIcon::Controller : ConfigurationIcon::Language;
                title = Tr(i == HomeFiles ? T::FileManager : i == HomeControllers ? T::ControllersAndKeyboard : T::System);
                hint = Tr(i == HomeFiles ? T::FileManagerHint : i == HomeControllers ? T::ControllersHint : T::SystemHint);
            } else {
                icon = i == 0 ? ConfigurationIcon::Folder : i == 1 ? ConfigurationIcon::Key :
                       i == 2 ? ConfigurationIcon::Firmware : i < 5 ? ConfigurationIcon::Folder : ConfigurationIcon::Remove;
                if (i == 0) title = Tr(T::AddGamesFolder);
                else if (i == 1) title = Tr(T::ImportKeys);
                else if (i == 2) title = Tr(T::ImportFirmware);
                else if (i == 3) title = Tr(T::ContentUpdates);
                else if (i == 4) title = Tr(T::ContentDlc);
                else title = I18n::Format(T::RemoveFolder, {panel.status.sources[i - 5].name});
            }
            RowFrame(layout.x, y, layout.width, layout.row_height, active, icon);
            Text(title, layout.x + 72, y + (panel.files ? 10.0f : 12.0f), panel.files ? 20.0f : 22.0f,
                 Palette::Text, layout.width - 135, 32, !panel.files);
            if (!hint.empty()) Text(hint, layout.x + 72, y + 44, 16, Palette::TextHint, layout.width - 135, 26);
            Chevron(layout.x + layout.width - 28, y + layout.row_height / 2, active ? Palette::Accent : Palette::TextSecondary);
        }
        ScrollIndicator(layout.x + layout.width + 14, layout.top,
                        5 * layout.stride - (layout.stride - layout.row_height), first,
                        5, count);
        if (!panel.files) {
            Text(Tr(T::EmulatorData), 180, 511, 12, Palette::TextSecondary, 190, 26, true);
            Text(Tr(panel.status.keys_ready ? T::KeysReady : T::KeysMissing), 380, 506, 16,
                 panel.status.keys_ready ? Palette::Accent : Palette::Warning, 260);
            Text(I18n::Format(T::FirmwareFiles, {std::to_wstring(panel.status.firmware_files)}),
                 680, 506, 16, Palette::TextSecondary, 350);
        }
        if (panel.busy) Text(I18n::Format(T::Importing, {std::to_wstring(panel.completed), std::to_wstring(panel.total)}),
                             130, 565, 18, Palette::Accent, 990);
        else Text(panel.notice, layout.x + 4, panel.files ? 561.0f : 538.0f, 16, Palette::Warning,
                  layout.width - 20, panel.files ? 40.0f : 28.0f, false, true);
        if (page) return;
        ControlPrompt(Navigation::Play, Tr(T::Open), layout.x + 4, layout.footer);
        ControlPrompt(Navigation::Back, Tr(panel.busy ? T::Cancel : T::Back),
                      layout.x + layout.width - 230, layout.footer);
    }
    void DrawSystemPage(const SystemPage& page, bool full_page = false) {
        using L = SystemPageLayout;
        if (!full_page) Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.86f);
        // As the Settings tab's page it ends above the screen's prompts, which replace its own.
        Round(L::PanelX, L::PanelY, L::PanelWidth, full_page ? 568.0f : L::PanelHeight, 24, Palette::Surface,
              full_page ? 0.82f : 1.0f);
        PageHeader(L::RowX, 99, ConfigurationIcon::Language, Tr(T::SystemTitle), Tr(T::SystemSubtitle));
        static constexpr std::array<ConfigurationIcon, SystemPage::RowCount> icons{
            ConfigurationIcon::User, ConfigurationIcon::Language, ConfigurationIcon::Controller,
            ConfigurationIcon::Region, ConfigurationIcon::Mods, ConfigurationIcon::Settings, ConfigurationIcon::Archive,
            ConfigurationIcon::Clock, ConfigurationIcon::Cache};
        for (size_t i = 0; i < SystemPage::RowCount; ++i) {
            const auto row = static_cast<SystemPage::Row>(i);
            const float y = L::RowTop + static_cast<float>(i) * L::RowStride;
            const bool active = i == page.selected;
            RowFrame(L::RowX, y, L::RowWidth, L::RowHeight, active, icons[i]);
            Text(Tr(SystemRowTitle(row)), L::RowX + 72, y + 4, 20, Palette::Text, 420, 30, true);
            Text(Tr(SystemRowHint(row)), L::RowX + 72, y + 26, 15, Palette::TextHint, 440, 22);
            const unsigned accent = active ? Palette::Accent : Palette::TextSecondary;
            const float right = L::RowX + L::RowWidth - 28;
            const float middle = y + L::RowHeight / 2;
            const bool choice = row == SystemPage::InterfaceLanguage || row == SystemPage::GameLanguage ||
                                row == SystemPage::Region || row == SystemPage::AdultMods || row == SystemPage::PerformanceStats ||
                                row == SystemPage::PulseSessions || row == SystemPage::PlayTime;
            if (choice) {
                // "< value >": arrows say left/right changes it, as in the controller panel.
                Chevron(L::RowX + 486, middle, accent, true);
                Chevron(right, middle, accent);
            } else {
                Chevron(right, middle, accent);
            }
            const auto value = SystemRowValue(page, row);
            if (!value.empty())
                Text(value, L::RowX + 500, y + (L::RowHeight - 30) / 2, 19, active ? Palette::Text : Palette::TextHint,
                     right - 22 - (L::RowX + 500), 30, active, false, false, true);
        }
        if (!page.notice.empty()) Text(page.notice, L::RowX + 4, L::Notice, 16, Palette::Accent, L::RowWidth - 20, 28);
        if (full_page) return;
        ControlPrompt(Navigation::Play, Tr(T::Change), L::RowX + 4, L::Footer);
        ControlPrompt(Navigation::Back, Tr(T::Back), L::RowX + L::RowWidth - 230, L::Footer);
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
    /// The game's options, and the option page open over them.
    void DrawGameOptions(const GameOptionsView& view) {
        if (view.content) { Content::DrawContentPanel(*this, *view.content, view.game ? view.game->name : Tr(T::ContentOverview)); return; }
        const LibraryEntry& game = *view.game;
        Round(0, 0, 1280, 720, 0, Palette::Canvas, 0.86f);
        if (view.page.page == GameOption::Cheats && view.cheats) {
            using L = Cheats::CheatsLayout;
            constexpr float x = (1280 - L::Width) / 2, y = 30;
            Cheats::DrawCheatsPanel(*this, {*view.cheats, view.cheats_page, game.name, view.notice, false}, x, y);
            const float footer = y + L::Footer;
            ControlPrompt(Navigation::Play, Tr(T::ToggleCheat), x + L::Margin, footer);
            ControlPrompt(Navigation::AddFolder, Tr(T::AllCheats), x + L::Margin + 170, footer);
            float next = x + L::Margin + 330;
            if (Cheats::CheatTabCount(view.cheats->Catalog()) > 1) {
                ControlPrompt(Navigation::Tabs, Tr(T::Category), next, footer);
                next += 250;
            }
            ControlPrompt(Navigation::Refresh, Tr(T::Refresh), next, footer);
            ControlPrompt(Navigation::Back, Tr(T::Back), x + L::Width - L::Margin - 150, footer);
            return;
        }
        if (view.page.page == GameOption::Mods && view.mods && view.mods_page) {
            using L = Mods::ModsLayout;
            using Button = Mods::ModsPrompt::Button;
            constexpr float x = (1280 - L::Width) / 2, y = 30;
            Mods::DrawModsPanel(*this, {*view.mods, *view.mods_page, game.name, view.notice}, x, y);
            const float footer = y + L::Footer;
            float next = x + L::Margin;
            for (const auto& prompt : Mods::ModsPrompts(*view.mods, *view.mods_page)) {
                const auto label = Tr(prompt.label);
                // B, the way out, always sits at the right end.
                if (prompt.button == Button::B) {
                    ControlPrompt(Navigation::Back, label, x + L::Width - L::Margin - 170, footer);
                    continue;
                }
                const Navigation action = prompt.button == Button::A      ? Navigation::Play
                                          : prompt.button == Button::X    ? Navigation::AddFolder
                                          : prompt.button == Button::Y    ? Navigation::Refresh
                                          : prompt.button == Button::Menu ? Navigation::Options
                                                                          : Navigation::Tabs;
                next += ControlPrompt(action, label, next, footer);
            }
            return;
        }
        using L = GameOptionsLayout;
        Round(L::PanelX, L::PanelY, L::PanelWidth, L::PanelHeight, 24, Palette::Panel);
        PageHeader(L::RowX, 99, UiIcon::Game, Tr(T::GameOptionsTitle),
                   I18n::Format(T::GameOptionsSubtitle, {game.name}));
        Cover(game, L::RowX + L::RowWidth - 72, 92, 72);
        const auto options = GameOptions();
        for (std::size_t i = 0; i < options.size(); ++i) {
            const float y = L::RowTop + static_cast<float>(i) * L::RowStride;
            const bool active = i == view.page.selected;
            RowFrame(L::RowX, y, L::RowWidth, L::RowHeight, active, GameOptionIcon(options[i]));
            Text(Tr(GameOptionTitle(options[i])), L::RowX + 72, y + 6, 22, Palette::Text, L::RowWidth - 135, 32, true);
            std::wstring hint{Tr(GameOptionHint(options[i]))};
            if (options[i] == GameOption::Pulse && !game.pulse_saved.empty())
                hint = I18n::Format(game.pulse_current ? T::PulseSavedOn : T::PulseSavedOther, {game.pulse_saved});
            Text(hint, L::RowX + 72, y + 36, 16, Palette::TextHint, L::RowWidth - 135, 26);
            Chevron(L::RowX + L::RowWidth - 28, y + L::RowHeight / 2, active ? Palette::Accent : Palette::TextSecondary);
        }
        if (!view.notice.empty()) Text(view.notice, L::RowX + 4, L::Footer - 44, 16, Palette::Warning, L::RowWidth - 20, 28);
        ControlPrompt(Navigation::Play, Tr(T::Open), L::RowX + 4, L::Footer);
        ControlPrompt(Navigation::Back, Tr(T::Back), L::RowX + L::RowWidth - 230, L::Footer);
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
        const auto layer = [&](const std::string& key, float opacity) {
            if (key.empty() || opacity <= 0.002f) return;
            if (auto* bitmap = FindBackdrop(key))
                target->DrawBitmap(bitmap, D2D1::RectF(0, 0, 1280, 720), opacity, D2D1_INTERPOLATION_MODE_LINEAR);
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
    /// Two screens of the grid and a mod's gallery: 640 px wide at most, ~0.6 MiB each.
    static constexpr size_t MaxPictures = 48;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID2D1DeviceContext> target;
    ComPtr<IWICImagingFactory> wic;
    std::array<ComPtr<ID2D1PathGeometry>, 5> controller_paths;
    ComPtr<ID2D1PathGeometry> pad_outline; ///< The header's controller (DrawPadGlyph).
    PromptFamily prompt_family{PromptFamily::Keyboard};
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
    const LibraryEntry* last_played{}; ///< Wears the "in progress" badge.
    TileKey focused_tile, left_tile;
    bool rows_shown{};
    bool animating{};
    std::string hero_path;
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
