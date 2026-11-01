#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 4, local_size_y = 4, local_size_z = 1) in;
layout (binding = 0, rgba8) uniform writeonly image2D img;
void main()
{
    ivec2 place = ivec2(gl_GlobalInvocationID.xy);
    imageStore(img, place, vec4(vec2(place) / 31.0, 0.0, 1.0));
}
