// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_blit_stencil_bit.frag from a multisampled source: each target sample tests its own.

#version 450 core

// The stencil plane, its value swizzled into every component.
layout(binding = 0) uniform usampler2DMSArray stencil_tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 16) uint bit_mask;
};

layout(location = 0) in vec2 texcoord;

void main() {
    const ivec2 size = textureSize(stencil_tex).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    if ((texelFetch(stencil_tex, ivec3(texel, 0), gl_SampleID).r & bit_mask) == 0u) {
        discard;
    }
}
