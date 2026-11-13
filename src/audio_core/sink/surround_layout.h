// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How the games' channels reach the console's speakers. Pure logic, no platform headers, so the
// gates test it on any host: the XAudio2 sink only feeds it what the console reports.
//
// The Switch mixes at most 5.1 (FL, FR, FC, LFE, BL, BR: the WAVE order). Whether the games get
// 5.1 or stereo depends on the console output: HDMI 5.1/7.1, and Dolby Atmos or Windows Sonic
// (which present a 7.1 mix to apps and render it spatially), all report six channels or more.
// In stereo the guest renderer applies the game's own downmix, as a Switch on a stereo TV does.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace AudioCore::Sink::Surround {

inline constexpr std::uint32_t StereoChannels = 2;
inline constexpr std::uint32_t GameChannels = 6; ///< Switch 5.1.
/// Largest console layout handled (7.1.4 beds); wider masks keep their first 12 speakers.
inline constexpr std::uint32_t MaxOutputChannels = 12;

/// Speaker bits with the values of the Windows channel mask (ksmedia.h SPEAKER_*).
enum Speaker : std::uint32_t {
    FrontLeft = 0x1,
    FrontRight = 0x2,
    FrontCenter = 0x4,
    LowFrequency = 0x8,
    BackLeft = 0x10,
    BackRight = 0x20,
    SideLeft = 0x200,
    SideRight = 0x400,
};

inline constexpr std::uint32_t MaskStereo = FrontLeft | FrontRight;
inline constexpr std::uint32_t Mask5Point1 = MaskStereo | FrontCenter | LowFrequency | BackLeft | BackRight;
inline constexpr std::uint32_t Mask5Point1Side = MaskStereo | FrontCenter | LowFrequency | SideLeft | SideRight;
inline constexpr std::uint32_t Mask7Point1 = Mask5Point1 | SideLeft | SideRight;

/// Channels the games are told about: 5.1 only when allowed and the console outputs surround.
constexpr std::uint32_t GameChannelCount(bool surround_allowed, std::uint32_t console_channels) {
    return surround_allowed && console_channels >= GameChannels ? GameChannels : StereoChannels;
}

/// The console's mask, or the usual layout for its channel count when it reports none.
constexpr std::uint32_t EffectiveMask(std::uint32_t console_channels, std::uint32_t reported_mask) {
    if (reported_mask != 0) return reported_mask;
    return console_channels >= 8 ? Mask7Point1 : console_channels >= 6 ? Mask5Point1 : MaskStereo;
}

/// XAudio2 output matrix, destination-major: level of source s on output d is at [d * 6 + s].
struct Matrix {
    std::uint32_t outputs{};
    std::array<float, GameChannels * MaxOutputChannels> levels{};

    constexpr float At(std::uint32_t output, std::uint32_t source) const {
        return levels[output * GameChannels + source];
    }
};

namespace Detail {
/// Output index of a speaker in a mask (channels are packed in bit order), or -1 if absent.
constexpr int IndexOf(std::uint32_t mask, std::uint32_t speaker) {
    return (mask & speaker) == 0 ? -1 : std::popcount(mask & (speaker - 1));
}
} // namespace Detail

/// Places 5.1 on the console layout. Speakers it has get their channel unchanged; 5.1 surrounds
/// go to the side pair when there is one (where they sit in 7.1), else to the back pair. What has
/// no speaker folds into the nearest ones at -3 dB; LFE without a subwoofer channel is dropped, as
/// standard downmixes do. Mono outputs take the front pair at -3 dB each.
constexpr Matrix MapGameSurround(std::uint32_t console_channels, std::uint32_t reported_mask) {
    constexpr float Full = 1.0F, Half = 0.70710678F;
    const std::uint32_t mask = EffectiveMask(console_channels, reported_mask);
    Matrix matrix;
    matrix.outputs = console_channels < MaxOutputChannels ? console_channels : MaxOutputChannels;
    const auto send = [&matrix](std::uint32_t source, int output, float level) {
        if (output >= 0 && static_cast<std::uint32_t>(output) < matrix.outputs)
            matrix.levels[static_cast<std::uint32_t>(output) * GameChannels + source] += level;
    };
    if (matrix.outputs == 0) return matrix;
    if (matrix.outputs == 1) {
        send(0, 0, Half), send(1, 0, Half), send(2, 0, Full), send(4, 0, Half), send(5, 0, Half);
        return matrix;
    }

    using Detail::IndexOf;
    const int left = IndexOf(mask, FrontLeft) >= 0 ? IndexOf(mask, FrontLeft) : 0;
    const int right = IndexOf(mask, FrontRight) >= 0 ? IndexOf(mask, FrontRight) : 1;
    send(0, left, Full);
    send(1, right, Full);
    if (const int center = IndexOf(mask, FrontCenter); center >= 0) {
        send(2, center, Full);
    } else {
        send(2, left, Half), send(2, right, Half);
    }
    send(3, IndexOf(mask, LowFrequency), Full);

    const bool sides = (mask & (SideLeft | SideRight)) == (SideLeft | SideRight);
    const int surround_left = IndexOf(mask, sides ? SideLeft : BackLeft);
    const int surround_right = IndexOf(mask, sides ? SideRight : BackRight);
    if (surround_left >= 0) send(4, surround_left, Full); else send(4, left, Half);
    if (surround_right >= 0) send(5, surround_right, Full); else send(5, right, Half);
    return matrix;
}

} // namespace AudioCore::Sink::Surround
