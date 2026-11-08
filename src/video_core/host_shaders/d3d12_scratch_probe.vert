// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Boot probe of vertex shader private arrays (see d3d12_local_memory_scratch.cpp), the same test
// as d3d12_scratch_probe.frag: a dynamic store read back from a constant index. The full-screen
// triangle carries the result to d3d12_scratch_probe_copy.frag in a flat varying.

#version 460 core

#define WORDS 512

layout(push_constant) uniform Probe {
    uint index;
    uint value;
} probe;

layout(location = 0) flat out uvec2 result;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    uint data[WORDS];
    for (uint i = 0; i < WORDS; ++i) {
        data[i] = 0u;
    }
    data[min(probe.index, WORDS - 1)] = probe.value;
    result = uvec2(data[WORDS - 1], data[0]);
    const float x = float((gl_VertexIndex & 1) << 2);
    const float y = float((gl_VertexIndex & 2) << 1);
    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);
}
