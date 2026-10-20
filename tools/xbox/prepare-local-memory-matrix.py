# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--dxc',required=True);p.add_argument('--glslang',required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
source=Path(__file__).parent/'tests/local-memory-matrix.hlsl';seed=0x3f123456;entries=[]
subprocess.run([a.dxc,'-T','vs_6_4','-E','main','-Fo',str(a.output/'vs.dxil'),str(source.with_name('local-memory-matrix-vs.hlsl'))],check=True)
names=['scalar','constant_array','dynamic_store','dynamic_load','dynamic_both','array_loop_counter','nested_loop_ssa','selection_uint_array','selection_float_bits','selection_separate_counter','selection_no_array_single','selection_no_array_multiple']
for stage,profile,spirvstage,compute in [('pixel','ps_6_4','frag',0),('compute','cs_6_4','comp',1)]:
 for mode,name,words in [*( (i,name,588) for i,name in enumerate(names)),(4,"dynamic_both_201",201),(4,"dynamic_both_1024",1024)]:
  expected=([seed,seed+7,200,5] if mode<=4 else [5,15,0,seed] if mode==5 else [25,300,24,seed] if mode==6 else [5 if mode==11 else 1,seed,25,0])
  for compiler in ('dxc_o3','dxc_od','mesa'):
   base=f'{stage}_{name}_{compiler}';file=base+'.dxil'
   common=['-E','main','-D',f'MODE={mode}','-D',f'COMPUTE={compute}','-D',f'ARRAY_WORDS={words}']
   if compiler=='mesa':
    spv=base+'.spv'
    subprocess.run([a.glslang,'-D','-V','--target-env','vulkan1.1','-S',spirvstage,'-e','main',f'-DMODE={mode}',f'-DCOMPUTE={compute}',f'-DARRAY_WORDS={words}','-Od','--auto-map-locations','-o',str(a.output/spv),str(source)],check=True,stdout=subprocess.DEVNULL)
   else:
    subprocess.run([a.dxc,'-T',profile,*common,'-O3' if compiler=='dxc_o3' else '-Od','-Fo',str(a.output/file),str(source)],check=True)
   entry=dict(name=base,stage=stage,compiler=compiler,file=file,params=[5,200,seed,5 if mode==11 else 0],expected=expected)
   if compiler=='mesa':entry['spv']=spv
   entries.append(entry)
(a.output/'manifest.json').write_text(json.dumps(entries,indent=2),encoding='utf8');(a.output/'enabled.flag').write_text('automatic matrix\n')
print('Prepared',len(entries),'matrix cases')
