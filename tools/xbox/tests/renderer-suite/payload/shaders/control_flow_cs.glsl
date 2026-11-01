#version 460
// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout (std140, binding = 0) uniform Input { ivec4 limits; };
layout (std430, binding = 0) buffer Block { int results[8]; };
int Fib(int n)
{
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) { int next = a + b; a = b; b = next; }
    return a;
}
void main()
{
    results[0] = Fib(limits.x);
    int total = 0;
    for (int i = 0; i < limits.y; ++i) {
        if (i % 2 == 0) continue;
        if (i > 7) break;
        total += i;
    }
    results[1] = total;
    int count = 0;
    do { ++count; } while (count < limits.z);
    results[2] = count;
    results[3] = 1;
    switch (limits.w) {
    case 0: results[3] = 10; break;
    case 1: results[3] = 20;
    case 2: results[3] += 5; break;
    default: results[3] = 40; break;
    }
    int nested = 0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (j == i) break;
            ++nested;
        }
    }
    results[4] = nested;
}
