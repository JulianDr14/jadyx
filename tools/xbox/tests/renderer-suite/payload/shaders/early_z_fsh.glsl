#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Counts only the fragments that passed depth, which it can only know with early tests.
layout (early_fragment_tests) in;
layout (location = 0) out vec4 outColor;
layout (std430, binding = 0) buffer Block { uint count; };
void main() { atomicAdd(count, 1u); outColor = vec4(0, 0, 1, 1); }
