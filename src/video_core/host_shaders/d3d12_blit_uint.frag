// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A blit of unsigned integers: the texel point sampling picks, without a float in between.

#version 450 core

layout(binding = 0) uniform usampler2D tex;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out uvec4 color;

void main() {
    const ivec2 size = textureSize(tex, 0);
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    color = texelFetch(tex, texel, 0);
}
