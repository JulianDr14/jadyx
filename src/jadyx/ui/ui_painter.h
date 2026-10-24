// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The frontend's drawing vocabulary on a Direct2D render target: rounded panels, text in the
// interface language, the vector icons, rows, page headers, switches and button glyphs. The
// library canvas draws on its swapchain with it, and the in-game menu on a CPU bitmap, so both
// look the same from one implementation.

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "jadyx/ui/ui_icon.h"

namespace Jadyx {

/// The frontend's colors (0xRRGGBB): the monochrome "living room cinema" palette. Colour on
/// screen comes from game art; the chrome stays neutral.
namespace Palette {
inline constexpr unsigned Canvas = 0x08090b;   ///< The base layer, behind everything.
inline constexpr unsigned Surface = 0x111315;  ///< Cards, panels and dialogs.
inline constexpr unsigned Graphite = 0x1c1f23; ///< Hairlines, dividers, tracks, badges.
inline constexpr unsigned Backdrop = Canvas;
inline constexpr unsigned Panel = Surface;
inline constexpr unsigned Row = 0x16181b;
/// The focused row: white at 8% over Surface.
inline constexpr unsigned RowActive = 0x26282b;
/// Focus and "on": the focal white, with Canvas on top of it.
inline constexpr unsigned Accent = 0xf5f5f7;
inline constexpr unsigned OnAccent = 0x08090b;
inline constexpr unsigned Text = 0xf5f5f7;
inline constexpr unsigned TextSecondary = 0x92959a;
inline constexpr unsigned TextHint = 0xb4b7bb;
inline constexpr unsigned CompatibilityPlayable = 0x73d69b;
inline constexpr unsigned CompatibilityBroken = 0xf08b8b;
inline constexpr unsigned Warning = 0xf0c27a;
inline constexpr unsigned Track = Graphite;
} // namespace Palette

/// The type scale, in 1280x720 layout units (the design's CSS pixels).
enum class TypeRole : unsigned {
    Hero,           ///< 56/64 semibold, the focused game's title.
    HeadlineLarge,  ///< 40/48 semibold, page titles.
    HeadlineMedium, ///< 28/36 medium.
    HeadlineSmall,  ///< 22/28 medium, section titles.
    BodyLarge,      ///< 18/26.
    Body,           ///< 15/22.
    LabelLarge,     ///< 14/20 medium.
    Label,          ///< 12/16 medium, tabular figures.
    Hint,           ///< 13/18 semibold, controller prompts.
    Caps,           ///< 12/16 semibold, widely tracked: the kicker over a title (pass it upper-cased).
    Count,
};

enum class TextAlign : unsigned { Leading, Center, Trailing };

/// Semantic targets emitted at the same place and size that a manager draws its controls.
enum class UiTarget : unsigned { ManagerTab, ManagerRow, ManagerCard };

class UiPainter {
public:
    virtual ~UiPainter() = default;
    virtual void Target(UiTarget kind, std::size_t index, float x, float y, float w, float h) {
        (void)kind, (void)index, (void)x, (void)y, (void)w, (void)h;
    }

    /// Draws on `target` from now on; texts are laid out by `write`.
    void Attach(ID2D1RenderTarget* target, IDWriteFactory* write);
    /// Text formats follow the interface language; call once per frame.
    void SyncLanguage();

