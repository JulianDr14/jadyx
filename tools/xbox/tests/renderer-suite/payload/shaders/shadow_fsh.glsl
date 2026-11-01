#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 1) in vec2 inUv;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2DShadow tex;
layout (std140, binding = 0) uniform Block { vec4 reference; };
void main() { outColor = vec4(vec3(texture(tex, vec3(inUv, reference.x))), 1.0); }
