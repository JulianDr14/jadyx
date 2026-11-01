#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) in vec4 inColor;
layout (location = 0, index = 0) out vec4 outFirst;
layout (location = 0, index = 1) out vec4 outSecond;
void main() { outFirst = inColor; outSecond = vec4(0.25, 0.5, 0.75, 0.5); }
