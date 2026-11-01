#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A triangle goes to the layer the uniform names.
layout (triangles) in;
layout (triangle_strip, max_vertices = 3) out;
layout (location = 0) in vec4 inColor[];
layout (location = 0) out vec4 outColor;
layout (std140, binding = 0) uniform Block { ivec4 layer; };
void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gl_Layer = layer.x;
        outColor = inColor[i];
        EmitVertex();
    }
    EndPrimitive();
}
