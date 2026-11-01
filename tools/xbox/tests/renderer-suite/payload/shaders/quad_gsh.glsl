#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A point becomes a quad, the way particle systems use geometry shaders.
layout (points) in;
layout (triangle_strip, max_vertices = 4) out;
layout (location = 0) in vec4 inColor[];
layout (location = 0) out vec4 outColor;
void main()
{
    const vec2 corners[4] = vec2[](vec2(-0.25, 0.25), vec2(-0.25, -0.25), vec2(0.25, 0.25), vec2(0.25, -0.25));
    for (int i = 0; i < 4; ++i) {
        gl_Position = gl_in[0].gl_Position + vec4(corners[i], 0.0, 0.0);
        outColor = inColor[0];
        EmitVertex();
    }
    EndPrimitive();
}
