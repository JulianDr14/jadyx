// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_msaa_to_flat.frag for one stencil bit: discards where the sample lacks bit_mask.

#version 450 core

// The stencil plane, its value swizzled into every component.
layout(binding = 0) uniform usampler2DMSArray msaa_in;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
    uint bit_mask;
};

void main() {
    const ivec2 coord = ivec2(gl_FragCoord.xy) - dst_offset + src_offset;
    const ivec2 sample_offset = coord % scale;
    const int sample_id = sample_offset.x + scale.x * sample_offset.y;
    if ((texelFetch(msaa_in, ivec3(coord / scale, 0), sample_id).r & bit_mask) == 0u) {
        discard;
    }
}
