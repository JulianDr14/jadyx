#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 1) in vec2 inUv;
layout (location = 0) out vec4 outColor;
void main() { outColor = vec4(dFdx(inUv.x) * 8.0, dFdy(inUv.y) * 8.0, dFdy(inUv.x) * 8.0 + 0.5, 1.0); }
