// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARRAY_WORDS
#define ARRAY_WORDS 588
#endif
// STORAGE selects how the array modes keep their words:
// 0 private array (alloca), 2 registers with select chains (no alloca),
// 3 per-lane UAV scratch from a wave ring, 4 per-pixel rasterizer-ordered UAV scratch.
#ifndef STORAGE
#define STORAGE 0
#endif
cbuffer Params : register(b0) { uint requested_n; uint requested_index; uint seed; uint unused; };
#if COMPUTE
RWStructuredBuffer<uint4> output_buffer : register(u1);
#elif STORAGE == 3
RWByteAddressBuffer scratch : register(u1);
#elif STORAGE == 4
RasterizerOrderedByteAddressBuffer scratch : register(u1);
#endif
#define SCRATCH_COUNTER 16
#define SCRATCH_BASE 256
#define SCRATCH_SLOTS 64
#if STORAGE == 2
uint SelectGet(uint data[ARRAY_WORDS], uint i) {
    uint r = 0;
    [unroll] for (uint j = 0; j < ARRAY_WORDS; j++) r = j == i ? data[j] : r;
    return r;
}
void SelectPut(inout uint data[ARRAY_WORDS], uint i, uint v) {
    [unroll] for (uint j = 0; j < ARRAY_WORDS; j++) data[j] = j == i ? v : data[j];
}
#endif
uint4 evaluate(uint varying) {
    uint n=min(requested_n,5u);
    uint index=min(requested_index,200u);
#if MODE == 0
    return uint4(seed, seed+7, index, n);
#elif MODE == 10 || MODE == 11
    uint total=n*n, chosen=0,best=0,minimum=100000;
    uint modulus=max(unused,1u);
    [loop] for(uint i=0;i<total;i++) {
        uint candidate=seed+i%modulus,count=0;
        [loop] for(uint j=0;j<total;j++) if(seed+j%modulus==candidate)count++;
        if(count>best){best=count;chosen=candidate;}
        minimum=min(minimum,i);
    }
    return uint4(min(total,modulus),chosen,total,minimum);
#elif MODE == 6
    uint count=0, sum=0;
    [loop] for(uint y=0;y<n;y++) [loop] for(uint x=0;x<n;x++) {sum+=y*n+x;count++;}
    return uint4(count,sum,n*n-1,seed);
#elif MODE == 21
    // Register pressure without any array access: ARRAY_WORDS chained per-lane values are
    // consumed in reverse order, so the driver must keep or spill all of them.
    uint v[ARRAY_WORDS];
    uint x=seed^index^varying;
    [unroll] for(uint i=0;i<ARRAY_WORDS;i++) {x=x*1664525u+1013904223u;x^=x>>13;v[i]=x;}
    uint acc=n;
    [unroll] for(int r=ARRAY_WORDS-1;r>=0;r--) acc=(acc*31u)^v[r];
    return uint4(acc,seed+7,index,n);
#else
#if MODE == 8
    float data[ARRAY_WORDS];
    [loop] for(uint z=0;z<ARRAY_WORDS;z++) data[z]=asfloat(0u);
#define GET(i) asuint(data[i])
#define PUT(i,v) data[i]=asfloat(v)
#elif STORAGE == 2
    uint data[ARRAY_WORDS];
    [unroll] for(uint z=0;z<ARRAY_WORDS;z++) data[z]=0;
#define GET(i) SelectGet(data,i)
#define PUT(i,v) SelectPut(data,i,v)
#elif STORAGE == 3 || STORAGE == 4
#if STORAGE == 3
    uint base=0;
    if(WaveIsFirstLane()) scratch.InterlockedAdd(SCRATCH_COUNTER,WaveGetLaneCount(),base);
    base=WaveReadLaneFirst(base);
    const uint slot=(base+WaveGetLaneIndex())%SCRATCH_SLOTS;
#else
    const uint slot=varying%SCRATCH_SLOTS;
#endif
    const uint origin=SCRATCH_BASE+slot*ARRAY_WORDS*4;
    [loop] for(uint z=0;z<ARRAY_WORDS;z++) scratch.Store(origin+z*4,0u);
#define GET(i) scratch.Load(origin+min(uint(i),ARRAY_WORDS-1u)*4)
#define PUT(i,v) scratch.Store(origin+min(uint(i),ARRAY_WORDS-1u)*4,v)
#else
    uint data[ARRAY_WORDS];
    [loop] for(uint z=0;z<ARRAY_WORDS;z++) data[z]=0;
#define GET(i) data[i]
#define PUT(i,v) data[i]=v
#endif
#if MODE == 1
    PUT(200,seed); return uint4(GET(200),seed+7,index,n);
#elif MODE == 2
    PUT(index,seed); return uint4(GET(200),seed+7,index,n);
#elif MODE == 3
    PUT(200,seed); return uint4(GET(index),seed+7,index,n);
#elif MODE == 4
    PUT(index,seed); return uint4(GET(index),seed+7,index,n);
#elif MODE == 20
    // Size sweep, dynamic store and constant load: never forwardable in registers.
    uint w=min(requested_index,ARRAY_WORDS-1u);
    PUT(w,seed); return uint4(GET(ARRAY_WORDS-1),seed+7,w,n);
#elif MODE == 25
    // Size sweep, constant store and dynamic load.
    uint w=min(requested_index,ARRAY_WORDS-1u);
    PUT(ARRAY_WORDS-1,seed); return uint4(GET(w),seed+7,w,n);
#elif MODE == 5
    PUT(index,n); uint count=0,sum=0;
    [loop] while(GET(200)>0 && count<8) {sum+=GET(200);PUT(index,GET(200)-1);count++;}
    return uint4(count,sum,GET(200),seed);
#else
    uint distinct=0,iterations=0,minimum=100000,outer=n;
    PUT(index,n);
    [loop] for(uint row=0;row<n;row++) {
#if MODE == 7 || MODE == 8
        if(GET(200)==0) break;
#endif
        [loop] for(uint col=0;col<n;col++) {
            uint found=25;
            [loop] for(uint k=0;k<distinct;k++) {if(GET(k*4)==seed){found=k;break;}}
            if(found==25) {PUT(distinct*4,seed);PUT(100+distinct*4,1);distinct++;}
            else {PUT(100+found*4,GET(100+found*4)+1);}
            minimum=min(minimum,row*n+col);iterations++;
        }
#if MODE == 7 || MODE == 8
        PUT(index,GET(200)-1);
#else
        outer--;
#endif
    }
    uint chosen=0,best=0;
    [loop] for(uint k=0;k<distinct;k++) {if(GET(100+k*4)>best){best=GET(100+k*4);chosen=GET(k*4);}}
    return uint4(distinct,chosen,iterations,minimum);
#endif
#endif
}
#if COMPUTE
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {output_buffer[0]=evaluate(id.x);}
#else
// The 1x1 target covers pixel (0,0) only, so varying is 0 at runtime but per-lane to the compiler.
uint4 main(float4 position : SV_Position) : SV_Target0 {
    return evaluate(uint(position.x)+uint(position.y));
}
#endif
