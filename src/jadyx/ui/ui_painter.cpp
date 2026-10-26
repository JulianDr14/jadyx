// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/ui/ui_painter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <dwrite_1.h>
#include <winrt/base.h>

#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_fonts.h"

namespace Jadyx {
using Microsoft::WRL::ComPtr;

namespace {

struct TypeStyle {
    float size;
    float line;
    DWRITE_FONT_WEIGHT weight;
    float tracking; ///< In em.
    bool tabular;
};

constexpr std::array<TypeStyle, static_cast<size_t>(TypeRole::Count)> TypeScale{{
    {56, 64, DWRITE_FONT_WEIGHT_SEMI_BOLD, -0.025f, false},
    {40, 48, DWRITE_FONT_WEIGHT_SEMI_BOLD, -0.02f, false},
    {28, 36, DWRITE_FONT_WEIGHT_MEDIUM, -0.015f, false},
    {22, 28, DWRITE_FONT_WEIGHT_MEDIUM, -0.01f, false},
    {18, 26, DWRITE_FONT_WEIGHT_NORMAL, -0.005f, false},
    {15, 22, DWRITE_FONT_WEIGHT_NORMAL, 0, false},
    {14, 20, DWRITE_FONT_WEIGHT_MEDIUM, 0.01f, true},
    {12, 16, DWRITE_FONT_WEIGHT_MEDIUM, 0.02f, true},
    {13, 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0.015f, true},
    {12, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0.2f, false},
}};

constexpr unsigned SweepFrames = 120;

} // Anonymous namespace

void UiPainter::Attach(ID2D1RenderTarget* target, IDWriteFactory* write_) {
    surface = target;
    write = write_;
    brush.Reset();
    layouts.clear();
    winrt::check_hresult(surface->CreateSolidColorBrush(D2D1::ColorF(Palette::Text), &brush));
}

void UiPainter::SyncLanguage() {
    const auto tag = I18n::CurrentUiLanguage().tag;
    if (tag != format_locale) {
        formats.clear();
        role_formats.clear();
        layouts.clear();
        format_locale = std::wstring{tag};
    }
    // Texts not drawn for a couple of seconds' worth of frames are laid out again if they return.
    if (++layout_frame % SweepFrames == 0)
        std::erase_if(layouts, [&](const auto& entry) { return layout_frame - entry.second.frame > SweepFrames; });
}

void UiPainter::Round(float x, float y, float w, float h, float radius, unsigned color, float alpha) {
    brush->SetColor(D2D1::ColorF(color, alpha));
    const auto rect = D2D1::RectF(x, y, x + w, y + h);
    if (radius <= 0) surface->FillRectangle(rect, brush.Get());
    else surface->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
}

void UiPainter::Outline(float x, float y, float w, float h, float radius, unsigned color, float alpha,
                        float stroke) {
    brush->SetColor(D2D1::ColorF(color, alpha));
    surface->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius),
                                  brush.Get(), stroke);
}

void UiPainter::Card(float x, float y, float w, float h, float radius) {
    Round(x, y, w, h, radius, Palette::Surface);
    Outline(x + 0.5f, y + 0.5f, w - 1, h - 1, radius, 0xffffff, 0.08f);
}

