// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/ui/ui_painter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <dwrite_1.h>
#include <winrt/base.h>

#include "jadyx/i18n/localization.h"
#include "jadyx/ui/ui_fonts.h"

namespace Jadyx {
using Microsoft::WRL::ComPtr;

namespace {

constexpr unsigned SweepFrames = 120;

} // Anonymous namespace

void UiPainter::Attach(ID2D1RenderTarget* target, IDWriteFactory* write_) {
    surface = target;
    write = write_;
    brush.Reset();
    focus_ribbon.Reset();
    layouts.clear();
    winrt::check_hresult(surface->CreateSolidColorBrush(Ink(Palette::Text), &brush));
}

void UiPainter::SyncLanguage() {
    const auto tag = I18n::CurrentUiLanguage().tag;
    if (tag != format_locale || theme_id != UiTheme()) {
        theme_id = UiTheme();
        focus_ribbon.Reset();
        formats.clear();
        role_formats.clear();
        layouts.clear();
        format_locale = std::wstring{tag};
    }
    // Texts not drawn for a couple of seconds' worth of frames are laid out again if they return.
    if (++layout_frame % SweepFrames == 0)
        std::erase_if(layouts, [&](const auto& entry) { return layout_frame - entry.second.frame > SweepFrames; });
}

D2D1_COLOR_F UiPainter::Ink(unsigned color, float alpha) const {
    const auto resolved = Theme().Color(color);
    return D2D1::ColorF(resolved.rgb, resolved.alpha * alpha);
}

void UiPainter::Round(float x, float y, float w, float h, float radius, unsigned color, float alpha) {
    brush->SetColor(Ink(color, alpha));
    const auto rect = D2D1::RectF(x, y, x + w, y + h);
    if (radius <= 0) surface->FillRectangle(rect, brush.Get());
    else surface->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
}

void UiPainter::Outline(float x, float y, float w, float h, float radius, unsigned color, float alpha,
                        float stroke) {
    brush->SetColor(Ink(color, alpha));
    surface->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius),
                                  brush.Get(), stroke);
}

void UiPainter::Card(float x, float y, float w, float h, float radius) {
    Round(x, y, w, h, radius, Palette::Panel);
    Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, w - Canvas::Unit1, h - Canvas::Unit1,
            radius, Palette::White, Canvas::Unit0_08F);
}

IDWriteTextLayout* UiPainter::TypeLayout(std::wstring_view text, TypeRole role, float width,
                                     TextAlign align, unsigned lines) {
    const auto index = static_cast<unsigned>(role);
    const auto& style = Theme().type[index];
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
        const auto typeface = Theme().system_font
                                  ? SystemInterfaceFonts(write.Get(), style.monospace)
                                  : InterfaceFonts(write.Get());
        winrt::check_hresult(write->CreateTextFormat(
            typeface.family, typeface.collection, static_cast<DWRITE_FONT_WEIGHT>(style.weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.size, format_locale.c_str(),
            &format));
        if (style.uniform_lines)
            format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style.line,
                                   style.line * 0.8f);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        winrt::check_hresult(write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
        format->SetTrimming(&trim, ellipsis.Get());
        role_lines[index] = style.line;
        if (!style.uniform_lines) {
            ComPtr<IDWriteTextLayout> sample;
            if (SUCCEEDED(write->CreateTextLayout(L"Ag", 2, format.Get(), 4096, 4096, &sample))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(sample->GetMetrics(&metrics)))
                    role_lines[index] = metrics.height;
            }
        }
    }
    const float height = role_lines[index] * static_cast<float>(std::max(lines, Canvas::Unit1U));
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
    if (text.empty() || width < 1)
        return Canvas::Unit0;
    auto* layout = TypeLayout(text, role, width, align, lines);
    brush->SetColor(Ink(color, alpha));
    surface->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::min(metrics.height, role_lines[static_cast<size_t>(role)] *
                                        static_cast<float>(std::max(lines, Canvas::Unit1U)));
}

