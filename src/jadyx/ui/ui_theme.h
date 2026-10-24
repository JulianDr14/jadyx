// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Jadyx {

// Stable LocalSettings values. Unknown/newer preferences fall back to today's appearance.
enum class ThemeId : std::int32_t { Jadyx = 0, OldNxbox = 1 };
constexpr std::int32_t ThemeValue(ThemeId id) {
    return static_cast<std::int32_t>(id);
}
constexpr ThemeId ThemeFromValue(std::int32_t value) {
    return value == ThemeValue(ThemeId::OldNxbox) ? ThemeId::OldNxbox : ThemeId::Jadyx;
}

inline constexpr std::wstring_view ThemeSettingKey = L"system_theme";
template <typename Read>
ThemeId LoadThemePreference(Read&& read) {
    return ThemeFromValue(read(ThemeSettingKey).value_or(ThemeValue(ThemeId::Jadyx)));
}
template <typename Write>
void SaveThemePreference(ThemeId id, Write&& write) {
    write(ThemeSettingKey, ThemeValue(id));
}

// Semantic color handles, deliberately outside RGB's range. UiPainter resolves them per surface;
// literal RGB values (for example colors extracted from artwork) remain valid.
namespace Palette {
inline constexpr unsigned First = 0x1000000;
inline constexpr unsigned Canvas = First + 0;
inline constexpr unsigned Surface = First + 1;
inline constexpr unsigned Graphite = First + 2;
inline constexpr unsigned Backdrop = First + 3;
inline constexpr unsigned Panel = First + 4;
inline constexpr unsigned Row = First + 5;
inline constexpr unsigned RowActive = First + 6;
inline constexpr unsigned Accent = First + 7;
inline constexpr unsigned OnAccent = First + 8;
inline constexpr unsigned Text = First + 9;
inline constexpr unsigned TextSecondary = First + 10;
inline constexpr unsigned TextHint = First + 11;
inline constexpr unsigned Warning = First + 12;
inline constexpr unsigned Track = First + 13;
inline constexpr unsigned White = First + 14;
inline constexpr unsigned Black = First + 15;
inline constexpr unsigned FocusSurface = First + 16;
inline constexpr unsigned ControllerOutline = First + 17;
inline constexpr unsigned ButtonA = First + 18;
inline constexpr unsigned ButtonB = First + 19;
inline constexpr unsigned ButtonX = First + 20;
inline constexpr unsigned ButtonY = First + 21;
inline constexpr unsigned ButtonNeutral = First + 22;
inline constexpr unsigned OnAccentSecondary = First + 23;
inline constexpr unsigned TabTrack = First + 24;
inline constexpr unsigned CompatibilityPlayable = First + 25;
inline constexpr unsigned CompatibilityBroken = First + 26;
inline constexpr std::size_t Count = 27;
} // namespace Palette

namespace ButtonColor {
inline constexpr unsigned A = Palette::ButtonA;
inline constexpr unsigned B = Palette::ButtonB;
inline constexpr unsigned X = Palette::ButtonX;
inline constexpr unsigned Y = Palette::ButtonY;
inline constexpr unsigned Neutral = Palette::ButtonNeutral;
} // namespace ButtonColor

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
    Caps, ///< 12/16 semibold, widely tracked: the kicker over a title (pass it upper-cased).
    SettingsTitle,
    SettingsDetail,
    CoverTitle,
    Clock,
    Stat,
    ControlLabel,
    PillLabel,
    Count,
};