IDWriteTextLayout* UiPainter::TypeLayout(std::wstring_view text, TypeRole role, float width,
                                     TextAlign align, unsigned lines) {
    const auto index = static_cast<unsigned>(role);
    const auto& style = TypeScale[index];
    std::wstring key{text};
    key.push_back(L'\x1f');
    unsigned bits;
    std::memcpy(&bits, &width, sizeof bits);
    key.append({static_cast<wchar_t>(index), static_cast<wchar_t>(align), static_cast<wchar_t>(lines),
                static_cast<wchar_t>(bits & 0xffff), static_cast<wchar_t>(bits >> 16)});
    auto& cached = layouts[key];
    cached.frame = layout_frame;
    if (cached.layout) return cached.layout.Get();
    auto& format = role_formats[index];
    if (!format) {
        const auto typeface = InterfaceFonts(write.Get());
        winrt::check_hresult(write->CreateTextFormat(typeface.family, typeface.collection,
            style.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.size,
            format_locale.c_str(), &format));
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style.line, style.line * 0.8f);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        winrt::check_hresult(write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
        format->SetTrimming(&trim, ellipsis.Get());
    }
    const float height = style.line * static_cast<float>(std::max(lines, 1U));
    winrt::check_hresult(write->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
        format.Get(), std::max(width, 1.0f), height, &cached.layout));
    auto* layout = cached.layout.Get();
    layout->SetWordWrapping(lines > 1 ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align == TextAlign::Center     ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == TextAlign::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                            : DWRITE_TEXT_ALIGNMENT_LEADING);
    const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
    if (style.tracking != 0) {
        ComPtr<IDWriteTextLayout1> spaced;
        if (SUCCEEDED(cached.layout.As(&spaced)))
            spaced->SetCharacterSpacing(0, style.tracking * style.size, 0, all);
    }
    if (style.tabular) {
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(write->CreateTypography(&typography))) {
            typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            layout->SetTypography(typography.Get(), all);
        }
    }
    return layout;
}

float UiPainter::Type(std::wstring_view text, TypeRole role, float x, float y, float width,
                      unsigned color, float alpha, TextAlign align, unsigned lines) {
    if (text.empty() || width < 1) return 0;
    auto* layout = TypeLayout(text, role, width, align, lines);
    brush->SetColor(D2D1::ColorF(color, alpha));
    surface->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::min(metrics.height, TypeScale[static_cast<size_t>(role)].line *
                                        static_cast<float>(std::max(lines, 1U)));
}

