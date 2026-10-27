// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// One implicit-LOD sample per pixel of a 16x16 viewport: uv = frag coord * scale + offset, so the
// derivative LOD is log2(scale * size). BIAS adds a shader bias (texture with bias).
#version 450

layout(set = 0, binding = 0, std430) buffer Data {
    vec4 params; // scale, offset u, offset v, bias
    vec4 results[];
};
layout(set = 0, binding = 1) uniform sampler2D tex;

void main() {
    const vec2 uv = gl_FragCoord.xy * params.x + params.yz;
#ifdef BIAS
    const vec4 value = texture(tex, uv, params.w);
#else
    const vec4 value = texture(tex, uv);
#endif
    results[uint(gl_FragCoord.y) * 16u + uint(gl_FragCoord.x)] = value;
}
