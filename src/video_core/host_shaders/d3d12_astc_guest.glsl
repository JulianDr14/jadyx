// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The importer replaces the dummy entry point; the exported function takes one bounded typed
// buffer. Every guest binding keeps its native texture and receives an additional ASTC buffer.
layout(set=0, binding=4096) uniform usamplerBuffer guest_dummy;
layout(set=29, binding=0, std430) readonly buffer GuestDraw { uvec4 guest_draw[]; };
vec4 eden_astc_coords;
vec4 eden_astc_native;
vec4 eden_astc_lod;
vec4 eden_astc_offset;
uint eden_astc_slot;
uint eden_astc_operation;
uvec2 block_dims;
uvec2 requested_texel;
bool decoder_srgb;
vec4 sampled_color;
struct EncodingData { uint data; };
#ifdef D3D12_ASTC_FRAGMENT
layout(location=0) out vec4 guest_dummy_output;
#else
layout(local_size_x=1) in;
#endif