float UiPainter::MeasureType(std::wstring_view text, TypeRole role) {
    if (text.empty()) return 0;
    DWRITE_TEXT_METRICS metrics{};
    TypeLayout(text, role, 4096, TextAlign::Leading, 1)->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void UiPainter::FocusGlow(float x, float y, float w, float h, float radius, float amount) {
    if (amount <= 0.001f) return;
    // Two diffuse drops (0 24 48 -12 at 65% and 0 8 16 -4 at 45%), as stacked translucent fills.
    for (int i = 0; i < 6; ++i) {
        const float spread = 4.0f + i * 5.0f;
        Round(x - spread + 12, y + 18 - spread + 12, w + 2 * spread - 24, h + 2 * spread - 24,
              radius + spread, 0, 0.07f * amount);
    }
    for (int i = 0; i < 3; ++i) {
        const float spread = 1.0f + i * 3.0f;
        Round(x - spread + 4, y + 6 - spread + 4, w + 2 * spread - 8, h + 2 * spread - 8,
              radius + spread, 0, 0.12f * amount);
    }
    // The halo: white 15% fading over 24 px, then the 1.5 px ring at 80%.
    for (int i = 0; i < 4; ++i) {
        const float spread = 3.0f + i * 5.0f;
        Outline(x - spread, y - spread, w + 2 * spread, h + 2 * spread, radius + spread, 0xffffff,
                0.04f * amount * (1.0f - i / 4.0f), 5);
    }
    Outline(x - 1.5f, y - 1.5f, w + 3, h + 3, radius + 1.5f, 0xffffff, 0.82f * amount, 1.5f);
}

float UiPainter::BadgeWidth(std::wstring_view letters) {
    return letters.size() > 1 ? std::max(22.0f, MeasureType(letters, TypeRole::Label) + 14) : 22.0f;
}
float UiPainter::Badge(std::wstring_view letters, float cx, float cy, unsigned color) {
    const float width = BadgeWidth(letters);
    Round(cx - width / 2, cy - 11, width, 22, 11, Palette::Graphite);
    Outline(cx - width / 2 + 0.5f, cy - 10.5f, width - 1, 21, 10.5f, 0xffffff, 0.15f);
    Type(letters, TypeRole::Label, cx - width / 2, cy - 8, width, color, 1, TextAlign::Center);
    return width;
}
float UiPainter::PillWidth(std::wstring_view label, std::wstring_view letters) {
    const float badge = letters.empty() ? 0.0f : 12 + BadgeWidth(letters);
    return 28 + MeasureType(label, TypeRole::BodyLarge) + badge + 22;
}
float UiPainter::PillButton(std::wstring_view label, std::wstring_view letters, unsigned letters_color,
                            float x, float y, bool primary, float height) {
    const float text = MeasureType(label, TypeRole::BodyLarge);
    const float width = PillWidth(label, letters);
    if (primary) {
        // A soft white lift under the primary action.
        Round(x - 2, y + 2, width + 4, height + 4, (height + 4) / 2, 0xffffff, 0.06f);
        Round(x, y, width, height, height / 2, Palette::Text);
    } else {
        Round(x, y, width, height, height / 2, 0xffffff, 0.10f);
        Outline(x + 0.5f, y + 0.5f, width - 1, height - 1, (height - 1) / 2, 0xffffff, 0.08f);
    }
    Type(label, TypeRole::BodyLarge, x + 28, y + (height - 26) / 2, text + 2,
         primary ? Palette::OnAccent : Palette::Text);
    // The button that presses it, as everywhere: a dark badge, even on the white pill.
    if (!letters.empty()) Badge(letters, x + 28 + text + 12 + BadgeWidth(letters) / 2, y + height / 2, letters_color);
    return width;
}
float UiPainter::Chip(std::wstring_view text, float x, float y, unsigned color, float alpha) {
    const float width = MeasureType(text, TypeRole::Label) + 24;
    Round(x, y, width, 26, 13, 0xffffff, 0.08f * alpha);
    Outline(x + 0.5f, y + 0.5f, width - 1, 25, 12.5f, 0xffffff, 0.10f * alpha);
    Type(text, TypeRole::Label, x + 12, y + 5, width - 22, color, alpha);
    return width;
}
IDWriteTextFormat* UiPainter::Format(float size, bool bold, bool wrap, bool center, bool trailing) {
    const unsigned key = static_cast<unsigned>(size) * 16 + (bold ? 1 : 0) + (wrap ? 2 : 0) +
                         (center ? 4 : 0) + (trailing ? 8 : 0);
    auto& format = formats[key];
    if (!format) {
        const auto typeface = InterfaceFonts(write.Get());
        winrt::check_hresult(write->CreateTextFormat(typeface.family, typeface.collection,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, format_locale.c_str(), &format));
        if (center) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else if (trailing) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        winrt::check_hresult(write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
        format->SetTrimming(&trim, ellipsis.Get());
    }
    return format.Get();
}

void UiPainter::Text(std::wstring_view text, float x, float y, float size, unsigned color,
                     float width, float height, bool bold, bool wrap, bool center, bool trailing) {
    brush->SetColor(D2D1::ColorF(color));
    surface->DrawText(text.data(), static_cast<UINT32>(text.size()),
                      Format(size, bold, wrap, center, trailing),
                      D2D1::RectF(x, y, x + width, y + height), brush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float UiPainter::MeasureText(std::wstring_view text, float size, bool bold) {
    if (text.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                       Format(size, bold, false, false, false), 4096, 100, &layout)))
        return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void UiPainter::Icon(UiIcon icon, float x, float y, unsigned color, float size) {
    if (size != 32) {
        D2D1_MATRIX_3X2_F previous;
        surface->GetTransform(&previous);
        surface->SetTransform(D2D1::Matrix3x2F::Scale(size / 32, size / 32, D2D1::Point2F(x, y)) * previous);
        Icon(icon, x, y, color);
        surface->SetTransform(previous);
        return;
    }
    brush->SetColor(D2D1::ColorF(color));
    const auto line = [&](float ax, float ay, float bx, float by) {
        surface->DrawLine(D2D1::Point2F(x + ax, y + ay), D2D1::Point2F(x + bx, y + by), brush.Get(), 2);
    };
    const auto circle = [&](float cx, float cy, float radius) {
        surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + cx, y + cy), radius, radius), brush.Get(), 2);
    };
    switch (icon) {
    case UiIcon::Folder:
    case UiIcon::Remove:
        line(3, 10, 3, 28); line(3, 28, 29, 28); line(29, 28, 29, 10);
        line(29, 10, 16, 10); line(16, 10, 12, 6); line(12, 6, 3, 6); line(3, 6, 3, 10);
        line(3, 14, 29, 14);
        if (icon == UiIcon::Remove) line(11, 21, 21, 21);
        break;
    case UiIcon::Controller:
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 1, y + 7, x + 31, y + 27), 7, 7), brush.Get(), 2);
        line(7, 17, 15, 17); line(11, 13, 11, 21);
        circle(23, 14, 1.4f); circle(26, 20, 1.4f);
        break;
    case UiIcon::Key:
        circle(10, 12, 7); circle(10, 12, 2);
        line(15, 17, 28, 30); line(22, 24, 26, 20); line(26, 28, 30, 24);
        break;
    case UiIcon::Firmware:
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 7, y + 7, x + 25, y + 25), 3, 3), brush.Get(), 2);
        surface->DrawRectangle(D2D1::RectF(x + 12, y + 12, x + 20, y + 20), brush.Get(), 2);
        for (float p : {11.0f, 21.0f}) {
            line(p, 2, p, 7); line(p, 25, p, 30);
            line(2, p, 7, p); line(25, p, 30, p);
        }
        break;
    case UiIcon::User:
        circle(16, 10, 6);
        line(5, 29, 7, 22); line(7, 22, 12, 19); line(12, 19, 20, 19);
        line(20, 19, 25, 22); line(25, 22, 27, 29); line(27, 29, 5, 29);
        break;
    case UiIcon::Language:
        circle(16, 16, 12);
        surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 16, y + 16), 5, 12), brush.Get(), 2);
        line(4, 16, 28, 16); line(7, 10, 25, 10); line(7, 22, 25, 22);
        break;
    case UiIcon::Region:
        circle(16, 12, 7); circle(16, 12, 2.5f);
        line(10, 16, 16, 28); line(16, 28, 22, 16);
        break;
    case UiIcon::Cache:
        line(6, 9, 26, 9); line(13, 9, 13, 5); line(13, 5, 19, 5); line(19, 5, 19, 9);
        line(8, 9, 10, 28); line(10, 28, 22, 28); line(22, 28, 24, 9);
        line(13, 13, 13, 24); line(19, 13, 19, 24);
        break;
    case UiIcon::Settings: {
        circle(16, 16, 10); circle(16, 16, 4);
        static constexpr std::array<D2D1_POINT_2F, 8> directions{{
            {0, -1}, {0.707f, -0.707f}, {1, 0}, {0.707f, 0.707f},
            {0, 1}, {-0.707f, 0.707f}, {-1, 0}, {-0.707f, -0.707f}}};
        for (const auto d : directions)
            line(16 + 11 * d.x, 16 + 11 * d.y, 16 + 15 * d.x, 16 + 15 * d.y);
        break;
    }
    case UiIcon::Cheats:
        // A spark: a four-pointed star and a small one beside it.
        line(13, 3, 16, 13); line(16, 13, 26, 16); line(26, 16, 16, 19); line(16, 19, 13, 29);
        line(13, 29, 10, 19); line(10, 19, 0, 16); line(0, 16, 10, 13); line(10, 13, 13, 3);
        line(26, 2, 26, 10); line(22, 6, 30, 6);
        break;
    case UiIcon::Game:
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 3, y + 3, x + 29, y + 29), 6, 6), brush.Get(), 2);
        line(9, 22, 14, 13); line(14, 13, 18, 19); line(18, 19, 20, 16); line(20, 16, 24, 22);
        circle(21, 10, 2);
        break;
    case UiIcon::Mods:
        // Layers stacked over the game: what mods are to the loader.
        line(16, 4, 28, 10); line(28, 10, 16, 16); line(16, 16, 4, 10); line(4, 10, 16, 4);
        line(4, 16, 16, 22); line(16, 22, 28, 16);
        line(4, 22, 16, 28); line(16, 28, 28, 22);
        break;
    case UiIcon::Archive:
        // A box with a zip down its middle.
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 6, y + 3, x + 26, y + 29), 3, 3), brush.Get(), 2);
        for (float p : {6.0f, 12.0f}) { line(14, p, 16, p); line(16, p + 3, 18, p + 3); }
        surface->DrawRectangle(D2D1::RectF(x + 13, y + 18, x + 19, y + 25), brush.Get(), 2);
        break;
    case UiIcon::Clock:
        circle(16, 16, 12);
        line(16, 16, 16, 8); line(16, 16, 22, 19);
        break;
    case UiIcon::Play:
        line(10, 6, 26, 16); line(26, 16, 10, 26); line(10, 26, 10, 6);
        break;
    case UiIcon::Camera:
        // A body with its viewfinder hump and the lens.
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 2, y + 9, x + 30, y + 28), 4, 4), brush.Get(), 2);
        line(10, 9, 12, 5); line(12, 5, 20, 5); line(20, 5, 22, 9);
        circle(16, 18, 5.5f);
        break;
    case UiIcon::Display:
        // A screen on its stand.
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 2, y + 5, x + 30, y + 23), 3, 3), brush.Get(), 2);
        line(16, 23, 16, 28); line(10, 28, 22, 28);
        break;
    case UiIcon::Exit:
        // An open door frame and the arrow leaving through it.
        line(18, 5, 6, 5); line(6, 5, 6, 27); line(6, 27, 18, 27);
        line(13, 16, 29, 16); line(29, 16, 24, 11); line(29, 16, 24, 21);
        break;
    case UiIcon::Airplane:
        // Seen from above: the body, the wings and the tail.
        line(16, 3, 16, 27);
        line(3, 19, 16, 11); line(16, 11, 29, 19);
        line(10, 29, 16, 25); line(16, 25, 22, 29);
        break;
    }
}

