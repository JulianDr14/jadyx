// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_blit_uint.frag for signed integers.

#version 450 core

layout(binding = 0) uniform isampler2D tex;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out ivec4 color;

void main() {
    const ivec2 size = textureSize(tex, 0);
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    color = texelFetch(tex, texel, 0);
}