struct ThemeColor {
    unsigned rgb{};
    float alpha{1};
};
struct ThemeType {
    float size{};
    float line{};
    unsigned weight{};
    float tracking{};
    bool tabular{};
    bool monospace{};
    bool uniform_lines{true};
};
struct ThemeTokens {
    // The fixed 1280x720 coordinate/spacing scale shared with hit testing. Literal types are
    // intentional: changing integer arithmetic or float precision can shift the default design.
    struct Canvas {
        static constexpr auto Unit0 = 0;
        static constexpr auto Unit0_0F = 0.0f;
        static constexpr auto Unit0_005F = 0.005f;
        static constexpr auto Unit0_04F = 0.04f;
        static constexpr auto Unit0_06F = 0.06f;
        static constexpr auto Unit0_07F = 0.07f;
        static constexpr auto Unit0_0722F = 0.0722f;
        static constexpr auto Unit0_08F = 0.08f;
        static constexpr auto Unit0_10F = 0.10f;
        static constexpr auto Unit0_12F = 0.12f;
        static constexpr auto Unit0_15F = 0.15f;
        static constexpr auto Unit0_16F = 0.16f;
        static constexpr auto Unit0_18F = 0.18f;
        static constexpr auto Unit0_2F = 0.2f;
        static constexpr auto Unit0_2126F = 0.2126f;
        static constexpr auto Unit0_3F = 0.3f;
        static constexpr auto Unit0_4F = 0.4f;
        static constexpr auto Unit0_45F = 0.45f;
        static constexpr auto Unit0_5F = 0.5f;
        static constexpr auto Unit0_55F = 0.55f;
        static constexpr auto Unit0_58F = 0.58f;
        static constexpr auto Unit0_6F = 0.6f;
        static constexpr auto Unit0_62F = 0.62f;
        static constexpr auto Unit0_7F = 0.7f;
        static constexpr auto Unit0_7152F = 0.7152f;
        static constexpr auto Unit0_72F = 0.72f;
        static constexpr auto Unit0_75F = 0.75f;
        static constexpr auto Unit0_78F = 0.78f;
        static constexpr auto Unit0_8F = 0.8f;
        static constexpr auto Unit0_82F = 0.82f;
        static constexpr auto Unit0_85F = 0.85f;
        static constexpr auto Unit0_86F = 0.86f;
        static constexpr auto Unit0_9F = 0.9f;
        static constexpr auto Unit1 = 1;
        static constexpr auto Unit1U = 1U;
        static constexpr auto Unit1_0F = 1.0f;
        static constexpr auto Unit1_2F = 1.2f;
        static constexpr auto Unit1_5F = 1.5f;
        static constexpr auto Unit2 = 2;
        static constexpr auto Unit2_0F = 2.0f;
        static constexpr auto Unit2_5F = 2.5f;
        static constexpr auto Unit3 = 3;
        static constexpr auto Unit3_0F = 3.0f;
        static constexpr auto Unit4 = 4;
        static constexpr auto Unit4_0F = 4.0f;
        static constexpr auto Unit5 = 5;
        static constexpr auto Unit5_0F = 5.0f;
        static constexpr auto Unit5_5F = 5.5f;
        static constexpr auto Unit6 = 6;
        static constexpr auto Unit6_2831853F = 6.2831853f;
        static constexpr auto Unit7 = 7;
        static constexpr auto Unit8 = 8;
        static constexpr auto Unit9 = 9;
        static constexpr auto Unit10 = 10;
        static constexpr auto Unit10_5F = 10.5f;
        static constexpr auto Unit11 = 11;
        static constexpr auto Unit11_5F = 11.5f;
        static constexpr auto Unit12 = 12;
        static constexpr auto Unit12_5F = 12.5f;
        static constexpr auto Unit13 = 13;
        static constexpr auto Unit13_5F = 13.5f;
        static constexpr auto Unit14 = 14;
        static constexpr auto Unit14_5F = 14.5f;
        static constexpr auto Unit15 = 15;
        static constexpr auto Unit16 = 16;
        static constexpr auto Unit17 = 17;
        static constexpr auto Unit18 = 18;
        static constexpr auto Unit19 = 19;
        static constexpr auto Unit20 = 20;
        static constexpr auto Unit20_0F = 20.0f;
        static constexpr auto Unit21 = 21;
        static constexpr auto Unit22 = 22;
        static constexpr auto Unit23_5F = 23.5f;
        static constexpr auto Unit24 = 24;
        static constexpr auto Unit25 = 25;
        static constexpr auto Unit26 = 26;
        static constexpr auto Unit27 = 27;
        static constexpr auto Unit28 = 28;
        static constexpr auto Unit29 = 29;
        static constexpr auto Unit30 = 30;
        static constexpr auto Unit31 = 31;
        static constexpr auto Unit32 = 32;
        static constexpr auto Unit34 = 34;
        static constexpr auto Unit36 = 36;
        static constexpr auto Unit38 = 38;
        static constexpr auto Unit40 = 40;
        static constexpr auto Unit42 = 42;
        static constexpr auto Unit44 = 44;
        static constexpr auto Unit46 = 46;
        static constexpr auto Unit47 = 47;
        static constexpr auto Unit48 = 48;
        static constexpr auto Unit50 = 50;
        static constexpr auto Unit51 = 51;
        static constexpr auto Unit52 = 52;
        static constexpr auto Unit54 = 54;
        static constexpr auto Unit55 = 55;
        static constexpr auto Unit56 = 56;
        static constexpr auto Unit58 = 58;
        static constexpr auto Unit60 = 60;
        static constexpr auto Unit62 = 62;
        static constexpr auto Unit64 = 64;
        static constexpr auto Unit64_0F = 64.0f;
        static constexpr auto Unit66 = 66;
        static constexpr auto Unit67 = 67;
        static constexpr auto Unit68 = 68;
        static constexpr auto Unit70 = 70;
        static constexpr auto Unit80 = 80;
        static constexpr auto Unit84 = 84;
        static constexpr auto Unit86 = 86;
        static constexpr auto Unit88 = 88;
        static constexpr auto Unit92 = 92;
        static constexpr auto Unit94 = 94;
        static constexpr auto Unit96 = 96;
        static constexpr auto Unit98 = 98;
        static constexpr auto Unit100 = 100;
        static constexpr auto Unit106 = 106;
        static constexpr auto Unit110 = 110;
        static constexpr auto Unit112 = 112;
        static constexpr auto Unit116 = 116;
        static constexpr auto Unit118 = 118;
        static constexpr auto Unit120 = 120;
        static constexpr auto Unit122 = 122;
        static constexpr auto Unit128 = 128;
        static constexpr auto Unit130 = 130;
        static constexpr auto Unit132 = 132;
        static constexpr auto Unit133 = 133;
        static constexpr auto Unit136 = 136;
        static constexpr auto Unit140 = 140;
        static constexpr auto Unit142 = 142;
        static constexpr auto Unit143 = 143;
        static constexpr auto Unit144 = 144;
        static constexpr auto Unit150 = 150;
        static constexpr auto Unit150_0F = 150.0f;
        static constexpr auto Unit153 = 153;
        static constexpr auto Unit155 = 155;
        static constexpr auto Unit156 = 156;
        static constexpr auto Unit160 = 160;
        static constexpr auto Unit166 = 166;
        static constexpr auto Unit168 = 168;
        static constexpr auto Unit170 = 170;
        static constexpr auto Unit172 = 172;
        static constexpr auto Unit174_0F = 174.0f;
        static constexpr auto Unit175 = 175;
        static constexpr auto Unit176 = 176;
        static constexpr auto Unit180 = 180;
        static constexpr auto Unit190 = 190;
        static constexpr auto Unit200 = 200;
        static constexpr auto Unit203 = 203;
        static constexpr auto Unit204 = 204;
        static constexpr auto Unit206 = 206;
        static constexpr auto Unit210 = 210;
        static constexpr auto Unit214 = 214;
        static constexpr auto Unit216 = 216;
        static constexpr auto Unit220 = 220;
        static constexpr auto Unit222 = 222;
        static constexpr auto Unit225 = 225;
        static constexpr auto Unit226 = 226;
        static constexpr auto Unit227 = 227;
        static constexpr auto Unit230 = 230;
        static constexpr auto Unit234 = 234;
        static constexpr auto Unit236 = 236;
        static constexpr auto Unit240 = 240;
        static constexpr auto Unit252 = 252;
        static constexpr auto Unit258 = 258;
        static constexpr auto Unit264 = 264;
        static constexpr auto Unit270 = 270;
        static constexpr auto Unit274 = 274;
        static constexpr auto Unit278 = 278;
        static constexpr auto Unit285 = 285;
        static constexpr auto Unit296 = 296;
        static constexpr auto Unit298 = 298;
        static constexpr auto Unit300 = 300;
        static constexpr auto Unit300_0F = 300.0f;
        static constexpr auto Unit304 = 304;
        static constexpr auto Unit314 = 314;
        static constexpr auto Unit327 = 327;
        static constexpr auto Unit330 = 330;
        static constexpr auto Unit332 = 332;
        static constexpr auto Unit334 = 334;
        static constexpr auto Unit342 = 342;
        static constexpr auto Unit359 = 359;
        static constexpr auto Unit360 = 360;
        static constexpr auto Unit360_0F = 360.0f;
        static constexpr auto Unit366 = 366;
        static constexpr auto Unit376 = 376;
        static constexpr auto Unit379 = 379;
        static constexpr auto Unit380 = 380;
        static constexpr auto Unit400 = 400;
        static constexpr auto Unit404 = 404;
        static constexpr auto Unit414 = 414;
        static constexpr auto Unit430 = 430;
        static constexpr auto Unit436 = 436;
        static constexpr auto Unit450 = 450;
        static constexpr auto Unit460 = 460;
        static constexpr auto Unit474 = 474;
        static constexpr auto Unit480 = 480;
        static constexpr auto Unit487 = 487;
        static constexpr auto Unit490 = 490;
        static constexpr auto Unit500 = 500;
        static constexpr auto Unit510 = 510;
        static constexpr auto Unit512 = 512;
        static constexpr auto Unit515 = 515;
        static constexpr auto Unit520 = 520;
        static constexpr auto Unit522 = 522;
        static constexpr auto Unit530 = 530;
        static constexpr auto Unit540 = 540;
        static constexpr auto Unit551 = 551;
        static constexpr auto Unit560 = 560;
        static constexpr auto Unit564 = 564;
        static constexpr auto Unit570 = 570;
        static constexpr auto Unit574 = 574;
        static constexpr auto Unit579 = 579;
        static constexpr auto Unit584 = 584;
        static constexpr auto Unit586 = 586;
        static constexpr auto Unit590 = 590;
        static constexpr auto Unit594 = 594;
        static constexpr auto Unit600 = 600;
        static constexpr auto Unit603 = 603;
        static constexpr auto Unit609 = 609;
        static constexpr auto Unit611 = 611;
        static constexpr auto Unit614 = 614;
        static constexpr auto Unit620 = 620;
        static constexpr auto Unit635 = 635;
        static constexpr auto Unit637 = 637;
        static constexpr auto Unit640 = 640;
        static constexpr auto Unit640_0F = 640.0f;
        static constexpr auto Unit644 = 644;
        static constexpr auto Unit648 = 648;
        static constexpr auto Unit650 = 650;
        static constexpr auto Unit652 = 652;
        static constexpr auto Unit658 = 658;
        static constexpr auto Unit660 = 660;
        static constexpr auto Unit664 = 664;
        static constexpr auto Unit665 = 665;
        static constexpr auto Unit669 = 669;
        static constexpr auto Unit676 = 676;
        static constexpr auto Unit680 = 680;
        static constexpr auto Unit700 = 700;
        static constexpr auto Unit720 = 720;
        static constexpr auto Unit720_0F = 720.0f;
        static constexpr auto Unit760 = 760;
        static constexpr auto Unit780 = 780;
        static constexpr auto Unit790 = 790;
        static constexpr auto Unit806 = 806;
        static constexpr auto Unit850 = 850;
        static constexpr auto Unit860 = 860;
        static constexpr auto Unit860_0F = 860.0f;
        static constexpr auto Unit900 = 900;
        static constexpr auto Unit990 = 990;
        static constexpr auto Unit1000 = 1000;
        static constexpr auto Unit1010 = 1010;
        static constexpr auto Unit1018 = 1018;
        static constexpr auto Unit1030_0F = 1030.0f;
        static constexpr auto Unit1056 = 1056;
        static constexpr auto Unit1060 = 1060;
        static constexpr auto Unit1080 = 1080;
        static constexpr auto Unit1080_0F = 1080.0f;
        static constexpr auto Unit1120 = 1120;
        static constexpr auto Unit1150 = 1150;
        static constexpr auto Unit1168 = 1168;
        static constexpr auto Unit1187 = 1187;
        static constexpr auto Unit1200 = 1200;
        static constexpr auto Unit1224 = 1224;
        static constexpr auto Unit1280 = 1280;
        static constexpr auto Unit1280_0F = 1280.0f;
        static constexpr auto Unit8_4F = 8.4f;
        static constexpr auto Unit2_4F = 2.4f;
        static constexpr auto Unit11_8F = 11.8f;
        static constexpr auto Unit2_1F = 2.1f;
        static constexpr auto Unit1_05F = 1.05f;
        static constexpr auto Unit4_4F = 4.4f;
        static constexpr auto Unit1_7F = 1.7f;
        static constexpr auto Unit382 = 382;
        static constexpr auto Unit339 = 339;
        static constexpr auto Unit321 = 321;
        static constexpr auto Unit0_92F = 0.92f;
        static constexpr auto Unit22_0F = 22.0f;
        static constexpr auto Unit23 = 23;
    };
    std::array<ThemeColor, Palette::Count> colors{};
    std::array<ThemeType, static_cast<std::size_t>(TypeRole::Count)>
        type{}; // TypeRole order, independent of Windows headers.
    float tab_opacity{0.78f};
    float secondary_pill_opacity{0.10f};
    float chip_opacity{0.08f};
    float settings_row_radius{12};
    float settings_category_radius{14};
    float settings_idle_opacity{0.55f};
    bool settings_groups{true};
    bool primary_lift{true};
    bool system_font{};
    bool ribbon_focus{};
    std::array<ThemeColor, 4> ribbon{};
    std::array<float, 4> ribbon_stops{0, 0.38f, 0.64f, 1};
    float cover_radius{12};
    float cover_mask_radius{1 / 12.0f};
    float focus_gap{};
    float focus_sin{0.98480775f};
    float focus_cos{-0.17364818f};
    float focus_stroke{1.5f};

