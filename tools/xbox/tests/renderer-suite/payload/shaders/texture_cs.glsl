#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout (binding = 0) uniform sampler2D tex;
layout (std430, binding = 0) buffer Block { vec4 value; ivec4 size; };
void main()
{
    value = texelFetch(tex, ivec2(2, 1), 0);
    size = ivec4(textureSize(tex, 0), textureQueryLevels(tex), 0);
}
