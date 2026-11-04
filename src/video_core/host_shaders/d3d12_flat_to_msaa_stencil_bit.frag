// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_flat_to_msaa.frag for one stencil bit: discards the samples whose texel lacks bit_mask.

#version 450 core

// The stencil plane, its value swizzled into every component.
layout(binding = 0) uniform usampler2DArray flat_in;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
    uint bit_mask;
};

void main() {
    const ivec2 msaa_coord = ivec2(gl_FragCoord.xy) - dst_offset;
    const ivec2 sample_offset = ivec2(gl_SampleID % scale.x, gl_SampleID / scale.x);
    const ivec3 coord = ivec3(msaa_coord * scale + sample_offset + src_offset, 0);
    if ((texelFetch(flat_in, coord, 0).r & bit_mask) == 0u) {
        discard;
    }
}
