#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Green channel gathers with texel offsets: the left half stays inside the texture, the right
// half reaches past its edge, where clamping applies to the offset footprint.
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() {
    outColor = gl_FragCoord.x < 16.0 ? textureGatherOffset(tex, vec2(0.5), ivec2(-1, 1), 1)
                                     : textureGatherOffset(tex, vec2(0.5), ivec2(2, -1), 1);
}
