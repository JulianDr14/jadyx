// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Depth between sample counts. Each target sample takes the source samples that fall on it (all
// of them for a target of one, its own when both have as many): their mean when the guest's blit
// filters linearly (average != 0), the first of them when it samples the nearest point.

#version 450 core

layout(binding = 0) uniform sampler2DMSArray depth_tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 20) int src_samples;
    layout(offset = 24) int dst_samples;
    layout(offset = 28) int average;
};

layout(location = 0) in vec2 texcoord;

void main() {
    const ivec2 size = textureSize(depth_tex).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    const int targets = max(dst_samples, 1);
    const int first = gl_SampleID * src_samples / targets;
    const int group = average != 0 ? max(src_samples / targets, 1) : 1;
    float sum = 0.0;
    for (int index = 0; index < group; ++index) {
        sum += texelFetch(depth_tex, ivec3(texel, 0), first + index).r;
    }
    gl_FragDepth = sum / float(group);
}