    constexpr ThemeColor Color(unsigned token) const {
        return token >= Palette::First && token - Palette::First < colors.size()
                   ? colors[token - Palette::First]
                   : ThemeColor{.rgb = token};
    }
};

// Default palette and typography frozen from Jadyx main 0fc5c018.
inline constexpr ThemeTokens JadyxTheme = [] {
    ThemeTokens tokens;
    tokens.colors[0].rgb = 0x08090b;
    tokens.colors[1].rgb = 0x111315;
    tokens.colors[2].rgb = 0x1c1f23;
    tokens.colors[3].rgb = 0x08090b;
    tokens.colors[4].rgb = 0x111315;
    tokens.colors[5].rgb = 0x16181b;
    tokens.colors[6].rgb = 0x26282b;
    tokens.colors[7].rgb = 0xf5f5f7;
    tokens.colors[8].rgb = 0x08090b;
    tokens.colors[9].rgb = 0xf5f5f7;
    tokens.colors[10].rgb = 0x92959a;
    tokens.colors[11].rgb = 0xb4b7bb;
    tokens.colors[12].rgb = 0xf0c27a;
    tokens.colors[13].rgb = 0x1c1f23;
    tokens.colors[14].rgb = 0xffffff;
    tokens.colors[15].rgb = 0x000000;
    tokens.colors[16].rgb = 0x333537;
    tokens.colors[17].rgb = 0x45484d;
    tokens.colors[18].rgb = 0x7ccd4f;
    tokens.colors[19].rgb = 0xf16b5f;
    tokens.colors[20].rgb = 0x68b7ff;
    tokens.colors[21].rgb = 0xf5d76e;
    tokens.colors[22].rgb = 0xf5f5f7;
    tokens.colors[23].rgb = 0x5c5f64;
    tokens.colors[24].rgb = 0x1c1f23;
    tokens.colors[25].rgb = 0x73d69b;
    tokens.colors[26].rgb = 0xf08b8b;
    tokens.type[0] = {.size = 56, .line = 64, .weight = 600, .tracking = -0.025f, .tabular = false};
    tokens.type[1] = {.size = 40, .line = 48, .weight = 600, .tracking = -0.02f, .tabular = false};
    tokens.type[2] = {.size = 28, .line = 36, .weight = 500, .tracking = -0.015f, .tabular = false};
    tokens.type[3] = {.size = 22, .line = 28, .weight = 500, .tracking = -0.01f, .tabular = false};
    tokens.type[4] = {.size = 18, .line = 26, .weight = 400, .tracking = -0.005f, .tabular = false};
    tokens.type[5] = {.size = 15, .line = 22, .weight = 400, .tracking = 0, .tabular = false};
    tokens.type[6] = {.size = 14, .line = 20, .weight = 500, .tracking = 0.01f, .tabular = true};
    tokens.type[7] = {.size = 12, .line = 16, .weight = 500, .tracking = 0.02f, .tabular = true};
    tokens.type[8] = {.size = 13, .line = 18, .weight = 600, .tracking = 0.015f, .tabular = true};
    tokens.type[9] = {.size = 12, .line = 16, .weight = 600, .tracking = 0.2f, .tabular = false};
    tokens.type[10] = tokens.type[6];
    tokens.type[11] = tokens.type[7];
    tokens.type[12] = tokens.type[6];
    tokens.type[13] = tokens.type[6];
    tokens.type[14] = tokens.type[7];
    tokens.type[15] = tokens.type[6];
    tokens.type[16] = tokens.type[4];
    return tokens;
}();

