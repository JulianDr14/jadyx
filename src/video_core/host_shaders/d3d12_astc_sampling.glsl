// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Isolated probe ABI. All offsets are elements, never pointers. These four SRVs are bounded raw
// views; explicit checks also protect the translator/driver when robust access is unavailable.
#ifdef D3D12_ASTC_FRAGMENT
layout(location = 0) out vec4 probe_output;
#else
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif
layout(push_constant) uniform ProbeConstants {
    uint query_count;
    uint metadata_count;
    uint astc_count;
    uint rgba_count;
    uint repetitions;
    uint query_first;
};
struct ProbeMip { uvec4 dimensions; uvec4 offsets; uvec4 flags; };
struct ProbeQuery { uvec4 config; vec4 coordinates; };
layout(binding = 0, std430) readonly buffer Blocks { uvec4 astc_data[]; };
layout(binding = 1, std430) readonly buffer Queries { ProbeQuery queries[]; };
layout(binding = 2, std430) readonly buffer Metadata { ProbeMip mips[]; };
layout(binding = 3, std430) writeonly buffer Results { vec4 results[]; };
layout(binding = 4, std430) readonly buffer Expanded { uint rgba_data[]; };
#ifdef D3D12_ASTC_NATIVE
layout(binding = 5) uniform sampler2DArray rgba_point;
layout(binding = 6) uniform sampler2DArray rgba_linear;
layout(binding = 7) uniform textureCube reference_cube;
layout(binding = 8) uniform texture3D reference_volume;
layout(binding = 9) uniform texture2DArray reference_srgb;
layout(binding = 40) uniform sampler point_repeat;
layout(binding = 41) uniform sampler point_clamp;
layout(binding = 42) uniform sampler point_border;
layout(binding = 43) uniform sampler linear_repeat;
layout(binding = 44) uniform sampler linear_clamp;
layout(binding = 45) uniform sampler linear_border;
#endif
uvec2 block_dims;
uvec2 requested_texel;
vec4 sampled_color;
bool decoder_srgb;
struct EncodingData { uint data; };
