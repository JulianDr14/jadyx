// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A multisampled image into its single-sample alias, whose texels are the samples: scale.x by
// scale.y texels per pixel, sample index = x + scale.x * y, as the guest stores them.

#version 450 core

#ifndef SAMPLER_TYPE
#define SAMPLER_TYPE sampler2DMSArray
#endif
#ifndef TEXEL_TYPE
#define TEXEL_TYPE vec4
#endif

layout(binding = 0) uniform SAMPLER_TYPE msaa_in;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
};

layout(location = 0) out TEXEL_TYPE color;

void main() {
    const ivec2 coord = ivec2(gl_FragCoord.xy) - dst_offset + src_offset;
    const ivec2 sample_offset = coord % scale;
    const int sample_id = sample_offset.x + scale.x * sample_offset.y;
    color = texelFetch(msaa_in, ivec3(coord / scale, 0), sample_id);
}
