#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Each triangle goes to viewports 0 and 1.
layout (triangles) in;
layout (triangle_strip, max_vertices = 6) out;
layout (location = 0) in vec4 inColor[];
layout (location = 0) out vec4 outColor;
void main()
{
    for (int viewport = 0; viewport < 2; ++viewport) {
        for (int i = 0; i < 3; ++i) {
            gl_Position = gl_in[i].gl_Position;
            gl_ViewportIndex = viewport;
            outColor = inColor[i];
            EmitVertex();
        }
        EndPrimitive();
    }
}