float UiPainter::MeasureType(std::wstring_view text, TypeRole role) {
    if (text.empty())
        return Canvas::Unit0;
    DWRITE_TEXT_METRICS metrics{};
    TypeLayout(text, role, 4096, TextAlign::Leading, 1)->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void UiPainter::FocusGlow(float x, float y, float w, float h, float radius, float amount) {
    if (amount <= 0.001f) return;
    if (Theme().ribbon_focus) {
        if (!focus_ribbon) {
            std::array<D2D1_GRADIENT_STOP, 4> stops{};
            for (std::size_t i = 0; i < stops.size(); ++i) {
                stops[i].position = Theme().ribbon_stops[i];
                stops[i].color = D2D1::ColorF(Theme().ribbon[i].rgb);
            }
            ComPtr<ID2D1GradientStopCollection> collection;
            if (SUCCEEDED(surface->CreateGradientStopCollection(
                    stops.data(), static_cast<UINT32>(stops.size()), &collection)))
                (void)surface->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F()),
                    collection.Get(), &focus_ribbon);
        }
        // NXbox widgets.cpp: a 100-degree ribbon, 6px outer gap and 3px band at 1080p.
        const float gap = Theme().focus_gap, stroke = Theme().focus_stroke;
        const float sine = Theme().focus_sin, cosine = Theme().focus_cos;
        const float half = (std::abs((w + Canvas::Unit2 * gap) * sine) +
                            std::abs((h + Canvas::Unit2 * gap) * cosine)) /
                           Canvas::Unit2;
        if (focus_ribbon) {
            focus_ribbon->SetStartPoint(
                D2D1::Point2F(x + w / 2 - sine * half, y + h / 2 + cosine * half));
            focus_ribbon->SetEndPoint(
                D2D1::Point2F(x + w / 2 + sine * half, y + h / 2 - cosine * half));
            focus_ribbon->SetOpacity(amount);
            const float inset = gap - stroke / Canvas::Unit2;
            const float ring_radius = std::max(radius - stroke / Canvas::Unit2, Canvas::Unit0_0F);
            surface->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(x - inset, y - inset, x + w + inset, y + h + inset),
                                  ring_radius, ring_radius),
                focus_ribbon.Get(), stroke);
        } else {
            Outline(x - gap, y - gap, w + Canvas::Unit2 * gap, h + Canvas::Unit2 * gap, radius,
                    Theme().ribbon[0].rgb, amount, stroke);
        }
        return;
    }
    // Two diffuse drops (0 24 48 -12 at 65% and 0 8 16 -4 at 45%), as stacked translucent fills.
    for (int i = 0; i < 6; ++i) {
        const float spread = Canvas::Unit4_0F + i * Canvas::Unit5_0F;
        Round(x - spread + Canvas::Unit12, y + Canvas::Unit18 - spread + Canvas::Unit12,
              w + Canvas::Unit2 * spread - Canvas::Unit24,
              h + Canvas::Unit2 * spread - Canvas::Unit24, radius + spread, Palette::Black,
              Canvas::Unit0_07F * amount);
    }
    for (int i = 0; i < 3; ++i) {
        const float spread = Canvas::Unit1_0F + i * Canvas::Unit3_0F;
        Round(x - spread + Canvas::Unit4, y + Canvas::Unit6 - spread + Canvas::Unit4,
              w + Canvas::Unit2 * spread - Canvas::Unit8,
              h + Canvas::Unit2 * spread - Canvas::Unit8, radius + spread, Palette::Black,
              Canvas::Unit0_12F * amount);
    }
    // The halo: white 15% fading over 24 px, then the 1.5 px ring at 80%.
    for (int i = 0; i < 4; ++i) {
        const float spread = Canvas::Unit3_0F + i * Canvas::Unit5_0F;
        Outline(x - spread, y - spread, w + Canvas::Unit2 * spread, h + Canvas::Unit2 * spread,
                radius + spread, Palette::White,
                Canvas::Unit0_04F * amount * (Canvas::Unit1_0F - i / Canvas::Unit4_0F),
                Canvas::Unit5);
    }
    Outline(x - Canvas::Unit1_5F, y - Canvas::Unit1_5F, w + Canvas::Unit3, h + Canvas::Unit3,
            radius + Canvas::Unit1_5F, Palette::White, Canvas::Unit0_82F * amount,
            Canvas::Unit1_5F);
}