// Ported from NXbox origin/main (7275e402), ui/theme.h and renderer.cpp (GPL-3.0-or-later).
// NXbox uses a 1920x1080 canvas; divide its typography and cover radius by 1.5 for 1280x720.
inline constexpr ThemeTokens OldNxboxTheme = [] {
    auto tokens = JadyxTheme;
    const auto color = [&](unsigned slot, unsigned rgb, float alpha = 1) {
        tokens.colors[slot - Palette::First] = {.rgb = rgb, .alpha = alpha};
    };
    color(Palette::Canvas, 0x000000);
    color(Palette::Backdrop, 0x000000);
    color(Palette::Surface, 0xffffff, 0.055f);
    color(Palette::Panel, 0x141416, 0.94f);
    color(Palette::Graphite, 0xffffff, 0.09f);
    color(Palette::Track, 0xffffff, 0.09f);
    color(Palette::Row, 0xffffff, 0.055f);
    color(Palette::RowActive, 0xffffff, 0.12f);
    color(Palette::FocusSurface, 0xffffff, 0.09f);
    color(Palette::OnAccent, 0x000000);
    color(Palette::TextSecondary, 0xf5f5f7, 0.64f);
    color(Palette::TextHint, 0xf5f5f7, 0.40f);
    color(Palette::Warning, 0xff8e87);
    color(Palette::ButtonA, 0x5ec26a);
    color(Palette::ButtonB, 0xe5534b);
    color(Palette::ButtonX, 0x4f8ff7);
    color(Palette::ButtonY, 0xf2c94c);
    color(Palette::TabTrack, 0x141418, 0.50f);
    tokens.tab_opacity = 1;
    tokens.secondary_pill_opacity = 0.12f;
    tokens.chip_opacity = 0.055f;
    tokens.settings_row_radius = 26;
    tokens.settings_category_radius = 29;
    tokens.settings_idle_opacity = 0;
    tokens.settings_groups = false;
    tokens.primary_lift = false;
    tokens.system_font = true;
    tokens.ribbon_focus = true;
    tokens.cover_radius = 22.0f / 1.5f;
    tokens.cover_mask_radius = 22.0f / 196.0f;
    tokens.focus_stroke = 3.0f / 1.5f;
    tokens.focus_gap = 6.0f / 1.5f;
    tokens.ribbon[0].rgb = 0x2fd07a;
    tokens.ribbon[1].rgb = 0x8fbf6a;
    tokens.ribbon[2].rgb = 0xe05a45;
    tokens.ribbon[3].rgb = 0xe8343e;
    tokens.type[0] = {.size = 72.0f / 1.5f, .line = 72.0f / 1.5f * 1.25f, .weight = 800};
    tokens.type[1] = {.size = 40.0f / 1.5f, .line = 40.0f / 1.5f * 1.25f, .weight = 700};
    tokens.type[2] = {.size = 52.0f / 1.5f, .line = 52.0f / 1.5f * 1.25f, .weight = 800};
    tokens.type[3] = {.size = 22.0f / 1.5f, .line = 22.0f / 1.5f * 1.25f, .weight = 650};
    tokens.type[4] = {.size = 24.0f / 1.5f, .line = 24.0f / 1.5f * 1.25f, .weight = 400};
    tokens.type[5] = {.size = 22.0f / 1.5f, .line = 22.0f / 1.5f * 1.25f, .weight = 400};
    tokens.type[6] = {.size = 21.0f / 1.5f, .line = 21.0f / 1.5f * 1.25f, .weight = 600};
    tokens.type[7] = {.size = 18.0f / 1.5f, .line = 18.0f / 1.5f * 1.25f, .weight = 600};
    tokens.type[8] = {.size = 19.0f / 1.5f, .line = 19.0f / 1.5f * 1.25f, .weight = 400};
    tokens.type[9] = {.size = 15.0f / 1.5f, .line = 15.0f / 1.5f * 1.25f, .weight = 500};
    tokens.type[10] = {.size = 25.0f / 1.5f, .line = 25.0f / 1.5f * 1.25f, .weight = 650};
    tokens.type[11] = {.size = 19.0f / 1.5f, .line = 19.0f / 1.5f * 1.25f, .weight = 400};
    tokens.type[12] = {.size = 20.0f / 1.5f, .line = 20.0f / 1.5f * 1.25f, .weight = 600};
    tokens.type[13] = {
        .size = 22.0f / 1.5f, .line = 22.0f / 1.5f * 1.25f, .weight = 600, .monospace = true};
    tokens.type[14] = tokens.type[13];
    tokens.type[15] = tokens.type[11];
    tokens.type[16] = {.size = 24.0f / 1.5f, .line = 24.0f / 1.5f * 1.25f, .weight = 650};
    for (auto& style : tokens.type)
        style.uniform_lines = false;
    return tokens;
}();

constexpr const ThemeTokens& LookupTheme(ThemeId id) {
    return id == ThemeId::OldNxbox ? OldNxboxTheme : JadyxTheme;
}
// Only the preference crosses threads. Each painter owns its formats, brushes and theme snapshot.
inline std::atomic<ThemeId> selected_ui_theme{ThemeId::Jadyx};
inline void SetUiTheme(ThemeId id) {
    selected_ui_theme.store(id, std::memory_order_relaxed);
}
inline ThemeId UiTheme() {
    return selected_ui_theme.load(std::memory_order_relaxed);
}

} // namespace Jadyx
