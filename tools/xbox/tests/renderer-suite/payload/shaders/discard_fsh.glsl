#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inUv;
layout (location = 0) out vec4 outColor;
void main() { if (inUv.x < 0.5) discard; outColor = inColor; }
