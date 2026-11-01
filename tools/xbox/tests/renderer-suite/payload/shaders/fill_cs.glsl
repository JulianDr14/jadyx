#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 4, local_size_y = 2, local_size_z = 1) in;
layout (std430, binding = 0) buffer Block { uint values[]; };
void main()
{
    uint index = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * 8u;
    values[index] = index * 2u + 1u;
}
