// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in Series experiment: the captured 5x5 picking selection without a local array.
// Series pixel shaders read zero from an indexable local array whenever the value is not
// forwarded in registers (local-memory matrix .24). Instead of storing a table of distinct
// IDs and their counts, each sample is re-fetched: sample i opens an entry when no earlier
// sample has an equal ID (float ==, as the table search), and that entry's count is 1 plus
// the later equal samples. Entries are visited in table order and the first strict maximum
// wins, so the chosen bits, minimum depth and RT1 zeros match the original program.
// Loop bounds are constant and every fetch is outside divergent control flow, so the
// implicit derivatives of Sample are those of the original.
Texture2D<float4> ids : register(t3);
Texture2D<float4> depths : register(t4);
SamplerState id_sampler : register(s3);
SamplerState depth_sampler : register(s4);
cbuffer PickConstants : register(b2) { float4 pick; };
struct Input { float4 a : TEXCOORD0; float4 b : TEXCOORD1; };
struct Output { float4 id : SV_Target0; float4 unused : SV_Target1; float4 depth : SV_Target2; };

float4 SampleId(Input input, int i) {
    const int x = i % 5 - 2;
    const int y = i / 5 - 2;
    return ids.Sample(id_sampler, float2(float(x * 2) * input.a.z + pick.x,
                                         float(y * 2) * input.a.w + pick.y));
}

Output main(Input input) {
    float minimum = 100000.0;
    int best = 0;
    uint4 chosen = 0;
    [loop] for (int i = 0; i < 25; ++i) {
        const int x = i % 5 - 2;
        const int y = i / 5 - 2;
        const float4 id = SampleId(input, i);
        const float depth = depths.Sample(depth_sampler, float2(float(x) * input.b.x + pick.x,
                                                                 float(y) * input.b.y + pick.y)).x;
        minimum = depth < minimum ? depth : minimum;
        const bool present = abs(id.x) + abs(id.y) + abs(id.z) + abs(id.w) != 0.0;
        bool first = true;
        int count = 1;
        [loop] for (int j = 0; j < 25; ++j) {
            const bool same = all(SampleId(input, j) == id);
            first = first && !(same && j < i);
            count += same && j > i ? 1 : 0;
        }
        const bool take = present && first && best < count;
        best = take ? count : best;
        chosen = take ? asuint(id) : chosen;
    }
    Output o;
    o.id = asfloat(chosen);
    o.unused = 0.0;
    o.depth = minimum;
    return o;
}
