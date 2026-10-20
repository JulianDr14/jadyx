# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Revision 27 local-memory matrix: size threshold, register-pressure spills and candidate
# storages (select registers, UAV ring) for pixel shaders. Series has no ROVs and its driver
# never finishes a PSO with 1024 live values, so spill cases stop at 256.
# prepare: compile DXC cases and Mesa SPIR-V; annotate (after the Mesa translate step):
# record per-case DXIL alloca/size so each Series result can be read against its code.
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('command',choices=('prepare','annotate'));p.add_argument('output',type=Path)
p.add_argument('--dxc',required=True);p.add_argument('--glslang');a=p.parse_args()
source=Path(__file__).parent/'tests/local-memory-matrix.hlsl';seed=0x3f123456
def chain_reduce(live,varying=0,index=200,n=5):
    m=0xffffffff;x=seed^index^varying;values=[]
    for _ in range(live):x=(x*1664525+1013904223)&m;x^=x>>13;values.append(x)
    acc=n
    for v in reversed(values):acc=((acc*31)&m)^v
    return acc
def cases():
    selection=[1,seed,25,0]
    for words in (202,588):
        for storage,label,compilers in ((0,'private',('dxc_o3','mesa')),(2,'select_registers',('dxc_o3',)),
                                        (3,'uav_ring',('dxc_o3','dxc_od'))):
            # 588 select registers would be 588 live values: far past the spill limit.
            if storage==2 and words==588: continue
            for compiler in compilers:
                yield dict(stage='pixel',mode=9,words=words,storage=storage,compiler=compiler,
                           name=f'selection_{label}_{words}',params=[5,200,seed,0],expected=selection)
    for mode,name in ((20,'sweep_dynamic_store'),(25,'sweep_dynamic_load')):
        for words in (4,8,16,32,64,128,256,588):
            for compiler in ('dxc_o3','mesa'):
                yield dict(stage='pixel',mode=mode,words=words,storage=0,compiler=compiler,name=f'{name}_{words}',
                           params=[5,words-1,seed,0],expected=[seed,seed+7,words-1,5])
    for live in (64,128,192,256):
        yield dict(stage='pixel',mode=21,words=live,storage=0,compiler='dxc_o3',name=f'spill_{live}',
                   params=[5,200,seed,0],expected=[chain_reduce(live),seed+7,200,5])
    yield dict(stage='compute',mode=21,words=256,storage=0,compiler='dxc_o3',name='spill_256',
               params=[5,200,seed,0],expected=[chain_reduce(256),seed+7,200,5])
    yield dict(stage='pixel',mode=20,words=588,storage=3,compiler='dxc_o3',name='sweep_dynamic_store_uav_ring_588',
               params=[5,587,seed,0],expected=[seed,seed+7,587,5])
    yield dict(stage='compute',mode=9,words=588,storage=0,compiler='dxc_o3',name='selection_private_588',
               params=[5,200,seed,0],expected=selection)
    yield dict(stage='compute',mode=20,words=588,storage=0,compiler='dxc_o3',name='sweep_dynamic_store_588',
               params=[5,587,seed,0],expected=[seed,seed+7,587,5])
def prepare():
    a.output.mkdir(parents=True,exist_ok=True);entries=[]
    subprocess.run([a.dxc,'-T','vs_6_4','-E','main','-Fo',str(a.output/'vs.dxil'),str(source.with_name('local-memory-matrix-vs.hlsl'))],check=True)
    for c in cases():
        compute=int(c['stage']=='compute');base=f"{c['stage']}_{c['name']}_{c['compiler']}";file=base+'.dxil'
        defines=[f"MODE={c['mode']}",f'COMPUTE={compute}',f"ARRAY_WORDS={c['words']}",f"STORAGE={c['storage']}"]
        entry=dict(name=base,stage=c['stage'],compiler=c['compiler'],file=file,params=c['params'],expected=c['expected'],
                   mode=c['mode'],words=c['words'],storage=c['storage'])
        if c['compiler']=='mesa':
            entry['spv']=base+'.spv'
            subprocess.run([a.glslang,'-D','-V','--target-env','vulkan1.1','-S','comp' if compute else 'frag','-e','main',
                            *('-D'+d for d in defines),'-Od','--auto-map-locations','-o',str(a.output/entry['spv']),str(source)],
                           check=True,stdout=subprocess.DEVNULL)
        else:
            subprocess.run([a.dxc,'-T','cs_6_4' if compute else 'ps_6_4','-E','main',*sum((['-D',d] for d in defines),[]),
                            '-O3' if c['compiler']=='dxc_o3' else '-Od','-Fo',str(a.output/file),str(source)],check=True)
        entries.append(entry)
    (a.output/'manifest.json').write_text(json.dumps(entries,indent=2),encoding='utf8')
    (a.output/'enabled.flag').write_text('automatic matrix\n')
    print('Prepared',len(entries),'matrix cases')
def annotate():
    path=a.output/'manifest.json';entries=json.loads(path.read_text(encoding='utf8'))
    for e in entries:
        text=subprocess.run([a.dxc,'-dumpbin',str(a.output/e['file'])],check=True,capture_output=True,text=True).stdout
        e['dxil_alloca']=' alloca ' in text;e['dxil_bytes']=(a.output/e['file']).stat().st_size
    path.write_text(json.dumps(entries,indent=2),encoding='utf8')
    for e in entries:print(f"{e['name']:52} alloca={e['dxil_alloca']!s:5} bytes={e['dxil_bytes']}")
prepare() if a.command=='prepare' else annotate()
