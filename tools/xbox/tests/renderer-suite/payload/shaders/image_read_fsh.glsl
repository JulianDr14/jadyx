#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (binding = 0, rgba8) uniform readonly image2D img;
void main() { outColor = imageLoad(img, ivec2(gl_FragCoord.xy)); }
