#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) flat in vec4 inColor;
layout (location = 0) out vec4 outColor;
void main() { outColor = inColor; }