void UiPainter::Chevron(float cx, float cy, unsigned color, bool left) {
    brush->SetColor(D2D1::ColorF(color));
    const float d = left ? -1.0f : 1.0f;
    surface->DrawLine(D2D1::Point2F(cx - 4 * d, cy - 5), D2D1::Point2F(cx + 1 * d, cy), brush.Get(), 2);
    surface->DrawLine(D2D1::Point2F(cx + 1 * d, cy), D2D1::Point2F(cx - 4 * d, cy + 5), brush.Get(), 2);
}

void UiPainter::MoreBelow(float cx, float cy, float alpha) {
    brush->SetColor(D2D1::ColorF(Palette::TextSecondary, alpha));
    surface->DrawLine(D2D1::Point2F(cx - 6, cy - 3), D2D1::Point2F(cx, cy + 3), brush.Get(), 2);
    surface->DrawLine(D2D1::Point2F(cx, cy + 3), D2D1::Point2F(cx + 6, cy - 3), brush.Get(), 2);
}

void UiPainter::ScrollIndicator(float x, float y, float height, std::size_t first,
                                std::size_t visible, std::size_t count, bool more_below) {
    if (height < 48 || !visible || (!more_below && count <= visible)) return;
    first = std::min(first, count > visible ? count - visible : std::size_t{0});
    const bool above = first > 0;
    const bool below = more_below || visible < count - first;
    const auto arrow = [&](float cy, bool up, bool available) {
        brush->SetColor(D2D1::ColorF(available ? Palette::TextSecondary : Palette::Track));
        const float tip = cy + (up ? -3.0f : 3.0f);
        const float base = cy + (up ? 3.0f : -3.0f);
        surface->DrawLine(D2D1::Point2F(x - 4, base), D2D1::Point2F(x, tip), brush.Get(), 2);
        surface->DrawLine(D2D1::Point2F(x, tip), D2D1::Point2F(x + 4, base), brush.Get(), 2);
    };
    arrow(y + 5, true, above);
    arrow(y + height - 5, false, below);
    const float track_y = y + 18, track_h = height - 36;
    const float fraction = count ? std::min(1.0f, static_cast<float>(visible) / count) : 1.0f;
    const float thumb_h = std::min(track_h, std::max(20.0f, track_h * fraction));
    const float position = count > visible ? static_cast<float>(first) / (count - visible) : 0.0f;
    Round(x - 1.5f, track_y, 3, track_h, 1.5f, Palette::Graphite);
    Round(x - 2, track_y + (track_h - thumb_h) * position, 4, thumb_h, 2, Palette::Text, 0.85f);
}

