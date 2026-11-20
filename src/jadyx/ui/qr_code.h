// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A QR code for what a phone camera should open: an address shown on the TV (ISO/IEC 18004,
// byte mode, versions 1 to 10; error correction level M holds up to 213 bytes, level H up to
// 119 and survives a logo over its centre). Pure and platform free: the canvas draws the
// modules (UiPainter::Qr), the gate checks them.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Jadyx::Ui {

/// How much of the code may be lost: M about 15%, H about 30% (room for a logo).
enum class QrLevel { M, H };

struct QrCode {
    QrLevel level = QrLevel::M;
    int version = 0;
    int mask = 0;
    int size = 0;                       ///< Modules per side, without the quiet zone (4 modules).
    std::vector<std::uint8_t> modules;  ///< size * size, row major; 1 is dark.

    bool Dark(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<std::size_t>(y) * size + x] != 0;
    }
};

/// The smallest code that holds `text`; nullopt when it is empty or longer than the level holds
/// (213 bytes at M, 119 at H). `forced_mask` (0-7) skips the mask choice; the gate uses it.
std::optional<QrCode> EncodeQr(std::string_view text, QrLevel level = QrLevel::M, int forced_mask = -1);

/// The side, in modules, of the centred square a logo may cover and the code still reads:
/// level H, versions 2 to 6 only (no format bits nor alignment pattern at the centre), else 0.
/// Odd, so the square sits on whole modules around the centre one.
int QrLogoModules(const QrCode& code);

} // namespace Jadyx::Ui
