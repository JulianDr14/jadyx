#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 8, local_size_y = 1, local_size_z = 1) in;
layout (std430, binding = 0) buffer Block { uint values[8]; };
shared uint scratch[8];
void main()
{
    scratch[gl_LocalInvocationIndex] = gl_LocalInvocationIndex + 10u;
    barrier();
    memoryBarrierShared();
    values[gl_LocalInvocationIndex] = scratch[7u - gl_LocalInvocationIndex];
}
