#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform usampler2D tex;
void main() { outColor = vec4(texelFetch(tex, ivec2(gl_FragCoord.xy), 0)) / 255.0; }
