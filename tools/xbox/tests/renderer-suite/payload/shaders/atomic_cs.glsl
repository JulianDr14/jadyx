#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout (std430, binding = 0) buffer Block { uint sum; uint low; uint high; uint ored; uint exchanged; };
void main()
{
    uint index = gl_LocalInvocationIndex + 1u;
    atomicAdd(sum, index);
    atomicMin(low, index);
    atomicMax(high, index);
    atomicOr(ored, 1u << (gl_LocalInvocationIndex % 32u));
    atomicCompSwap(exchanged, 7u, 9u);
}
