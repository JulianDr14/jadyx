#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 2, local_size_y = 3, local_size_z = 1) in;
layout (std430, binding = 0) buffer Block { uvec4 values[]; };
void main()
{
    uint index = gl_WorkGroupID.x * 6u + gl_LocalInvocationIndex;
    values[index] = uvec4(gl_WorkGroupID.x, gl_LocalInvocationID.x, gl_LocalInvocationID.y, gl_GlobalInvocationID.x);
}