    void Round(float x, float y, float w, float h, float radius, unsigned color, float alpha = 1);
    /// A rounded hairline (stroke centred on the rectangle's edge).
    void Outline(float x, float y, float w, float h, float radius, unsigned color, float alpha,
                 float stroke = 1);
    /// A dialog or card surface: Surface with a white hairline at 8%.
    void Card(float x, float y, float w, float h, float radius = 16);
    /// Game-management cards share a quiet surface and a thin white focus ring.
    void SelectionCard(float x, float y, float w, float h, bool focused, float radius = 16);
    /// Clips what is drawn to the rectangle until PopClip (lists that scroll under a fixed edge).
    void PushClip(float x, float y, float w, float h);
    void PopClip();
    /// Page chrome shared by mods, cheats and installed content; coordinates stay local.
    void ManagementSurface(float x, float y, float w, float h, float footer);
    /// Text in the type scale, clipped to `width` and at most `lines` lines (ellipsis after).
    /// Returns the height it took.
    float Type(std::wstring_view text, TypeRole role, float x, float y, float width, unsigned color,
               float alpha = 1, TextAlign align = TextAlign::Leading, unsigned lines = 1);
    float MeasureType(std::wstring_view text, TypeRole role);
    /// The tvOS focus: a diffuse shadow, a white ring and a faint halo around the rectangle,
    /// `amount` (0..1) of the way in.
    void FocusGlow(float x, float y, float w, float h, float radius, float amount);
    /// A capsule button: white with dark text when primary, graphite otherwise. A badge with the
    /// button that presses it sits at its end. Returns its width.
    float PillButton(std::wstring_view label, std::wstring_view letters, unsigned letters_color,
                     float x, float y, bool primary, float height = 44);
    float PillWidth(std::wstring_view label, std::wstring_view letters);
    /// A quiet capsule of metadata (a version, a play time); returns its width.
    float Chip(std::wstring_view text, float x, float y, unsigned color, float alpha = 1);
    /// A controller button badge: a 22 px graphite circle (a capsule for longer names) with a
    /// hairline, centred on (cx, cy); the letters take `color`. Returns its width.
    float Badge(std::wstring_view letters, float cx, float cy, unsigned color);
    float BadgeWidth(std::wstring_view letters);
    void Text(std::wstring_view text, float x, float y, float size, unsigned color, float width,
              float height = 40, bool bold = false, bool wrap = false, bool center = false,
              bool trailing = false);
    float MeasureText(std::wstring_view text, float size, bool bold);
    /// Native vector strokes: no texture loads, font glyphs or geometry allocation.
    /// Draws `icon` in a `size` box (the icons are drawn for 32).
    void Icon(UiIcon icon, float x, float y, unsigned color, float size = 32);
    /// The ">" at the end of a row that opens a page.
    void Chevron(float cx, float cy, unsigned color, bool left = false);
    /// A small "v" telling that a page goes on below what it shows.
    void MoreBelow(float cx, float cy, float alpha = 1);
    /// A selectable row: background, accent bar when active and the icon in its tile.
    /// Passive list position: track/thumb and available directions, hidden when everything fits.
    void ScrollIndicator(float x, float y, float height, std::size_t first,
                         std::size_t visible, std::size_t count, bool more_below = false);
    void RowFrame(float x, float y, float width, float height, bool active, UiIcon icon);
    void PageHeader(float x, float y, UiIcon icon, std::wstring_view title, std::wstring_view subtitle);
    /// An on/off switch, 52x28 at (x, y).
    void Switch(float x, float y, bool on, bool active);
    /// A controller button drawn natively: a ring with its letters (A, B, X, Y, LB, RB...).
    void ButtonGlyph(std::wstring_view letters, float x, float y, unsigned color);
    /// A button glyph and its label; returns the width it took.
    float Prompt(std::wstring_view letters, unsigned color, std::wstring_view label, float x, float y);
    /// The picture in `file`, cropped to fill the rectangle, with rounded corners. False when this
    /// surface draws no pictures or the file cannot be decoded: the caller draws a placeholder.
    virtual bool Picture(const std::filesystem::path& file, float x, float y, float w, float h) {
        (void)file, (void)x, (void)y, (void)w, (void)h;
        return false;
    }

protected:
    IDWriteTextFormat* Format(float size, bool bold, bool wrap, bool center, bool trailing);
    IDWriteTextLayout* TypeLayout(std::wstring_view text, TypeRole role, float width, TextAlign align,
                              unsigned lines);

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> surface;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<IDWriteFactory> write;

private:
    struct CachedLayout {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        unsigned frame;
    };

    std::wstring format_locale{L"en"};
    std::unordered_map<unsigned, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats;
    std::unordered_map<unsigned, Microsoft::WRL::ComPtr<IDWriteTextFormat>> role_formats;
    /// Laid-out texts by content and box: a redraw lays out only what changed. Entries unused for
    /// a while are dropped at SyncLanguage.
    std::unordered_map<std::wstring, CachedLayout> layouts;
    unsigned layout_frame{};
};

/// Colors of the Xbox face buttons, for ButtonGlyph.
namespace ButtonColor {
inline constexpr unsigned A = 0x7ccd4f;
inline constexpr unsigned B = 0xf16b5f;
inline constexpr unsigned X = 0x68b7ff;
inline constexpr unsigned Y = 0xf5d76e;
inline constexpr unsigned Neutral = 0xf5f5f7;
} // namespace ButtonColor

} // namespace Jadyx
