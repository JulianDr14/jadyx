// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// One bit of a stencil blit for hosts without shader stencil export (SV_StencilRef): the pass
// writes reference 0xFF through a write mask of bit_mask, so it sets that bit of the target
// wherever the source texel has it and discards elsewhere. Eight passes copy the whole value.

#version 450 core

// The stencil plane, its value swizzled into every component.
layout(binding = 0) uniform usampler2DArray stencil_tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 16) uint bit_mask;
};

layout(location = 0) in vec2 texcoord;

void main() {
    // The texel point sampling picks, as the depth pass reads it.
    const ivec2 size = textureSize(stencil_tex, 0).xy;
    const ivec2 texel = clamp(ivec2(floor(texcoord * vec2(size))), ivec2(0), size - 1);
    if ((texelFetch(stencil_tex, ivec3(texel, 0), 0).r & bit_mask) == 0u) {
        discard;
    }
}
