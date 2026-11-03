// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// d3d12_clear_uint.frag for signed integer targets.

#version 450 core

layout(push_constant) uniform PushConstants {
    ivec4 clear_color;
};

layout(location = 0) out ivec4 color;

void main() {
    color = clear_color;
}
