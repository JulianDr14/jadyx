// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_flat_to_msaa.frag for depth.

#version 450 core

layout(binding = 0) uniform sampler2DArray flat_in;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
};

void main() {
    const ivec2 msaa_coord = ivec2(gl_FragCoord.xy) - dst_offset;
    const ivec2 sample_offset = ivec2(gl_SampleID % scale.x, gl_SampleID / scale.x);
    gl_FragDepth =
        texelFetch(flat_in, ivec3(msaa_coord * scale + sample_offset + src_offset, 0), 0).r;
}
