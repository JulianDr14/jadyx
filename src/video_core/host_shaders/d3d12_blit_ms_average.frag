// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A resolve of any region: the average of the samples of the pixel point sampling picks.

#version 450 core

layout(binding = 0) uniform sampler2DMSArray tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 20) int samples;
};

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main() {
    const ivec2 size = textureSize(tex).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    vec4 sum = vec4(0.0);
    for (int index = 0; index < samples; ++index) {
        sum += texelFetch(tex, ivec3(texel, 0), index);
    }
    color = sum / float(samples);
}
