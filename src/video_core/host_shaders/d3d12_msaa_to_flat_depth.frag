// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_msaa_to_flat.frag for depth.

#version 450 core

layout(binding = 0) uniform sampler2DMSArray msaa_in;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
};

void main() {
    const ivec2 coord = ivec2(gl_FragCoord.xy) - dst_offset + src_offset;
    const ivec2 sample_offset = coord % scale;
    const int sample_id = sample_offset.x + scale.x * sample_offset.y;
    gl_FragDepth = texelFetch(msaa_in, ivec3(coord / scale, 0), sample_id).r;
}
