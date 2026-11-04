// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_blit_stencil_bit.frag from a multisampled source: each target sample tests one,
// as d3d12_blit_ms_uint.frag picks it.

#version 450 core

// The stencil plane, its value swizzled into every component.
layout(binding = 0) uniform usampler2DMSArray stencil_tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 16) uint bit_mask;
    layout(offset = 20) int src_samples;
    layout(offset = 24) int dst_samples;
};

layout(location = 0) in vec2 texcoord;

void main() {
    const ivec2 size = textureSize(stencil_tex).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    const int sample_id = gl_SampleID * src_samples / max(dst_samples, 1);
    if ((texelFetch(stencil_tex, ivec3(texel, 0), sample_id).r & bit_mask) == 0u) {
        discard;
    }
}
