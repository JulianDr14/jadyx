#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) noperspective in float inLinear;
layout (location = 1) in float inPerspective;
layout (location = 0) out vec4 outColor;
void main() { outColor = vec4(inLinear, inPerspective, 0.0, 1.0); }
