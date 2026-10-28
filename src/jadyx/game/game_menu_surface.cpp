// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/game/game_menu_surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>

#include "jadyx/cheats/cheats_view.h"
#include "jadyx/game/game_menu.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/storage/utf8.h"
#include "jadyx/ui/list_navigation.h"
#include "jadyx/ui/text_entry_panel.h"
#include "jadyx/ui/ui_painter.h"

namespace Jadyx {
namespace {
using Microsoft::WRL::ComPtr;
using I18n::Text;

/// The full-screen menu, in layout units of the 1280x720 screen: the paused game on the left,
/// the panel on the right, the button prompts under it.
struct Screen {
    static constexpr float Left = 64, Right = 1216;
    static constexpr float Kicker = 62, Title = 88, TitleWidth = 640;
    static constexpr float Pill = 36, Caption = 664, Footer = 656;
    static constexpr float PanelX = 760, PanelY = 88, PanelWidth = 456, PanelHeight = 552, Inset = 22;
    static constexpr float InnerX = PanelX + Inset, InnerWidth = PanelWidth - 2 * Inset;
    static constexpr float HeaderY = PanelY + Inset;
    // The menu's rows.
    static constexpr float ListTop = 176, RowHeight = 56, RowStride = 60;
    static constexpr std::size_t VisibleRows = 7;
    // The cheats page.
    static constexpr float TabsTop = 168, TabHeight = 36;
    static constexpr float CheatTop = 216, CheatHeight = 52, CheatStride = 58;
    static constexpr float Budget = 512, Credit = 548, Note = 570;
};

/// Text under a focused (white) row.
constexpr unsigned OnAccentSecondary = 0x5c5f64;

std::wstring Upper(std::wstring_view text) {
    std::wstring upper(text);
    if (!text.empty() &&
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, text.data(),
                      static_cast<int>(text.size()), upper.data(), static_cast<int>(upper.size()), nullptr,
                      nullptr, 0) == 0)
        upper = text;
    return upper;
}

/// Three box blurs of `radius` over rows of `channels` floats approach a gaussian.
void BoxBlur(std::vector<float>& image, u32 width, u32 height, int radius) {
    constexpr int Channels = 3;
    std::vector<float> line;
    const auto pass = [&](u32 length, u32 lines, auto at) {
        line.resize(static_cast<std::size_t>(length) * Channels);
        for (u32 l = 0; l < lines; ++l) {
            for (u32 i = 0; i < length; ++i)
                for (int c = 0; c < Channels; ++c) line[i * Channels + c] = *at(l, i, c);
            for (u32 i = 0; i < length; ++i) {
                for (int c = 0; c < Channels; ++c) {
                    float sum = 0;
                    for (int k = -radius; k <= radius; ++k) {
                        const int j = std::clamp(static_cast<int>(i) + k, 0, static_cast<int>(length) - 1);
                        sum += line[static_cast<std::size_t>(j) * Channels + c];
                    }
                    *at(l, i, c) = sum / static_cast<float>(2 * radius + 1);
                }
            }
        }
    };
    for (int round = 0; round < 3; ++round) {
        pass(width, height, [&](u32 y, u32 x, int c) {
            return &image[(static_cast<std::size_t>(y) * width + x) * Channels + c];
        });
        pass(height, width, [&](u32 x, u32 y, int c) {
            return &image[(static_cast<std::size_t>(y) * width + x) * Channels + c];
        });
    }
}

/// A picture of the paused game, BGRA as Direct2D takes it.
struct FramePixels {
    u32 width{};
    u32 height{};
    std::vector<u8> pixels;
};
} // namespace

