#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) in vec3 inPos;
layout (location = 1) in ivec4 inValue;
layout (location = 0) out vec4 outColor;
layout (std140, binding = 0) uniform Block { vec4 scale; vec4 offset; };
void main()
{
    gl_Position = vec4(inPos, 1.0);
    outColor = vec4(inValue) * scale + offset;
}
