#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
void main() { outColor = gl_FrontFacing ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1); }