struct GameMenuSurface::Impl : UiPainter {
    Impl() {
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic)));
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d)));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())));
        try {
            const auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
            version_text = L"v" + std::to_wstring(version.Major) + L"." + std::to_wstring(version.Minor) + L"." +
                           std::to_wstring(version.Build) + L"." + std::to_wstring(version.Revision);
        } catch (const winrt::hresult_error&) { /* Not packaged: the caption goes without it. */ }
    }

    /// A bitmap of `width` x `height` pixels to draw on, kept while the size holds.
    void Prepare(UINT width, UINT height) {
        if (bitmap && width == bitmap_width && height == bitmap_height) return;
        target.Reset();
        bitmap.Reset();
        sharp_bitmap.Reset();
        soft_bitmap.Reset();
        winrt::check_hresult(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapCacheOnDemand, &bitmap));
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target));
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        Attach(target.Get(), text_factory.Get());
        bitmap_width = width;
        bitmap_height = height;
    }

    /// A panel of `width` x `height` layout units the renderer copies over the running game. The
    /// scale follows the screen's height (720 units), so the panel keeps its size on a 4K swapchain.
    template <typename Draw>
    D3D12::MenuImage Render(float width, float height, std::pair<u32, u32> screen, Draw&& draw) {
        const auto [screen_width, screen_height] = screen;
        if (screen_width == 0 || screen_height == 0) return {};
        const float scale = std::min({static_cast<float>(screen_height) / 720.0f,
                                      static_cast<float>(screen_width) * 0.95f / width,
                                      static_cast<float>(screen_height) * 0.95f / height});
        Prepare(static_cast<UINT>(std::ceil(width * scale)), static_cast<UINT>(std::ceil(height * scale)));
        SyncLanguage();
        target->BeginDraw();
        // The image is copied, not blended: the panel's color fills it to its square corners.
        target->Clear(D2D1::ColorF(Palette::Panel));
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        draw();
        // A text the system's fallback font cannot draw spoils the frame, not the game.
        (void)target->EndDraw();
        return Pixels();
    }

    /// The whole screen: the paused game (blurred, or `sharp` to judge the image settings), the
    /// scrims that keep text legible over it, and `draw` in the centred 1280x720 layout.
    template <typename Draw>
    D3D12::MenuImage RenderScreen(std::pair<u32, u32> screen, bool sharp, Draw&& draw) {
        const auto [width, height] = screen;
        if (width == 0 || height == 0) return {};
        Prepare(width, height);
        SyncLanguage();
        const auto w = static_cast<float>(width);
        const auto h = static_cast<float>(height);
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->Clear(D2D1::ColorF(Palette::Canvas));
        if (ID2D1Bitmap* const picture = Backdrop(sharp)) {
            target->DrawBitmap(picture, D2D1::RectF(0, 0, w, h), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            const float strength = sharp ? 0.7f : 0.85f;
            Scrim({0, 0}, {w * 0.62f, 0}, strength, w, h);
            Scrim({0, h * 0.55f}, {0, h}, sharp ? 0.0f : 0.75f, w, h, true);
            // The panel's side: enough to read the rows, light enough to see the game.
            if (sharp) Scrim({w, 0}, {w * 0.55f, 0}, 0.65f, w, h);
        }
        const float scale = std::min(w / 1280.0f, h / 720.0f);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation((w - 1280 * scale) / 2, (h - 720 * scale) / 2));
        draw();
        // A text the system's fallback font cannot draw spoils the frame, not the game.
        (void)target->EndDraw();
        auto image = Pixels();
        image.fill_screen = true;
        image.fill = Palette::Canvas;
        return image;
    }

    /// Canvas from `alpha` at `from` to clear at `to` (or the other way round, `rising`).
    void Scrim(D2D1_POINT_2F from, D2D1_POINT_2F to, float alpha, float w, float h, bool rising = false) {
        if (alpha <= 0) return;
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(Palette::Canvas, rising ? 0.0f : alpha)},
            {1.0f, D2D1::ColorF(Palette::Canvas, rising ? alpha : 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (FAILED(target->CreateGradientStopCollection(stops, 2, &collection)) ||
            FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                                     collection.Get(), &gradient)))
            return;
        target->FillRectangle(D2D1::RectF(0, 0, w, h), gradient.Get());
    }

    ID2D1Bitmap* Backdrop(bool sharp) {
        const FramePixels& picture = sharp ? sharp_picture : soft_picture;
        ComPtr<ID2D1Bitmap>& cached = sharp ? sharp_bitmap : soft_bitmap;
        if (picture.pixels.empty()) return nullptr;
        if (!cached) {
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(target->CreateBitmap(D2D1::SizeU(picture.width, picture.height), picture.pixels.data(),
                                            picture.width * 4, properties, &cached)))
                return nullptr;
        }
        return cached.Get();
    }

    void SetBackdrop(const D3D12::MenuImage& frame) {
        sharp_bitmap.Reset();
        soft_bitmap.Reset();
        sharp_picture = {};
        soft_picture = {};
        if (frame.width == 0 || frame.height == 0 ||
            frame.pixels.size() < static_cast<std::size_t>(frame.width) * frame.height * 4)
            return;
        frame_size = {frame.width, frame.height};
        sharp_picture = {frame.width, frame.height, std::vector<u8>(frame.pixels.size())};
        for (std::size_t i = 0; i + 3 < frame.pixels.size(); i += 4) {
            sharp_picture.pixels[i + 0] = frame.pixels[i + 2];
            sharp_picture.pixels[i + 1] = frame.pixels[i + 1];
            sharp_picture.pixels[i + 2] = frame.pixels[i + 0];
            sharp_picture.pixels[i + 3] = 0xFF;
        }
        // Blurred small and stretched back: a 240-pixel-wide average, three box blurs and dimmed.
        const u32 factor = std::max(1u, frame.width / 240);
        const u32 width = std::max(1u, frame.width / factor);
        const u32 height = std::max(1u, frame.height / factor);
        std::vector<float> reduced(static_cast<std::size_t>(width) * height * 3);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                float sum[3]{};
                for (u32 dy = 0; dy < factor; ++dy) {
                    const u8* in = frame.pixels.data() +
                                   ((static_cast<std::size_t>(y) * factor + dy) * frame.width + x * factor) * 4;
                    for (u32 dx = 0; dx < factor; ++dx, in += 4)
                        for (int c = 0; c < 3; ++c) sum[c] += in[c];
                }
                for (int c = 0; c < 3; ++c)
                    reduced[(static_cast<std::size_t>(y) * width + x) * 3 + c] =
                        sum[c] / static_cast<float>(factor * factor);
            }
        }
        BoxBlur(reduced, width, height, 3);
        soft_picture = {width, height, std::vector<u8>(static_cast<std::size_t>(width) * height * 4)};
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            for (int c = 0; c < 3; ++c)
                soft_picture.pixels[i * 4 + 2 - c] =
                    static_cast<u8>(std::clamp(reduced[i * 3 + c] * 0.55f, 0.0f, 255.0f));
            soft_picture.pixels[i * 4 + 3] = 0xFF;
        }
    }

    /// The bitmap as RGBA rows, the swapchain's order (D2D draws BGRA).
    D3D12::MenuImage Pixels() {
        D3D12::MenuImage image{bitmap_width, bitmap_height, {}};
        image.pixels.resize(static_cast<std::size_t>(bitmap_width) * bitmap_height * 4);
        const WICRect rect{0, 0, static_cast<INT>(bitmap_width), static_cast<INT>(bitmap_height)};
        ComPtr<IWICBitmapLock> lock;
        winrt::check_hresult(bitmap->Lock(&rect, WICBitmapLockRead, &lock));
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        winrt::check_hresult(lock->GetStride(&stride));
        winrt::check_hresult(lock->GetDataPointer(&size, &data));
        for (UINT y = 0; y < bitmap_height; ++y) {
            const BYTE* in = data + static_cast<std::size_t>(y) * stride;
            u8* out = image.pixels.data() + static_cast<std::size_t>(y) * bitmap_width * 4;
            for (UINT x = 0; x < bitmap_width; ++x, in += 4, out += 4) {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = 0xFF;
            }
        }
        return image;
    }

    u8 prompt_mapping{};

    /// The pad's own face: letters swapped by position, or colors left out, as input maps them.
    std::pair<std::wstring_view, unsigned> Key(std::wstring_view letters, unsigned color) const {
        if (prompt_mapping & 1) {
            if (letters == L"A") { letters = L"B"; color = ButtonColor::B; }
            else if (letters == L"B") { letters = L"A"; color = ButtonColor::A; }
            else if (letters == L"X") { letters = L"Y"; color = ButtonColor::Y; }
            else if (letters == L"Y") { letters = L"X"; color = ButtonColor::X; }
        }
        if (prompt_mapping & 2) color = ButtonColor::Neutral;
        return {letters, color};
    }

    float Prompt(std::wstring_view letters, unsigned color, std::wstring_view label, float x, float y) {
        const auto [key, tint] = Key(letters, color);
        return UiPainter::Prompt(key, tint, label, x, y);
    }

    struct PromptItem {
        std::wstring_view letters;
        unsigned color;
        std::wstring_view label;
    };

    /// The prompts in a line that ends at the screen's right margin.
    void Prompts(std::initializer_list<PromptItem> items) {
        float width = 0;
        for (const auto& item : items)
            width += BadgeWidth(Key(item.letters, item.color).first) + 8 +
                     MeasureType(item.label, TypeRole::LabelLarge) + 28;
        float x = Screen::Right - (width - 28);
        for (const auto& item : items) x += Prompt(item.letters, item.color, item.label, x, Screen::Footer);
    }

    /// The paused game beside the panel: a kicker with the session's time, its name and maker, the
    /// clock and renderer at the top and Jadyx's version at the bottom.
    void DrawScene(const GameMenuScene& scene) {
        using S = Screen;
        std::wstring kicker{I18n::Tr(Text::MenuPaused)};
        if (!scene.session.empty()) kicker += L" · " + I18n::Format(Text::MenuSession, {scene.session});
        Round(S::Left, S::Kicker + 4, 8, 8, 4, Palette::Text);
        Type(Upper(kicker), TypeRole::Caps, S::Left + 18, S::Kicker, S::TitleWidth, Palette::Text);
        const float title = Type(scene.title, TypeRole::Hero, S::Left, S::Title, S::TitleWidth - 40, Palette::Text,
                                 1, TextAlign::Leading, 2);
        if (!scene.details.empty())
            Type(scene.details, TypeRole::BodyLarge, S::Left, S::Title + title + 10, S::TitleWidth - 40,
                 Palette::TextSecondary);

        // The status pill: the time of day and what draws the game.
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t clock[8]{};
        std::swprintf(clock, std::size(clock), L"%02u:%02u", now.wHour, now.wMinute);
        std::wstring renderer = L"Direct3D 12";
        if (frame_size.first)
            renderer += L" · " + std::to_wstring(frame_size.first) + L"×" + std::to_wstring(frame_size.second);
        const float clock_width = MeasureType(clock, TypeRole::LabelLarge);
        const float renderer_width = MeasureType(renderer, TypeRole::LabelLarge);
        const float pill = 14 + 16 + 8 + clock_width + 25 + renderer_width + 16;
        const float x = S::Right - pill;
        Round(x, S::Pill, pill, 32, 16, Palette::Surface, 0.85f);
        Outline(x + 0.5f, S::Pill + 0.5f, pill - 1, 31, 15.5f, 0xffffff, 0.08f);
        Icon(UiIcon::Clock, x + 14, S::Pill + 8, Palette::TextSecondary, 16);
        Type(clock, TypeRole::LabelLarge, x + 38, S::Pill + 6, clock_width + 2, Palette::Text);
        Round(x + 38 + clock_width + 12, S::Pill + 9, 1, 14, 0.5f, 0xffffff, 0.2f);
        Type(renderer, TypeRole::LabelLarge, x + 38 + clock_width + 25, S::Pill + 6, renderer_width + 2,
             Palette::TextSecondary);

        std::wstring caption = L"Jadyx";
        if (!version_text.empty()) caption += L" " + version_text;
        Type(Upper(caption + L" · Direct3D 12"), TypeRole::Caps, S::Left, S::Caption + 8, 480,
             Palette::TextSecondary);
    }

    /// The panel and its header: an icon tile, the title and a line under it.
    void DrawPanel(UiIcon icon, std::wstring_view title, std::wstring_view subtitle, bool back = false) {
        using S = Screen;
        Round(S::PanelX, S::PanelY, S::PanelWidth, S::PanelHeight, 20, Palette::Surface, 0.94f);
        Outline(S::PanelX + 0.5f, S::PanelY + 0.5f, S::PanelWidth - 1, S::PanelHeight - 1, 19.5f, 0xffffff, 0.08f);
        Round(S::InnerX, S::HeaderY, 44, 44, 12, Palette::Graphite);
        Icon(icon, S::InnerX + 10, S::HeaderY + 10, Palette::Text, 24);
        const float text_x = S::InnerX + 58;
        const float text_width = S::InnerWidth - 58;
        if (subtitle.empty()) {
            Type(title, TypeRole::HeadlineSmall, text_x, S::HeaderY + 8, text_width, Palette::Text);
            return;
        }
        Type(title, TypeRole::HeadlineSmall, text_x, S::HeaderY - 2, text_width, Palette::Text);
        float sub_x = text_x;
        if (back) {
            const auto [key, tint] = Key(L"B", ButtonColor::B);
            sub_x += Badge(key, sub_x + BadgeWidth(key) / 2, S::HeaderY + 36, tint) + 8;
        }
        Type(subtitle, TypeRole::Label, sub_x, S::HeaderY + 28, text_x + text_width - sub_x, Palette::TextSecondary);
    }

    void DrawRow(const GameMenuRow& row, float top, bool active) {
        using S = Screen;
        const float x = S::InnerX;
        const float width = S::InnerWidth - 10;
        const float right = x + width;
        if (active) {
            Round(x - 1, top + 1, width + 2, S::RowHeight + 2, 15, 0xffffff, 0.06f);
            Round(x, top, width, S::RowHeight, 14, Palette::Text);
        }
        float text_x = x + 18;
        if (row.icon) {
            Round(x + 10, top + 10, 36, 36, 10, active ? 0xe4e5e8 : Palette::Graphite);
            Icon(*row.icon, x + 17, top + 17, active ? Palette::OnAccent : Palette::Text, 22);
            text_x = x + 58;
        }
        const unsigned label_color = active ? Palette::OnAccent : Palette::Text;
        const unsigned secondary = active ? OnAccentSecondary : Palette::TextSecondary;
        const float middle = top + S::RowHeight / 2;

        // What sits at the row's end, right to left: a page's ">", a setting's value between its
        // arrows, the button that does it.
        float trailing = right - 14;
        switch (row.kind) {
        case GameMenuRow::Kind::Page:
            Chevron(right - 18, middle, active ? Palette::OnAccent : Palette::TextSecondary);
            trailing = right - 36;
            break;
        case GameMenuRow::Kind::Choice: {
            const unsigned arrows = active ? Palette::OnAccent : Palette::TextSecondary;
            Chevron(right - 16, middle, arrows);
            float value_left = right - 30;
            if (!row.value.empty()) {
                const float value_width = std::min(MeasureType(row.value, TypeRole::Body) + 2, width * 0.5f);
                value_left -= value_width;
                Type(row.value, TypeRole::Body, value_left, middle - 11, value_width, label_color, 1,
                     TextAlign::Trailing);
                value_left -= 6;
            }
            Chevron(value_left - 8, middle, arrows, true);
            trailing = value_left - 24;
            break;
        }
        case GameMenuRow::Kind::Info:
            break;
        case GameMenuRow::Kind::Action:
            if (active) {
                const auto [key, tint] = Key(L"A", ButtonColor::A);
                Badge(key, right - 14 - BadgeWidth(key) / 2, middle, tint);
                trailing = right - 24 - BadgeWidth(key);
            }
            break;
        }
        const float text_width = std::max(trailing - text_x, 40.0f);
        if (row.detail.empty()) {
            Type(row.label, TypeRole::Body, text_x, middle - 11, text_width, label_color);
        } else {
            Type(row.label, TypeRole::Body, text_x, top + 8, text_width, label_color);
            Type(row.detail, TypeRole::Label, text_x, top + 31, text_width, secondary);
        }
    }

    void DrawMenu(const GameMenu& menu, const GameMenuScene& scene) {
        using S = Screen;
        DrawScene(scene);
        const auto page = menu.CurrentPage();
        if (page != menu_page) {
            menu_page = page;
            menu_first = 0;
        }
        const bool quick = page == GameMenu::Page::Quick;
        const UiIcon icon = page == GameMenu::Page::Controller ? UiIcon::Controller
                            : page == GameMenu::Page::Display  ? UiIcon::Display
                                                               : UiIcon::Game;
        const std::wstring back = quick ? std::wstring{} : I18n::Format(Text::MenuBackTo, {I18n::Tr(Text::MenuQuick)});
        DrawPanel(icon, menu.PageTitle(), back, !quick);

        const auto rows = menu.Rows();
        std::size_t selected = menu.Selected();
        ClampListWindow(selected, menu_first, rows.size(), S::VisibleRows);
        const std::size_t end = std::min(rows.size(), menu_first + S::VisibleRows);
        for (std::size_t i = menu_first; i < end; ++i)
            DrawRow(rows[i], S::ListTop + static_cast<float>(i - menu_first) * S::RowStride, i == selected);
        ScrollIndicator(S::InnerX + S::InnerWidth - 4, S::ListTop,
                        S::VisibleRows * S::RowStride - (S::RowStride - S::RowHeight), menu_first, S::VisibleRows,
                        rows.size());

        if (quick) {
            Prompts({{L"A", ButtonColor::A, I18n::Tr(Text::Select)}, {L"B", ButtonColor::B, I18n::Tr(Text::MenuResume)}});
        } else {
            // A and left/right both change a setting; the arrows on the row already say so.
            Prompts({{L"A", ButtonColor::A, I18n::Tr(Text::Change)}, {L"B", ButtonColor::B, I18n::Tr(Text::Back)}});
        }
    }

    /// The categories as a segmented control, with how many of each are on.
    void DrawTabs(const Cheats::CheatCatalog& catalog, Cheats::CheatCategory current) {
        using S = Screen;
        std::array<Cheats::CheatCategory, Cheats::CategoryCount> storage;
        const auto tabs = Cheats::CheatTabs(catalog, storage);
        if (tabs.empty()) return;
        const float width = S::InnerWidth - 10;
        Round(S::InnerX, S::TabsTop, width, S::TabHeight, S::TabHeight / 2, Palette::Graphite);
        const float segment = width / static_cast<float>(tabs.size());
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            const auto category = tabs[i];
            const bool active = category == current;
            const float x = S::InnerX + segment * static_cast<float>(i);
            if (active) Round(x + 3, S::TabsTop + 3, segment - 6, S::TabHeight - 6, (S::TabHeight - 6) / 2, Palette::Text);
            const std::wstring label = std::wstring{Cheats::CategoryName(category)} + L" " +
                                       std::to_wstring(catalog.EnabledCount(category)) + L"/" +
                                       std::to_wstring(catalog.Cheats(category).size());
            Type(label, TypeRole::LabelLarge, x + 6, S::TabsTop + 8, segment - 12,
                 active ? Palette::OnAccent : Palette::TextSecondary, 1, TextAlign::Center);
        }
    }

    void DrawCheatList(const Cheats::CheatCatalog& catalog, const Cheats::CheatsPage& page) {
        using S = Screen;
        const auto cheats = catalog.Cheats(page.category);
        const float x = S::InnerX;
        const float width = S::InnerWidth - 10;
        const std::size_t end = std::min(cheats.size(), page.first + CheatRows);
        for (std::size_t i = page.first; i < end; ++i) {
            const float top = S::CheatTop + static_cast<float>(i - page.first) * S::CheatStride;
            const bool active = i == page.selected;
            const bool on = catalog.Enabled(cheats[i]);
            Round(x, top, width, S::CheatHeight, 14, active ? Palette::Text : Palette::Row);
            Type(Utf8ToUtf16(catalog.Name(cheats[i])), TypeRole::Body, x + 18, top + 15, width - 18 - 80,
                 active ? Palette::OnAccent : on ? Palette::Text : Palette::TextHint);
            const float switch_x = x + width - 52 - 14;
            const float switch_y = top + 12;
            if (active) {
                // The switch inverted on the white row: dark when on, a light grey track when off.
                Round(switch_x, switch_y, 52, 28, 14, on ? Palette::OnAccent : 0xd3d4d8);
                brush->SetColor(D2D1::ColorF(0xffffff));
                surface->FillEllipse(D2D1::Ellipse(D2D1::Point2F(on ? switch_x + 38 : switch_x + 14, switch_y + 14),
                                                   10, 10),
                                     brush.Get());
            } else {
                Switch(switch_x, switch_y, on, false);
            }
        }
        ScrollIndicator(S::InnerX + S::InnerWidth - 4, S::CheatTop,
                        CheatRows * S::CheatStride - (S::CheatStride - S::CheatHeight), page.first, CheatRows,
                        cheats.size());
    }

    void DrawCheatsEmpty(const Cheats::CheatSession& session) {
        using S = Screen;
        std::wstring message;
        if (session.Loading() || session.Source() == Cheats::CheatSource::Checking) {
            message = I18n::Tr(Text::CheatsLoading);
        } else if (session.Source() == Cheats::CheatSource::Offline) {
            message = I18n::Tr(Text::CheatsOffline);
        } else {
            message = I18n::Tr(Text::CheatsNone);
            if (session.OtherBuilds())
                message += L"\n" + I18n::Plural(Text::CheatsOtherBuildsOne, Text::CheatsOtherBuildsOther,
                                                session.OtherBuilds());
        }
        Icon(UiIcon::Cheats, S::InnerX + S::InnerWidth / 2 - 16, 250, Palette::TextSecondary);
        Type(message, TypeRole::Body, S::InnerX + 10, 300, S::InnerWidth - 20, Palette::TextHint, 1,
             TextAlign::Center, 5);
    }

    void DrawCheats(const Cheats::CheatsView& view, const GameMenuScene& scene) {
        using S = Screen;
        DrawScene(scene);
        const auto& session = view.session;
        const auto& catalog = session.Catalog();
        std::size_t on = 0;
        for (const auto category :
             {Cheats::CheatCategory::Gameplay, Cheats::CheatCategory::Graphics, Cheats::CheatCategory::Mine})
            on += catalog.EnabledCount(category);
        std::wstring subtitle = I18n::Plural(Text::CheatsOnOne, Text::CheatsOnOther, on);
        DrawPanel(UiIcon::Cheats, I18n::Tr(Text::CheatsTitle), subtitle);
        if (catalog.Empty()) {
            DrawCheatsEmpty(session);
        } else {
            DrawTabs(catalog, view.page.category);
            DrawCheatList(catalog, view.page);
        }

        // The cheat engine's room, where the cheats came from and the last press's outcome.
        const float x = S::InnerX;
        const float width = S::InnerWidth - 10;
        const std::size_t used = catalog.UsedWords();
        Type(I18n::Format(Text::CheatsBudget, {std::to_wstring(used), std::to_wstring(Cheats::WordBudget)}),
             TypeRole::Label, x, S::Budget, width * 0.5f, Palette::TextSecondary);
        const bool online = session.Source() == Cheats::CheatSource::Online;
        Type(Cheats::SourceText(session.Source()), TypeRole::Label, x + width * 0.5f, S::Budget, width * 0.5f,
             online ? Palette::TextSecondary : Palette::Warning, 1, TextAlign::Trailing);
        const float filled = width * static_cast<float>(std::min(used, Cheats::WordBudget)) / Cheats::WordBudget;
        Round(x, S::Budget + 22, width, 6, 3, Palette::Track);
        if (filled > 0)
            Round(x, S::Budget + 22, std::max(filled, 6.0f), 6, 3,
                  used * 5 > Cheats::WordBudget * 4 ? Palette::Warning : Palette::Accent);
        std::wstring credit{I18n::Tr(Text::CheatsCredit)};
        for (std::size_t i = 0; i < std::min<std::size_t>(session.Credits().size(), 3); ++i)
            credit += (i ? L", " : L" · ") + Utf8ToUtf16(session.Credits()[i]);
        Type(credit, TypeRole::Label, x, S::Credit, width, Palette::TextSecondary);
        const bool notice = !view.notice.empty();
        const std::wstring_view note =
            notice ? view.notice : I18n::Tr(view.running ? Text::CheatsAppliesNow : Text::CheatsAppliesAtBoot);
        Type(note, TypeRole::Label, x, S::Note, width, notice ? Palette::Warning : Palette::TextHint, 1,
             TextAlign::Leading, 2);

        const bool tabs = Cheats::CheatTabCount(catalog) > 1;
        if (tabs) {
            Prompts({{L"A", ButtonColor::A, I18n::Tr(Text::ToggleCheat)},
                     {L"X", ButtonColor::X, I18n::Tr(Text::AllCheats)},
                     {L"LB", ButtonColor::Neutral, I18n::Tr(Text::Category)},
                     {L"Y", ButtonColor::Y, I18n::Tr(Text::Refresh)},
                     {L"B", ButtonColor::B, I18n::Tr(Text::Back)}});
        } else {
            Prompts({{L"A", ButtonColor::A, I18n::Tr(Text::ToggleCheat)},
                     {L"X", ButtonColor::X, I18n::Tr(Text::AllCheats)},
                     {L"Y", ButtonColor::Y, I18n::Tr(Text::Refresh)},
                     {L"B", ButtonColor::B, I18n::Tr(Text::Back)}});
        }
    }

    void DrawKeyboard(const TextEntryPanelView& view, bool can_cancel) {
        using K = TextEntryPanelLayout;
        DrawTextEntryPanel(*this, view, 0, 0);
        // The pad as the console's keyboard: A types, X deletes, Y adds a space; the bumpers stand
        // in for + and -, which the game menu's shortcut holds.
        float x = K::Left;
        const float y = K::Footer;
        x += Prompt(L"A", ButtonColor::A, I18n::Tr(Text::TypeKey), x, y);
        x += Prompt(L"X", ButtonColor::X, I18n::Tr(Text::DeleteKey), x, y);
        x += Prompt(L"Y", ButtonColor::Y, I18n::Tr(Text::SpaceKey), x, y);
        x += Prompt(L"LB", ButtonColor::Neutral, I18n::Tr(Text::ShiftKey), x, y);
        x += Prompt(L"RB", ButtonColor::Neutral, I18n::Tr(Text::DoneKey), x, y);
        if (can_cancel) Prompt(L"B", ButtonColor::B, I18n::Tr(Text::Cancel), x, y);
    }

    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    UINT bitmap_width{};
    UINT bitmap_height{};

    FramePixels sharp_picture;
    FramePixels soft_picture;
    ComPtr<ID2D1Bitmap> sharp_bitmap;
    ComPtr<ID2D1Bitmap> soft_bitmap;
    std::pair<u32, u32> frame_size{};
    std::wstring version_text;

    GameMenu::Page menu_page{GameMenu::Page::Quick};
    std::size_t menu_first{};
};

