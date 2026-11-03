// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// An integer clear with the exact bits: D3D12's ClearRenderTargetView takes floats, which cannot
// hold every integer past 2^24, and has no write mask.

#version 450 core

layout(push_constant) uniform PushConstants {
    uvec4 clear_color;
};

layout(location = 0) out uvec4 color;

void main() {
    color = clear_color;
}
