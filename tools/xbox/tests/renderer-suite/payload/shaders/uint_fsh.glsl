#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) out uvec4 outColor;
layout (std140, binding = 0) uniform Block { uvec4 value; };
void main() { outColor = value; }