float UiPainter::BadgeWidth(std::wstring_view letters) {
    return letters.size() > Canvas::Unit1
               ? std::max(Canvas::Unit22_0F, MeasureType(letters, TypeRole::Label) + Canvas::Unit14)
               : Canvas::Unit22_0F;
}
float UiPainter::Badge(std::wstring_view letters, float cx, float cy, unsigned color) {
    const float width = BadgeWidth(letters);
    Round(cx - width / Canvas::Unit2, cy - Canvas::Unit11, width, Canvas::Unit22, Canvas::Unit11,
          Palette::Graphite);
    Outline(cx - width / Canvas::Unit2 + Canvas::Unit0_5F, cy - Canvas::Unit10_5F,
            width - Canvas::Unit1, Canvas::Unit21, Canvas::Unit10_5F, Palette::White,
            Canvas::Unit0_15F);
    Type(letters, TypeRole::Label, cx - width / Canvas::Unit2, cy - Canvas::Unit8, width, color,
         Canvas::Unit1, TextAlign::Center);
    return width;
}
float UiPainter::PillWidth(std::wstring_view label, std::wstring_view letters) {
    const float badge = letters.empty() ? Canvas::Unit0_0F : Canvas::Unit12 + BadgeWidth(letters);
    return Canvas::Unit28 + MeasureType(label, TypeRole::PillLabel) + badge + Canvas::Unit22;
}
float UiPainter::PillButton(std::wstring_view label, std::wstring_view letters, unsigned letters_color,
                            float x, float y, bool primary, float height) {
    const float text = MeasureType(label, TypeRole::PillLabel);
    const float width = PillWidth(label, letters);
    if (primary) {
        // A soft white lift under the primary action.
        if (Theme().primary_lift)
            Round(x - Canvas::Unit2, y + Canvas::Unit2, width + Canvas::Unit4,
                  height + Canvas::Unit4, (height + Canvas::Unit4) / Canvas::Unit2, Palette::White,
                  Canvas::Unit0_06F);
        Round(x, y, width, height, height / Canvas::Unit2, Palette::Accent);
    } else {
        Round(x, y, width, height, height / Canvas::Unit2, Palette::White,
              Theme().secondary_pill_opacity);
        Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, width - Canvas::Unit1,
                height - Canvas::Unit1, (height - Canvas::Unit1) / Canvas::Unit2, Palette::White,
                Canvas::Unit0_08F);
    }
    Type(label, TypeRole::PillLabel, x + Canvas::Unit28,
         y + (height - role_lines[static_cast<std::size_t>(TypeRole::PillLabel)]) / Canvas::Unit2,
         text + Canvas::Unit2, primary ? Palette::OnAccent : Palette::Text);
    // The button that presses it, as everywhere: a dark badge, even on the white pill.
    if (!letters.empty())
        Badge(letters,
              x + Canvas::Unit28 + text + Canvas::Unit12 + BadgeWidth(letters) / Canvas::Unit2,
              y + height / Canvas::Unit2, letters_color);
    return width;
}
float UiPainter::Chip(std::wstring_view text, float x, float y, unsigned color, float alpha) {
    const float width = MeasureType(text, TypeRole::Label) + Canvas::Unit24;
    Round(x, y, width, Canvas::Unit26, Canvas::Unit13, Palette::White,
          Theme().chip_opacity * alpha);
    Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, width - Canvas::Unit1, Canvas::Unit25,
            Canvas::Unit12_5F, Palette::White, Canvas::Unit0_10F * alpha);
    Type(text, TypeRole::Label, x + Canvas::Unit12, y + Canvas::Unit5, width - Canvas::Unit22,
         color, alpha);
    return width;
}
IDWriteTextFormat* UiPainter::Format(float size, bool bold, bool wrap, bool center, bool trailing) {
    const unsigned key = static_cast<unsigned>(size) * 16 + (bold ? 1 : 0) + (wrap ? 2 : 0) +
                         (center ? 4 : 0) + (trailing ? 8 : 0);
    auto& format = formats[key];
    if (!format) {
        const auto typeface = Theme().system_font ? SystemInterfaceFonts(write.Get(), false)
                                                  : InterfaceFonts(write.Get());
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
    brush->SetColor(Ink(color));
    surface->DrawText(text.data(), static_cast<UINT32>(text.size()),
                      Format(size, bold, wrap, center, trailing),
                      D2D1::RectF(x, y, x + width, y + height), brush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float UiPainter::MeasureText(std::wstring_view text, float size, bool bold) {
    if (text.empty())
        return Canvas::Unit0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                       Format(size, bold, false, false, false), 4096, 100, &layout)))
        return Canvas::Unit0;
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
    brush->SetColor(Ink(color));
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
        surface->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + Canvas::Unit1, y + Canvas::Unit7, x + Canvas::Unit31,
                                          y + Canvas::Unit27),
                              Canvas::Unit7, Canvas::Unit7),
            brush.Get(), 2);
        line(7, 17, 15, 17); line(11, 13, 11, 21);
        circle(23, 14, 1.4f); circle(26, 20, 1.4f);
        break;
    case UiIcon::Key:
        circle(10, 12, 7); circle(10, 12, 2);
        line(15, 17, 28, 30); line(22, 24, 26, 20); line(26, 28, 30, 24);
        break;
    case UiIcon::Firmware:
        surface->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + Canvas::Unit7, y + Canvas::Unit7, x + Canvas::Unit25,
                                          y + Canvas::Unit25),
                              Canvas::Unit3, Canvas::Unit3),
            brush.Get(), 2);
        surface->DrawRectangle(D2D1::RectF(x + Canvas::Unit12, y + Canvas::Unit12,
                                           x + Canvas::Unit20, y + Canvas::Unit20),
                               brush.Get(), 2);
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
        surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + Canvas::Unit16, y + Canvas::Unit16),
                                           Canvas::Unit5, Canvas::Unit12),
                             brush.Get(), 2);
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
        surface->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + Canvas::Unit3, y + Canvas::Unit3, x + Canvas::Unit29,
                                          y + Canvas::Unit29),
                              Canvas::Unit6, Canvas::Unit6),
            brush.Get(), 2);
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
        surface->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + Canvas::Unit6, y + Canvas::Unit3, x + Canvas::Unit26,
                                          y + Canvas::Unit29),
                              Canvas::Unit3, Canvas::Unit3),
            brush.Get(), 2);
        for (float p : {6.0f, 12.0f}) { line(14, p, 16, p); line(16, p + 3, 18, p + 3); }
        surface->DrawRectangle(D2D1::RectF(x + Canvas::Unit13, y + Canvas::Unit18,
                                           x + Canvas::Unit19, y + Canvas::Unit25),
                               brush.Get(), 2);
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
        surface->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + Canvas::Unit2, y + Canvas::Unit9, x + Canvas::Unit30,
                                          y + Canvas::Unit28),
                              Canvas::Unit4, Canvas::Unit4),
            brush.Get(), 2);
        line(10, 9, 12, 5); line(12, 5, 20, 5); line(20, 5, 22, 9);
        circle(16, 18, 5.5f);
        break;
    case UiIcon::Display:
        // A screen on its stand.
        surface->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + Canvas::Unit2, y + Canvas::Unit5, x + Canvas::Unit30,
                                          y + Canvas::Unit23),
                              Canvas::Unit3, Canvas::Unit3),
            brush.Get(), 2);
        line(16, 23, 16, 28); line(10, 28, 22, 28);
        break;
    case UiIcon::Exit:
        // An open door frame and the arrow leaving through it.
        line(18, 5, 6, 5); line(6, 5, 6, 27); line(6, 27, 18, 27);
        line(13, 16, 29, 16); line(29, 16, 24, 11); line(29, 16, 24, 21);
        break;
    }
}

