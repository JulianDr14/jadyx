#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The patch's corners interpolated: 0 at (0, 0), 1 at (0, 1), 2 at (1, 0), 3 at (1, 1).
layout (quads, equal_spacing, ccw) in;
layout (location = 0) out vec4 outColor;
void main()
{
    const vec2 uv = gl_TessCoord.xy;
    gl_Position = mix(mix(gl_in[0].gl_Position, gl_in[2].gl_Position, uv.x),
                      mix(gl_in[1].gl_Position, gl_in[3].gl_Position, uv.x), uv.y);
    outColor = vec4(uv, 0.0, 1.0);
}
