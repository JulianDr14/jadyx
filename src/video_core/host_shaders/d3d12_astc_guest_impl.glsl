// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

vec3 GuestSRGB(vec3 c) {
    return mix(pow((c+0.055)/1.055,vec3(2.4)),c/12.92,lessThanEqual(c,vec3(0.04045)));
}
int GuestAddress(int p,int size,uint mode) {
    if(mode==1u) return p>=0?p%size:size-1-((-1-p)%size); // D3D WRAP
    if(mode==2u) { // MIRROR: integer addressing, including negative periods
        int period=size*2;
        int q=p>=0?p%period:period-1-((-1-p)%period);
        return q<size?q:period-1-q;
    }
    if(mode==3u) return clamp(p,0,size-1);
    return p; // border; unsupported modes are kept on native sampling
}
uint GuestSwizzle(uvec2 p) {
    return ((p.x&32u)<<3u)|((p.y&6u)<<5u)|((p.x&16u)<<1u)|((p.y&1u)<<4u)|(p.x&15u);
}
vec4 GuestLoad(usamplerBuffer source,uint level,ivec3 p,bool fetch) {
    uvec4 view=guest_draw[eden_astc_slot*6u];
    uvec4 shape=guest_draw[eden_astc_slot*6u+1u];
    uvec4 address=guest_draw[eden_astc_slot*6u+2u];
    if (shape.x == 0u || shape.y < 4u || shape.z < 4u || shape.y > 12u || shape.z > 12u)
        return vec4(1,0,1,1);
    if (textureSize(source) < 4) return vec4(1,0,1,1);
    uint mip=level+view.y;
    uvec4 header=texelFetch(source,0);
    if(mip>=header.x || mip>=32u || 3u+mip*3u>=uint(textureSize(source))) return vec4(0);
    uvec4 size=texelFetch(source,int(1u+mip*3u));
    uvec4 layout0=texelFetch(source,int(2u+mip*3u));
    uvec4 layout1=texelFetch(source,int(3u+mip*3u));
    if (size.x == 0u || size.y == 0u || size.x > 32768u || size.y > 32768u)
        return vec4(1,0,1,1);
    if(!fetch) {
        p.x=GuestAddress(p.x,int(size.x),address.x);
        p.y=GuestAddress(p.y,int(size.y),address.y);
    }
    if(any(lessThan(p,ivec3(0))) || p.x>=int(size.x) || p.y>=int(size.y) || p.z>=int(shape.x))
        return fetch?vec4(0):uintBitsToFloat(guest_draw[eden_astc_slot*6u+4u]);
    p.z+=int(view.w);
    uvec2 b=uvec2(p.xy)/shape.yz;
    uvec2 pos=uvec2(b.x*16u,b.y);
    uint by=pos.y>>3u;
    uint offset=uint(p.z)*layout0.y+(by>>layout1.x)*layout0.z;
    offset+=(by&layout1.y)<<9u;
    offset+=(pos.x>>6u)<<layout0.w;
    offset+=GuestSwizzle(pos);
    uint index=header.y+layout0.x/16u+offset/16u;
    if(index>=header.y+header.z || index>=uint(textureSize(source))) return vec4(1,0,1,1);
    block_dims=shape.yz;
    requested_texel=uvec2(p.xy)%block_dims;
    local_buff=texelFetch(source,int(index));
    total_bitsread=0; color_bitsread=0; result_index=0;
    result_limit_reached=false; result_vector_max_index=0;
    color_endpoint_data=uvec4(0); sampled_color=vec4(0);
    decoder_srgb=guest_draw[eden_astc_slot*6u+5u].x!=0u;
    DecompressBlock(ivec3(0));
    vec4 color=sampled_color;
    if(decoder_srgb) color.rgb=GuestSRGB(color.rgb);
    vec4 mapped;
    for(uint c=0u;c<4u;c++) {
        uint component=(shape.w>>(c*3u))&7u;
        mapped[c]=component<4u?color[component]:(component==5u?1.0:0.0);
    }
    return mapped;
}
vec4 EdenAstcGuest(usamplerBuffer source) {
    uvec4 view=guest_draw[eden_astc_slot*6u];
    if(view.x==0u) return eden_astc_native;
    if (view.z == 0u || view.z > 32u || textureSize(source) < 4) return vec4(1,0,1,1);
    // Avoid undefined float-to-integer conversion before any address or mip calculation.
    if (any(isnan(eden_astc_coords)) || any(isinf(eden_astc_coords)) ||
        isnan(eden_astc_lod.x)) return vec4(0);
    uint last=view.z-1u;
    bool fetch=eden_astc_operation==2u;
    if(fetch) {
        if(eden_astc_lod.x<0.0 || eden_astc_lod.x>float(last)) return vec4(0);
    }
    if(eden_astc_operation==3u) {
        if(eden_astc_lod.x<0.0 || eden_astc_lod.x>float(last)) return uintBitsToFloat(uvec4(0));
        uvec4 size=texelFetch(source,int(1u+(uint(eden_astc_lod.x)+view.y)*3u));
        size.z=guest_draw[eden_astc_slot*6u+1u].x;
        return uintBitsToFloat(size);
    }
    if(eden_astc_operation==4u) return uintBitsToFloat(uvec4(view.z));
    uvec4 filters=guest_draw[eden_astc_slot*6u+2u];
    vec4 lods=uintBitsToFloat(guest_draw[eden_astc_slot*6u+3u]);
    float lambda=eden_astc_lod.x;
    if(eden_astc_operation==0u) {
#ifdef D3D12_ASTC_FRAGMENT
        vec2 dim=vec2(texelFetch(source,int(1u+view.y*3u)).xy);
        vec2 dx=dFdx(eden_astc_coords.xy)*dim;
        vec2 dy=dFdy(eden_astc_coords.xy)*dim;
        lambda=log2(max(max(length(dx),length(dy)),1e-20))+lods.y+eden_astc_lod.y;
#else
        lambda=0.0;
#endif
    }
    lambda=max(lambda,eden_astc_lod.z);
    lambda=clamp(lambda,lods.z,lods.w);
    bool linear=!fetch && (lambda<=0.0?filters.w!=0u:filters.z!=0u);
    float lod=clamp(lambda,0.0,float(last));
    bool mip_linear=!fetch && floatBitsToUint(lods.x)!=0u;
    uint low=fetch?uint(eden_astc_lod.x):uint(floor(lod+(mip_linear?0.0:0.5)));
    uint high=min(low+1u,last);
    uvec4 shape=guest_draw[eden_astc_slot*6u+1u];
    int layer=fetch?int(eden_astc_coords.z):clamp(int(floor(eden_astc_coords.z+0.5)),0,int(shape.x)-1);
    vec4 values[2];
    // A single GuestLoad call site for fetch, point, bilinear and both mips. Mesa inlines
    // functions; separate branches previously duplicated the entire ASTC decoder in each call.
    for(uint mt=0u;mt<2u;mt++) {
        if (mt != 0u && !mip_linear) break;
        uint level=mt==0u?low:high;
        uvec4 size=texelFetch(source,int(1u+(level+view.y)*3u));
        vec2 p=fetch?eden_astc_coords.xy:eden_astc_coords.xy*vec2(size.xy)+eden_astc_offset.xy;
        if(linear) p-=0.5;
        ivec2 base=fetch?ivec2(p):ivec2(floor(p));
        vec2 f=fract(p);
        vec4 taps[4];
        for(int t=0;t<4;t++) {
            if (t != 0 && !linear) break;
            taps[t]=GuestLoad(source,level,ivec3(base+ivec2(t&1,t>>1),layer),fetch);
        }
        values[mt]=linear?mix(mix(taps[0],taps[1],f.x),mix(taps[2],taps[3],f.x),f.y):taps[0];
    }
    return mip_linear?mix(values[0],values[1],fract(lod)):values[0];
}
void main() {
    eden_astc_slot=0u; eden_astc_coords=vec4(0); eden_astc_lod=vec4(0);
    eden_astc_operation=0u; eden_astc_native=vec4(0);
#ifdef D3D12_ASTC_FRAGMENT
    guest_dummy_output=EdenAstcGuest(guest_dummy);
#else
    // Keep the export reachable for glslang; this dummy entry point is discarded by the importer.
    sampled_color=EdenAstcGuest(guest_dummy);
#endif
}
