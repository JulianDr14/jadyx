#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout (std140, binding = 0) uniform Input { vec4 first; uvec4 second; };
layout (std430, binding = 0) buffer Block { vec4 got_first; uvec4 got_second; };
void main() { got_first = first; got_second = second; }
