// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARRAY_WORDS
#define ARRAY_WORDS 588
#endif
cbuffer Params : register(b0) { uint requested_n; uint requested_index; uint seed; uint unused; };
#if COMPUTE
RWStructuredBuffer<uint4> output_buffer : register(u1);
#endif
uint4 evaluate() {
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
#else
#if MODE == 8
    float data[ARRAY_WORDS];
    [loop] for(uint z=0;z<ARRAY_WORDS;z++) data[z]=asfloat(0u);
#define GET(i) asuint(data[i])
#define PUT(i,v) data[i]=asfloat(v)
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
[numthreads(1,1,1)] void main() {output_buffer[0]=evaluate();}
#else
uint4 main() : SV_Target0 {return evaluate();}
#endif
