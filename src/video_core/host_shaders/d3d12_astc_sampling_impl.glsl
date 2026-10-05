// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

vec3 LinearSRGB(vec3 v) {
    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92,
               lessThanEqual(v, vec3(0.04045)));
}

int Address(int p, int size, uint mode) {
    // GLSL '%' with negative operands is undefined. Keep both remainders non-negative so Mesa
    // cannot fold the apparently redundant +size into a different negative-coordinate result.
    if (mode == 0u) return p >= 0 ? p % size : size - 1 - ((-1 - p) % size);
    if (mode == 1u) return clamp(p, 0, size - 1);
    return p; // transparent black border (the prototype's explicit border color)
}

// +X,-X,+Y,-Y,+Z,-Z, the D3D/Vulkan face convention. Remapping taps through a direction lets
// bilinear taps cross faces without clamping to an edge.
vec3 FaceDirection(uint f, vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    if (f == 0u) return vec3(1, -p.y, -p.x);
    if (f == 1u) return vec3(-1, -p.y, p.x);
    if (f == 2u) return vec3(p.x, 1, p.y);
    if (f == 3u) return vec3(p.x, -1, -p.y);
    if (f == 4u) return vec3(p.x, -p.y, 1);
    return vec3(-p.x, -p.y, -1);
}

vec3 CubeLocation(vec3 d) {
    vec3 a = abs(d);
    uint f;
    vec2 p;
    if (a.x >= a.y && a.x >= a.z) {
        f = d.x >= 0.0 ? 0u : 1u;
        p = vec2(d.x >= 0.0 ? -d.z : d.z, -d.y) / a.x;
    } else if (a.y >= a.z) {
        f = d.y >= 0.0 ? 2u : 3u;
        p = vec2(d.x, d.y >= 0.0 ? d.z : -d.z) / a.y;
    } else {
        f = d.z >= 0.0 ? 4u : 5u;
        p = vec2(d.z >= 0.0 ? d.x : -d.x, -d.y) / a.z;
    }
    return vec3(p * 0.5 + 0.5, float(f));
}

vec4 LoadTexelSimple(uint level, ivec3 p, uint address_mode) {
    if (level >= metadata_count) return vec4(0);
    ProbeMip m = mips[level];
    ivec3 size = ivec3(m.dimensions.xyz);
    if (m.flags.x == 2u && (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y)) {
        vec3 loc = CubeLocation(FaceDirection(uint(p.z), (vec2(p.xy) + 0.5) / vec2(size.xy)));
        p = ivec3(clamp(ivec2(floor(loc.xy * vec2(size.xy))), ivec2(0), size.xy - 1), int(loc.z));
    } else {
        p.x = Address(p.x, size.x, address_mode);
        p.y = Address(p.y, size.y, address_mode);
        // Array/cube layers are selected explicitly, never wrapped like spatial coordinates.
        if (m.flags.x == 3u) p.z = Address(p.z, size.z, address_mode);
    }
    if (any(lessThan(p, ivec3(0))) || any(greaterThanEqual(p, size))) return vec4(0);
    vec4 color;
#ifdef D3D12_ASTC_REFERENCE
    uint index = m.offsets.y + (uint(p.z) * m.dimensions.y + uint(p.y)) * m.dimensions.x + uint(p.x);
    if (index >= rgba_count) return vec4(0);
    color = unpackUnorm4x8(rgba_data[index]);
#else
    block_dims = m.offsets.zw;
    uvec2 blocks = (m.dimensions.xy + block_dims - 1u) / block_dims;
    uvec2 xy = uvec2(p.xy);
    uint index = m.offsets.x + (uint(p.z) * blocks.y + xy.y / block_dims.y) * blocks.x + xy.x / block_dims.x;
    if (index >= astc_count) return vec4(0);
    requested_texel = xy % block_dims;
    local_buff = astc_data[index];
    // Every tap is a new decode; private decoder state must not leak between taps/repetitions.
    total_bitsread = 0;
    color_bitsread = 0;
    result_index = 0;
    result_limit_reached = false;
    result_vector_max_index = 0;
    color_endpoint_data = uvec4(0);
    sampled_color = vec4(0);
    decoder_srgb = m.flags.y != 0u;
    DecompressBlock(ivec3(0));
    color = sampled_color;
#endif
    if (m.flags.y != 0u) {
        // sRGB transfer operates on stored 8-bit values before any spatial/mip filtering.
        color.rgb = LinearSRGB(color.rgb);
    }
    return color;
}

