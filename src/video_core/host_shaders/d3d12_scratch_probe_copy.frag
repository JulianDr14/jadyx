// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Writes the result of d3d12_scratch_probe.vert to the probe's target.

#version 460 core

layout(location = 0) flat in uvec2 result;

layout(location = 0) out uvec4 color;

void main() {
    color = uvec4(result, 0u, 0u);
}
