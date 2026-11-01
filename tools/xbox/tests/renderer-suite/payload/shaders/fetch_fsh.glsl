#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
layout (std140, binding = 0) uniform Block { ivec4 place; };
void main() { outColor = texelFetch(tex, place.xy, place.z); }
