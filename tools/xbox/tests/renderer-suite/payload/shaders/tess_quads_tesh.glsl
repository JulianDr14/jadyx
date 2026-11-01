#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (quads, equal_spacing, ccw) in;
layout (location = 0) out vec4 outColor;
void main()
{
    gl_Position = vec4(gl_TessCoord.x * 2.0 - 1.0, 1.0 - gl_TessCoord.y * 2.0, 0.0, 1.0);
    outColor = vec4(gl_TessCoord.x, gl_TessCoord.y, 0.0, 1.0);
}
