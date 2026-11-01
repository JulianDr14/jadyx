#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 4, local_size_y = 4, local_size_z = 1) in;
layout (binding = 0, r32ui) uniform uimage2D img;
void main() { imageAtomicAdd(img, ivec2(0, 0), 1u); }
