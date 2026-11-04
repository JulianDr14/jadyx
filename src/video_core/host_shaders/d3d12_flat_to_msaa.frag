// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The inverse of d3d12_msaa_to_flat.frag: each sample of the target reads its texel of the
// single-sample alias.

#version 450 core

#ifndef SAMPLER_TYPE
#define SAMPLER_TYPE sampler2DArray
#endif
#ifndef TEXEL_TYPE
#define TEXEL_TYPE vec4
#endif

layout(binding = 0) uniform SAMPLER_TYPE flat_in;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
};

layout(location = 0) out TEXEL_TYPE color;

void main() {
    const ivec2 msaa_coord = ivec2(gl_FragCoord.xy) - dst_offset;
    const ivec2 sample_offset = ivec2(gl_SampleID % scale.x, gl_SampleID / scale.x);
    color = texelFetch(flat_in, ivec3(msaa_coord * scale + sample_offset + src_offset, 0), 0);
}
