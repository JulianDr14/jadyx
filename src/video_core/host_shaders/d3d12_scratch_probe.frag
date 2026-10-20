// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Boot probe of pixel shader private arrays (see d3d12_local_memory_scratch.cpp): a dynamic store
// read back from a constant index, which no compiler can forward in registers. Hosts that lose
// large private arrays in pixel shaders return 0 instead of the stored value.

#version 460 core

#define WORDS 512

layout(push_constant) uniform Probe {
    uint index;
    uint value;
} probe;

layout(location = 0) out uvec4 color;

void main() {
    uint data[WORDS];
    for (uint i = 0; i < WORDS; ++i) {
        data[i] = 0u;
    }
    data[min(probe.index, WORDS - 1)] = probe.value;
    color = uvec4(data[WORDS - 1], data[0], 0u, 0u);
}
