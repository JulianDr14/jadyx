#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inUv;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec2 outUv;

void main()
{
    gl_Position = vec4(inPos, 1.0);
    outColor = inColor;
    outUv = inUv;
}
