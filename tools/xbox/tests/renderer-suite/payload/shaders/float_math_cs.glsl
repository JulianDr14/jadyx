#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout (std140, binding = 0) uniform Input { vec4 a; };
layout (std430, binding = 0) buffer Block { float results[32]; };
void main()
{
    int exponent;
    results[0] = floor(a.x);
    results[1] = ceil(a.x);
    results[2] = trunc(a.x);
    results[3] = round(a.y);
    results[4] = roundEven(a.y);
    results[5] = fract(a.x);
    results[6] = mod(a.x, a.y);
    results[7] = sqrt(a.y);
    results[8] = inversesqrt(a.y);
    results[9] = pow(a.y, 2.5);
    results[10] = exp2(a.y);
    results[11] = log2(a.y);
    results[12] = sin(a.y);
    results[13] = cos(a.y);
    results[14] = atan(a.y, a.z);
    results[15] = mix(a.x, a.y, 0.25);
    results[16] = smoothstep(0.0, 4.0, a.y);
    results[17] = step(a.y, a.x);
    results[18] = clamp(a.x, -1.0, 1.0);
    results[19] = min(a.x, a.y);
    results[20] = max(a.x, a.y);
    results[21] = fma(a.x, a.y, a.z);
    results[22] = frexp(a.y, exponent);
    results[23] = float(exponent);
    results[24] = ldexp(a.y, 3);
    results[25] = dot(a.xy, a.zw);
    results[26] = length(a.yz);
    results[27] = cross(a.xyz, a.yzw).x;
    results[28] = normalize(a.yz).x;
    results[29] = abs(a.x);
    results[30] = sign(a.x);
    results[31] = uintBitsToFloat(0x3FC00000u);
}