GameMenuSurface::GameMenuSurface() : impl{std::make_unique<Impl>()} {}
GameMenuSurface::~GameMenuSurface() = default;

void GameMenuSurface::SetPromptMapping(u8 mapping) { impl->prompt_mapping = mapping; }

void GameMenuSurface::SetBackdrop(const D3D12::MenuImage& frame) { impl->SetBackdrop(frame); }

D3D12::MenuImage GameMenuSurface::DrawMenu(const GameMenu& menu, const GameMenuScene& scene,
                                          std::pair<u32, u32> screen) {
    // The display page shows the game sharp: its settings change the very image behind it.
    const bool sharp = menu.CurrentPage() == GameMenu::Page::Display;
    return impl->RenderScreen(screen, sharp, [&] { impl->DrawMenu(menu, scene); });
}

D3D12::MenuImage GameMenuSurface::DrawCheats(const Cheats::CheatsView& view, const GameMenuScene& scene,
                                            std::pair<u32, u32> screen) {
    return impl->RenderScreen(screen, false, [&] { impl->DrawCheats(view, scene); });
}

D3D12::MenuImage GameMenuSurface::DrawKeyboard(const TextEntryPanelView& view, bool can_cancel,
                                              std::pair<u32, u32> screen) {
    using K = TextEntryPanelLayout;
    return impl->Render(K::Width, K::Height, screen, [&] { impl->DrawKeyboard(view, can_cancel); });
}

} // namespace Jadyx
