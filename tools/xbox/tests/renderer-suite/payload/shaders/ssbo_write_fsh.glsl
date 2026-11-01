#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out vec4 outColor;
layout (std430, binding = 0) buffer Block { uint count; };
void main() { atomicAdd(count, 1u); outColor = vec4(0, 1, 0, 1); }