void UiPainter::SelectionCard(float x, float y, float w, float h, bool focused, float radius) {
    Round(x, y, w, h, radius, focused ? Palette::RowActive : Palette::Surface);
    Outline(x + 0.5f, y + 0.5f, w - 1, h - 1, radius, Palette::Text,
            focused ? 0.9f : 0.08f, focused ? 2.0f : 1.0f);
}

void UiPainter::PushClip(float x, float y, float w, float h) {
    surface->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_ALIASED);
}

void UiPainter::PopClip() {
    surface->PopAxisAlignedClip();
}

void UiPainter::ManagementSurface(float x, float y, float w, float h, float footer) {
    Round(x, y, w, h, 24, Palette::Canvas);
    Round(x + 40, y + footer - 12, w - 80, 1, 0, Palette::Graphite);
}

void UiPainter::RowFrame(float x, float y, float width, float height, bool active, UiIcon icon) {
    // Idle rows sit flush with a hairline under them; the focused one lifts into a white 8% bar.
    if (active) {
        const float grow = width * 0.005f;
        Round(x - grow, y - grow * height / width, width + 2 * grow, height + 2 * grow * height / width,
              12, 0xffffff, 0.08f);
    } else {
        Round(x + 16, y + height - 1, width - 32, 1, 0, Palette::Graphite);
    }
    Icon(icon, x + 20, y + (height - 32) / 2, active ? Palette::Text : Palette::TextSecondary);
}

