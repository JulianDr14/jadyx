#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout (std140, binding = 0) uniform Input { uvec4 a; ivec4 b; };
layout (std430, binding = 0) buffer Block { uint results[32]; };
void main()
{
    uint high, low, carry, borrow;
    umulExtended(a.x, a.y, high, low);
    results[0] = high;
    results[1] = low;
    results[2] = uaddCarry(a.x, a.x, carry);
    results[3] = carry;
    results[4] = usubBorrow(a.y, a.x, borrow);
    results[5] = borrow;
    results[6] = bitfieldExtract(a.x, 8, 12);
    results[7] = uint(bitfieldExtract(b.x, 4, 8));
    results[8] = bitfieldInsert(a.x, a.y, 4, 8);
    results[9] = bitfieldReverse(a.x);
    results[10] = uint(bitCount(a.x));
    results[11] = uint(findLSB(a.y));
    results[12] = uint(findMSB(a.x));
    results[13] = uint(findMSB(b.x));
    // The compiler divides through a float reciprocal: the numerator stays within 24 bits.
    results[14] = a.y / a.z;
    results[15] = a.y % a.z;
    results[16] = uint(b.x / b.y);
    results[17] = uint(b.x >> 3);
    results[18] = a.x >> 3;
    results[19] = a.x << 5;
    results[20] = uint(min(b.x, b.y));
    results[21] = uint(max(b.x, b.y));
    results[22] = uint(abs(b.x));
    results[23] = uint(sign(b.x));
    results[24] = ~a.x;
    results[25] = a.x ^ a.y;
    results[26] = uint(clamp(b.x, -5, 5));
    results[27] = packUnorm4x8(vec4(0.0, 0.25, 0.5, 1.0));
    results[28] = packSnorm2x16(vec2(-1.0, 0.5));
    results[29] = packHalf2x16(vec2(1.5, -2.0));
    results[30] = uint(int(-3.75 * a.w));
    results[31] = uint(3.75 * float(a.w));
}
