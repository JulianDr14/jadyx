// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Capture-only control. Compile 25 fixed offsets to avoid a new runtime binding.
Texture2D<float4> ids : register(t3);
Texture2D<float4> depth_input : register(t4);
SamplerState id_sampler : register(s3);
SamplerState depth_sampler : register(s4);
cbuffer PickConstants : register(b2) { float4 pick; };
struct Input { float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 p : SV_Position; };
struct Output { float4 id : SV_Target0; float4 depth_coords : SV_Target1; float4 varyings : SV_Target2; };
Output main(Input input) {
    Output o;
    float2 offset = float2(OFFSET_X, OFFSET_Y);
    float2 uv_id = pick.xy + offset * 2 * input.a.zw;
    float2 uv_depth = pick.xy + offset * input.b.xy;
    o.id = ids.Sample(id_sampler, uv_id);
    o.depth_coords = float4(depth_input.Sample(depth_sampler, uv_depth).r, uv_id, pick.x);
    o.varyings = float4(input.a.zw, input.b.xy);
    return o;
}
