#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The uv carries the value to interpolate and the w of the vertex.
layout (location = 0) in vec3 inPos;
layout (location = 2) in vec2 inUv;
layout (location = 0) noperspective out float outLinear;
layout (location = 1) out float outPerspective;
void main()
{
    gl_Position = vec4(inPos * inUv.y, inUv.y);
    outLinear = inUv.x;
    outPerspective = inUv.x;
}