vec4 LoadTexel(uint level, ivec3 p, uint address_mode) {
    if (level >= metadata_count) return vec4(0);
    ProbeMip m = mips[level];
    ivec2 size = ivec2(m.dimensions.xy);
    bool at_corner = m.flags.x == 2u && (p.x < 0 || p.x >= size.x) && (p.y < 0 || p.y >= size.y);
    // One decode call site: inlining three copies of the decoder would inflate DXIL/compilation.
    vec4 value = vec4(0);
    uint taps = at_corner ? 3u : 1u;
    ivec2 corner = clamp(p.xy, ivec2(0), size - 1);
    for (uint tap = 0u; tap < taps; ++tap) {
        ivec3 location = p;
        if (at_corner) {
            location = ivec3(corner, p.z);
            if (tap == 1u) location.x = p.x;
            if (tap == 2u) location.y = p.y;
        }
        value += LoadTexelSimple(level, location, address_mode);
    }
    return value / float(taps);
}

#ifdef D3D12_ASTC_NATIVE
vec4 NativeVolume(vec3 p, float lod, uint address, bool point) {
    if (point) {
        if (address == 0u) return textureLod(sampler3D(reference_volume, point_repeat), p, lod);
        if (address == 1u) return textureLod(sampler3D(reference_volume, point_clamp), p, lod);
        return textureLod(sampler3D(reference_volume, point_border), p, lod);
    }
    if (address == 0u) return textureLod(sampler3D(reference_volume, linear_repeat), p, lod);
    if (address == 1u) return textureLod(sampler3D(reference_volume, linear_clamp), p, lod);
    return textureLod(sampler3D(reference_volume, linear_border), p, lod);
}
vec4 NativeSRGB(vec3 p, float lod, uint address, bool point) {
    if (point) {
        if (address == 0u) return textureLod(sampler2DArray(reference_srgb, point_repeat), p, lod);
        if (address == 1u) return textureLod(sampler2DArray(reference_srgb, point_clamp), p, lod);
        return textureLod(sampler2DArray(reference_srgb, point_border), p, lod);
    }
    if (address == 0u) return textureLod(sampler2DArray(reference_srgb, linear_repeat), p, lod);
    if (address == 1u) return textureLod(sampler2DArray(reference_srgb, linear_clamp), p, lod);
    return textureLod(sampler2DArray(reference_srgb, linear_border), p, lod);
}
#endif

vec4 SampleLevel(uint level, vec3 coordinates, uint mode, uint address_mode) {
    ProbeMip m = mips[level];
    bool volume = m.flags.x == 3u;
    if (mode == 0u) return LoadTexel(level, ivec3(floor(coordinates)), address_mode);
    vec3 pos = coordinates;
    if (m.flags.x == 2u) pos = CubeLocation(coordinates);
    pos.xy *= vec2(m.dimensions.xy);
    if (volume) pos.z *= float(m.dimensions.z);
    if (mode == 1u) return LoadTexel(level, ivec3(floor(pos)), address_mode);
    pos.xy -= 0.5;
    if (volume) pos.z -= 0.5;
    ivec3 base = ivec3(floor(pos));
    vec3 f = fract(pos);
    vec4 planes[2];
    for (int z = 0; z < (volume ? 2 : 1); z++) {
        vec4 a = LoadTexel(level, base + ivec3(0, 0, z), address_mode);
        vec4 b = LoadTexel(level, base + ivec3(1, 0, z), address_mode);
        vec4 c = LoadTexel(level, base + ivec3(0, 1, z), address_mode);
        vec4 d = LoadTexel(level, base + ivec3(1, 1, z), address_mode);
        planes[z] = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }
    return volume ? mix(planes[0], planes[1], f.z) : planes[0];
}

