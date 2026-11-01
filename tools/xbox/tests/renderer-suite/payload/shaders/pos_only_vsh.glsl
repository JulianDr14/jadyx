#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) in vec3 inPos;
void main() { gl_Position = vec4(inPos, 1.0); }
