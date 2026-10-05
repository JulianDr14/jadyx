# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Targeted geometry diagnostic for captured VS b4cf3ca379e7cfbe.

Position operations are transcribed from the captured DXIL, not guessed from
matrix layout. This estimates clipping/coverage; it is not a hardware replay.
"""
import argparse
import math
import struct
from pathlib import Path


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def transform(rows, p):
    result = []
    for r in rows:
        value = f32(f32(f32(p[0]*r[0])+f32(p[1]*r[1]))+f32(p[2]*r[2]))
        result.append(f32(value+r[3]))
    return result


def clip(poly):
    for axis, sign, w in [(0,1,1),(0,-1,1),(1,1,1),(1,-1,1),(2,1,0),(2,-1,1)]:
        output=[]
        for a,b in zip(poly, poly[1:]+poly[:1]):
            da=sign*a[axis]+w*a[3]; db=sign*b[axis]+w*b[3]
            if da>=0: output.append(a)
            if (da>=0)!=(db>=0):
                t=da/(da-db)
                output.append([x+(y-x)*t for x,y in zip(a,b)])
        poly=output
        if not poly: break
    return poly


def run(directory, draw):
    prefix=f'astc-capture-9112b4082b675746-draw{draw}'
    def data(label): return (directory/(prefix+'-'+label+'.bin')).read_bytes()
    def floats(label):
        b=data(label); return struct.unpack('<'+'f'*(len(b)//4),b)
    c0=floats('buffer3-kind2-slot0')
    model=floats('buffer4-kind2-slot1')
    vertices=data('buffer1-kind3-slot0')
    indices=struct.unpack('<315H', data('buffer0-kind4-slot0'))
    model_rows=[model[i:i+4] for i in (0,4,8)]
    view_rows=[c0[i:i+4] for i in (0,4,8)]
    projection=[c0[i:i+4] for i in (28,32,36,40)]
    positions={}
    for index in set(indices):
        p=struct.unpack_from('<3f',vertices,(index+329)*12)
        world=transform(model_rows,p)
        view=transform(view_rows,world)
        x,y,z,w=transform(projection,view)
        positions[index]=[x,y,f32(f32(z+w)*0.5),w]
    visible=0; area=0; clockwise=0; counterclockwise=0
    for at in range(0,len(indices),3):
        polygon=clip([positions[i] for i in indices[at:at+3]])
        if len(polygon)<3 or any(p[3]==0 for p in polygon): continue
        xy=[(p[0]/p[3]*32,p[1]/p[3]*32) for p in polygon]
        signed=sum(x*y2-y*x2 for (x,y),(x2,y2) in zip(xy,xy[1:]+xy[:1]))/2
        if abs(signed)>1e-8:
            visible+=1; area+=abs(signed)
            clockwise+=signed<0; counterclockwise+=signed>0
    print(dict(draw=draw,triangles=105,unclipped_w_range=[min(p[3] for p in positions.values()),max(p[3] for p in positions.values())],
               clip_survivors=visible,clockwise=clockwise,counterclockwise=counterclockwise,
               summed_area_pixels=area,finite=all(math.isfinite(v) for p in positions.values() for v in p),
               limitation='triangle-list assumed; excludes culling/depth/scissor/quad rules'))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    for draw in (2,3): run(args.directory,draw)
