#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Blue channel gather with four offsets (PTP): each component is the top-left texel of the
// footprint its offset moves to.
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() {
    outColor = textureGatherOffsets(tex, vec2(0.5),
                                    ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(-1, 1), ivec2(1, 1)), 2);
}
