// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mixing steps of the spatial sound output (Dolby Atmos, Windows Sonic): every guest stream adds
// its interleaved PCM16 into one planar float bed, one plane per game channel, and each plane then
// feeds one speaker object. Pure logic, no platform headers, so the gates test it on any host.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "audio_core/sink/surround_layout.h"

namespace AudioCore::Sink::Surround {

inline constexpr float S16ToFloat = 1.0F / 32768.0F;

/// Adds `frames` interleaved frames of `channels` samples into the planar bed: plane c holds game
/// channel c at [c * plane_stride, c * plane_stride + frames). Channels past the bed are ignored;
/// a stereo stream fills only the front pair.
constexpr void AccumulateInterleaved(std::span<const std::int16_t> interleaved,
                                     std::uint32_t channels, std::size_t frames,
                                     std::span<float> planes, std::size_t plane_stride) {
    const std::uint32_t used = std::min(channels, GameChannels);
    for (std::uint32_t channel = 0; channel < used; ++channel) {
        float* const plane = planes.data() + channel * plane_stride;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            plane[frame] += static_cast<float>(interleaved[frame * channels + channel]) * S16ToFloat;
        }
    }
}

/// Copies one plane to a speaker object's buffer, clipped: several streams may sum past full scale.
constexpr void WritePlane(std::span<const float> plane, std::span<float> out) {
    const std::size_t count = std::min(plane.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = std::clamp(plane[i], -1.0F, 1.0F);
    }
}

} // namespace AudioCore::Sink::Surround
