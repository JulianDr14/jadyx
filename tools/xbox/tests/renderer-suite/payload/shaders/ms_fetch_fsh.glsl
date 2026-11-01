#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2DMS tex;
layout (std140, binding = 0) uniform Block { ivec4 sample_index; };
void main() { outColor = texelFetch(tex, ivec2(gl_FragCoord.xy), sample_index.x); }
