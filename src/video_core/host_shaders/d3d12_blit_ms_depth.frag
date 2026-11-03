// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Depth is not averaged: each target sample reads its own, so a target of one sample keeps sample 0.

#version 450 core

layout(binding = 0) uniform sampler2DMSArray depth_tex;

layout(location = 0) in vec2 texcoord;

void main() {
    const ivec2 size = textureSize(depth_tex).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    gl_FragDepth = texelFetch(depth_tex, ivec3(texel, 0), gl_SampleID).r;
}