vec4 SampleExplicit(ProbeQuery q) {
    uint base = q.config.x;
    if (base >= metadata_count) return vec4(0);
    float lod = clamp(q.coordinates.w, 0.0, float(mips[base].dimensions.w - 1u));
    uint l0 = uint(floor(lod));
    uint l1 = min(l0 + 1u, mips[base].dimensions.w - 1u);
    if (base + l1 >= metadata_count) return vec4(0);
#ifdef D3D12_ASTC_NATIVE
    ProbeMip m = mips[base];
    float native_lod = q.config.y == 3u ? lod : float(l0);
    if (m.flags.x == 2u) {
        vec3 dir = q.config.y == 0u ? FaceDirection(uint(q.coordinates.z),
            (q.coordinates.xy + 0.5) / vec2(mips[base + l0].dimensions.xy)) : q.coordinates.xyz;
        if (q.config.y <= 1u) return textureLod(samplerCube(reference_cube, point_clamp), dir, native_lod);
        return textureLod(samplerCube(reference_cube, linear_clamp), dir, native_lod);
    }
    if (m.flags.x == 3u) {
        if (q.config.y == 0u) return texelFetch(sampler3D(reference_volume, point_clamp), ivec3(q.coordinates.xyz), int(l0));
        return NativeVolume(q.coordinates.xyz, native_lod, q.config.z, q.config.y == 1u);
    }
    if (m.flags.y != 0u) {
        if (q.config.y == 0u) return texelFetch(sampler2DArray(reference_srgb, point_clamp), ivec3(q.coordinates.xyz), int(l0));
        return NativeSRGB(q.coordinates.xyz, native_lod, q.config.z, q.config.y == 1u);
    }
    // Bulk benchmark reference is an RGBA8 array with the same complete mips.
    if (q.config.y == 0u) return texelFetch(rgba_point, ivec3(q.coordinates.xyz), int(l0));
    if (q.config.y == 1u) return textureLod(rgba_point, q.coordinates.xyz, float(l0));
    return textureLod(rgba_linear, q.coordinates.xyz, q.config.y == 3u ? lod : float(l0));
#else
    vec4 a = SampleLevel(base + l0, q.coordinates.xyz, q.config.y, q.config.z);
    if (q.config.y != 3u) return a;
    return mix(a, SampleLevel(base + l1, q.coordinates.xyz, 2u, q.config.z), fract(lod));
#endif
}

void main() {
    // The 8x8 group is flattened; no cross-invocation/shared decoded cache exists.
#ifdef D3D12_ASTC_FRAGMENT
    uint i = uint(gl_FragCoord.y) * 256u + uint(gl_FragCoord.x);
    if (i >= query_count) { probe_output = vec4(0); return; }
#else
    uint i = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
    if (i >= query_count) return;
#endif
    ProbeQuery q = queries[query_first + i];
    vec4 sum = vec4(0);
    // Vary queries as well as samples; this prevents a compiler hoisting one decode out of the loop.
    uint taps = max(q.config.w, 1u);
    for (uint r = 0u; r < repetitions; r++) {
        for (uint t = 0u; t < taps; t++) {
            ProbeQuery next = queries[query_first + ((i + t + r * 17u) % query_count)];
            sum += SampleExplicit(next);
        }
    }
#ifdef D3D12_ASTC_FRAGMENT
    probe_output = sum / float(taps * repetitions);
#else
    results[i] = sum / float(taps * repetitions);
#endif
}
