#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Places a quad in a cell of an 8x8 grid from its ids, with no vertex buffer at all.
layout (location = 0) out vec4 outColor;
layout (std140, binding = 0) uniform Block { ivec4 mode; };
void main()
{
    const vec2 corners[4] = vec2[](vec2(0, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    int vertex_in_quad = gl_VertexID % 4;
    int quad = gl_VertexID / 4;
    int cell = mode.x == 0 ? gl_InstanceID : quad;
    vec2 place = vec2(cell % 8, cell / 8) + corners[vertex_in_quad];
    gl_Position = vec4(place.x / 4.0 - 1.0, 1.0 - place.y / 4.0, 0.0, 1.0);
    outColor = vec4(float(gl_InstanceID + 1) / 8.0, float(quad + 1) / 8.0, 0.0, 1.0);
}