void UiPainter::Chevron(float cx, float cy, unsigned color, bool left) {
    brush->SetColor(Ink(color));
    const float d = left ? -Canvas::Unit1_0F : Canvas::Unit1_0F;
    surface->DrawLine(D2D1::Point2F(cx - 4 * d, cy - 5), D2D1::Point2F(cx + 1 * d, cy), brush.Get(), 2);
    surface->DrawLine(D2D1::Point2F(cx + 1 * d, cy), D2D1::Point2F(cx - 4 * d, cy + 5), brush.Get(), 2);
}

void UiPainter::MoreBelow(float cx, float cy, float alpha) {
    brush->SetColor(Ink(Palette::TextSecondary, alpha));
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
        brush->SetColor(Ink(available ? Palette::TextSecondary : Palette::Track));
        const float tip = cy + (up ? -Canvas::Unit3_0F : Canvas::Unit3_0F);
        const float base = cy + (up ? Canvas::Unit3_0F : -Canvas::Unit3_0F);
        surface->DrawLine(D2D1::Point2F(x - 4, base), D2D1::Point2F(x, tip), brush.Get(), 2);
        surface->DrawLine(D2D1::Point2F(x, tip), D2D1::Point2F(x + 4, base), brush.Get(), 2);
    };
    arrow(y + 5, true, above);
    arrow(y + height - 5, false, below);
    const float track_y = y + Canvas::Unit18, track_h = height - Canvas::Unit36;
    const float fraction =
        count ? std::min(Canvas::Unit1_0F, static_cast<float>(visible) / count) : Canvas::Unit1_0F;
    const float thumb_h = std::min(track_h, std::max(Canvas::Unit20_0F, track_h * fraction));
    const float position =
        count > visible ? static_cast<float>(first) / (count - visible) : Canvas::Unit0_0F;
    Round(x - Canvas::Unit1_5F, track_y, Canvas::Unit3, track_h, Canvas::Unit1_5F,
          Palette::Graphite);
    Round(x - Canvas::Unit2, track_y + (track_h - thumb_h) * position, Canvas::Unit4, thumb_h,
          Canvas::Unit2, Palette::Text, Canvas::Unit0_85F);
}