void UiPainter::PageHeader(float x, float y, UiIcon icon, std::wstring_view title,
                           std::wstring_view subtitle) {
    Round(x, y + 5, 48, 48, 24, Palette::Graphite);
    Outline(x + 0.5f, y + 5.5f, 47, 47, 23.5f, 0xffffff, 0.12f);
    Icon(icon, x + 12, y + 17, Palette::Text, 24);
    Type(title, TypeRole::HeadlineLarge, x + 66, y - 2, 850, Palette::Text);
    Type(subtitle, TypeRole::Body, x + 68, y + 47, 790, Palette::TextSecondary);
}

void UiPainter::Switch(float x, float y, bool on, bool active) {
    Round(x, y, 52, 28, 14, on ? Palette::Text : Palette::Graphite);
    if (!on) Outline(x + 0.5f, y + 0.5f, 51, 27, 13.5f, 0xffffff, active ? 0.45f : 0.15f);
    const float knob = on ? x + 38 : x + 14;
    brush->SetColor(D2D1::ColorF(on ? Palette::OnAccent : Palette::Text));
    surface->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y + 14), 10, 10), brush.Get());
}

void UiPainter::ButtonGlyph(std::wstring_view letters, float x, float y, unsigned color) {
    // The 32 px box callers lay out around; the badge itself is the design's 22 px.
    if (letters.size() > 1) {
        Badge(letters, x + 19, y + 16, color);
        return;
    }
    Badge(letters, x + 16, y + 16, color);
}

float UiPainter::Prompt(std::wstring_view letters, unsigned color, std::wstring_view label, float x,
                        float y) {
    // The 32 px line callers lay out on; the badge itself is the design's 22 px.
    const float badge = BadgeWidth(letters);
    Badge(letters, x + badge / 2, y + 16, color);
    const float width = MeasureType(label, TypeRole::LabelLarge);
    Type(label, TypeRole::LabelLarge, x + badge + 8, y + 6, width + 4, Palette::TextSecondary);
    return badge + 8 + width + 28;
}

} // namespace Jadyx
