#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (std140, binding = 0) uniform Block { vec4 values[4]; };
void main() { outColor = values[int(gl_FragCoord.x) / 8]; }