void UiPainter::SelectionCard(float x, float y, float w, float h, bool focused, float radius) {
    Round(x, y, w, h, radius, focused ? Palette::RowActive : Palette::Surface);
    Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, w - Canvas::Unit1, h - Canvas::Unit1,
            radius, Palette::Text, focused ? Canvas::Unit0_9F : Canvas::Unit0_08F,
            focused ? Canvas::Unit2_0F : Canvas::Unit1_0F);
}

void UiPainter::PushClip(float x, float y, float w, float h) {
    surface->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_ALIASED);
}

void UiPainter::PopClip() {
    surface->PopAxisAlignedClip();
}

void UiPainter::ManagementSurface(float x, float y, float w, float h, float footer) {
    Round(x, y, w, h, Canvas::Unit24, Palette::Canvas);
    Round(x + Canvas::Unit40, y + footer - Canvas::Unit12, w - Canvas::Unit80, Canvas::Unit1,
          Canvas::Unit0, Palette::Graphite);
}

void UiPainter::RowFrame(float x, float y, float width, float height, bool active, UiIcon icon) {
    // Idle rows sit flush with a hairline under them; the focused one lifts into a white 8% bar.
    if (active) {
        const float grow = width * Canvas::Unit0_005F;
        Round(x - grow, y - grow * height / width, width + Canvas::Unit2 * grow,
              height + Canvas::Unit2 * grow * height / width, Canvas::Unit12, Palette::White,
              Canvas::Unit0_08F);
    } else {
        Round(x + Canvas::Unit16, y + height - Canvas::Unit1, width - Canvas::Unit32, Canvas::Unit1,
              Canvas::Unit0, Palette::Graphite);
    }
    Icon(icon, x + Canvas::Unit20, y + (height - Canvas::Unit32) / Canvas::Unit2,
         active ? Palette::Text : Palette::TextSecondary);
}

void UiPainter::PageHeader(float x, float y, UiIcon icon, std::wstring_view title,
                           std::wstring_view subtitle) {
    Round(x, y + Canvas::Unit5, Canvas::Unit48, Canvas::Unit48, Canvas::Unit24, Palette::Graphite);
    Outline(x + Canvas::Unit0_5F, y + Canvas::Unit5_5F, Canvas::Unit47, Canvas::Unit47,
            Canvas::Unit23_5F, Palette::White, Canvas::Unit0_12F);
    Icon(icon, x + Canvas::Unit12, y + Canvas::Unit17, Palette::Text, Canvas::Unit24);
    Type(title, TypeRole::HeadlineLarge, x + Canvas::Unit66, y - Canvas::Unit2, Canvas::Unit850,
         Palette::Text);
    Type(subtitle, TypeRole::Body, x + Canvas::Unit68, y + Canvas::Unit47, Canvas::Unit790,
         Palette::TextSecondary);
}

void UiPainter::Switch(float x, float y, bool on, bool active) {
    Round(x, y, Canvas::Unit52, Canvas::Unit28, Canvas::Unit14,
          on ? Palette::Text : Palette::Graphite);
    if (!on)
        Outline(x + Canvas::Unit0_5F, y + Canvas::Unit0_5F, Canvas::Unit51, Canvas::Unit27,
                Canvas::Unit13_5F, Palette::White, active ? Canvas::Unit0_45F : Canvas::Unit0_15F);
    const float knob = on ? x + Canvas::Unit38 : x + Canvas::Unit14;
    brush->SetColor(Ink(on ? Palette::OnAccent : Palette::Text));
    surface->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(knob, y + Canvas::Unit14), Canvas::Unit10, Canvas::Unit10),
        brush.Get());
}

void UiPainter::ButtonGlyph(std::wstring_view letters, float x, float y, unsigned color) {
    // The 32 px box callers lay out around; the badge itself is the design's 22 px.
    if (letters.size() > 1) {
        Badge(letters, x + Canvas::Unit19, y + Canvas::Unit16, color);
        return;
    }
    Badge(letters, x + Canvas::Unit16, y + Canvas::Unit16, color);
}

float UiPainter::Prompt(std::wstring_view letters, unsigned color, std::wstring_view label, float x,
                        float y) {
    // The 32 px line callers lay out on; the badge itself is the design's 22 px.
    const float badge = BadgeWidth(letters);
    Badge(letters, x + badge / Canvas::Unit2, y + Canvas::Unit16, color);
    const float width = MeasureType(label, TypeRole::ControlLabel);
    Type(label, TypeRole::ControlLabel, x + badge + Canvas::Unit8, y + Canvas::Unit6,
         width + Canvas::Unit4, Palette::TextSecondary);
    return badge + Canvas::Unit8 + width + Canvas::Unit28;
}

} // namespace Jadyx
