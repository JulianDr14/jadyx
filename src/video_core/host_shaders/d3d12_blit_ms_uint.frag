// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Unsigned integers between sample counts. Each target sample takes the source samples that
// fall on it (all of them for a target of one, its own when both have as many) and writes their
// mean, rounded down: the guest's resolves average the samples.

#version 450 core

layout(binding = 0) uniform usampler2DMSArray tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 20) int src_samples;
    layout(offset = 24) int dst_samples;
};

layout(location = 0) in vec2 texcoord;
layout(location = 0) out uvec4 color;

void main() {
    const ivec2 size = textureSize(tex).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    const int targets = max(dst_samples, 1);
    const int first = gl_SampleID * src_samples / targets;
    const int group = max(src_samples / targets, 1);
    if (group == 1) {
        color = texelFetch(tex, ivec3(texel, 0), first);
        return;
    }
    // Sample counts are powers of two, so the mean is shifts and masks, no division: the floor
    // of the mean, exact and without overflow, is the sum of each value shifted down plus the
    // sum of the bits shifted out, shifted down (remainders sum to less than group * group).
    const int shift = findLSB(group);
    const uvec4 mask = uvec4(group - 1);
    uvec4 high = uvec4(0);
    uvec4 low = uvec4(0);
    for (int index = 0; index < group; ++index) {
        const uvec4 value = texelFetch(tex, ivec3(texel, 0), first + index);
        high += value >> shift;
        low += value & mask;
    }
    color = high + (low >> shift);
}
