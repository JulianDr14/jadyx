// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Header-only: how the games' 5.1 reaches each console layout (audio_core/sink/surround_layout.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include "audio_core/sink/surround_layout.h"

namespace {
using namespace AudioCore::Sink::Surround;

int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-4F;
}
enum Source : std::uint32_t { L, R, C, Lfe, Ls, Rs };

/// Every source reaches the output with the expected total level: nothing lost, nothing doubled.
float SourceTotal(const Matrix& m, std::uint32_t source) {
    float total = 0;
    for (std::uint32_t out = 0; out < m.outputs; ++out) total += m.At(out, source);
    return total;
}
} // namespace

int main() {
    // Channel count the games are told.
    static_assert(GameChannelCount(true, 2) == 2);
    static_assert(GameChannelCount(true, 6) == 6);
    static_assert(GameChannelCount(true, 8) == 6);  // 7.1, Dolby Atmos, Windows Sonic.
    static_assert(GameChannelCount(true, 12) == 6); // 7.1.4 bed.
    static_assert(GameChannelCount(false, 8) == 2); // Setting off.
    static_assert(GameChannelCount(true, 0) == 2);  // Probe failed.
    static_assert(MapGameSurround(8, Mask7Point1).At(6, Ls) == 1.0F, "usable at compile time");

    // 5.1 console: identity.
    {
        const auto m = MapGameSurround(6, Mask5Point1);
        Check(m.outputs == 6, "5.1 outputs");
        for (std::uint32_t out = 0; out < 6; ++out)
            for (std::uint32_t src = 0; src < 6; ++src)
                Check(m.At(out, src) == (out == src ? 1.0F : 0.0F), "5.1 identity");
    }
    // 5.1 with side surrounds (KSAUDIO_SPEAKER_5POINT1_SURROUND): surrounds on the sides.
    {
        const auto m = MapGameSurround(6, Mask5Point1Side);
        Check(m.At(4, Ls) == 1.0F && m.At(5, Rs) == 1.0F, "5.1 side surrounds");
    }
    // 7.1 (also what Atmos and Sonic present): FL FR FC LFE BL BR SL SR; surrounds on the sides,
    // backs silent.
    {
        const auto m = MapGameSurround(8, Mask7Point1);
        Check(m.outputs == 8, "7.1 outputs");
        Check(m.At(0, L) == 1.0F && m.At(1, R) == 1.0F && m.At(2, C) == 1.0F && m.At(3, Lfe) == 1.0F,
              "7.1 fronts and LFE");
        Check(m.At(6, Ls) == 1.0F && m.At(7, Rs) == 1.0F, "7.1 surrounds on sides");
        for (std::uint32_t src = 0; src < 6; ++src)
            Check(m.At(4, src) == 0.0F && m.At(5, src) == 0.0F, "7.1 backs silent");
        // No mask reported: same as 7.1.
        const auto guessed = MapGameSurround(8, 0);
        Check(guessed.At(6, Ls) == 1.0F && guessed.At(7, Rs) == 1.0F, "7.1 without mask");
    }
    // The console changed to stereo between the probe and the engine: everything folds into L/R.
    {
        const auto m = MapGameSurround(2, MaskStereo);
        Check(m.outputs == 2, "stereo outputs");
        Check(m.At(0, L) == 1.0F && m.At(1, R) == 1.0F, "stereo fronts");
        Check(Near(m.At(0, C), 0.70710678F) && Near(m.At(1, C), 0.70710678F), "stereo centre -3 dB");
        Check(Near(m.At(0, Ls), 0.70710678F) && m.At(1, Ls) == 0.0F, "stereo left surround");
        Check(Near(m.At(1, Rs), 0.70710678F) && m.At(0, Rs) == 0.0F, "stereo right surround");
        Check(SourceTotal(m, Lfe) == 0.0F, "stereo drops LFE");
    }
    // Quad (no centre, no LFE): centre splits, LFE dropped, surrounds on the backs.
    {
        const auto m = MapGameSurround(4, MaskStereo | BackLeft | BackRight);
        Check(Near(m.At(0, C), 0.70710678F) && Near(m.At(1, C), 0.70710678F), "quad centre");
        Check(m.At(2, Ls) == 1.0F && m.At(3, Rs) == 1.0F, "quad surrounds");
        Check(SourceTotal(m, Lfe) == 0.0F, "quad drops LFE");
    }
    // Mono: one output, never out of bounds.
    {
        const auto m = MapGameSurround(1, FrontCenter);
        Check(m.outputs == 1 && m.At(0, C) == 1.0F && Near(m.At(0, L), 0.70710678F), "mono");
    }
    // Wider than supported: clamped to MaxOutputChannels, nothing written past it.
    {
        const auto m = MapGameSurround(32, 0);
        Check(m.outputs == MaxOutputChannels, "clamped outputs");
        Check(m.At(6, Ls) == 1.0F, "wide layout keeps 7.1 positions");
    }
    // Every layout keeps each non-LFE source exactly once at unit or -3 dB pairs.
    for (const auto [channels, mask] : {std::pair{6u, Mask5Point1}, std::pair{8u, Mask7Point1},
                                        std::pair{6u, Mask5Point1Side}}) {
        const auto m = MapGameSurround(channels, mask);
        for (std::uint32_t src = 0; src < 6; ++src)
            Check(SourceTotal(m, src) == 1.0F, "surround layouts keep every source");
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("surround-layout: OK\n");
    return 0;
}
