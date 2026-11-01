#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFirst;
layout (location = 1) out vec4 outSecond;
void main() { outFirst = inColor; outSecond = inColor.bgra; }
