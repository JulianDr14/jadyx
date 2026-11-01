#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform samplerCube tex;
layout (std140, binding = 0) uniform Block { vec4 direction; };
void main() { outColor = texture(tex, direction.xyz); }
