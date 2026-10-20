# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare private capture controls. Guest LLVM/DXIL stays outside the repository."""
import argparse, pathlib, re, subprocess, json
p=argparse.ArgumentParser();p.add_argument('llvm',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('--assembler',required=True);p.add_argument('--dxc',required=True);a=p.parse_args()
s=a.llvm.read_text(encoding='utf8');a.output.mkdir(parents=True,exist_ok=True)
if '%1 = alloca [588 x i32]' not in s or s.count('@dx.op.sample.f32(i32 60,')!=2:
 raise ValueError('Unexpected picking shader; inspect before instrumenting')
level=s.replace('@dx.op.sample.f32','@dx.op.sampleLevel.f32').replace('sampleLevel.f32(i32 60,','sampleLevel.f32(i32 62,')
level,n=re.subn(r'(@dx.op.sampleLevel.f32\(i32 62,[^\n]+), float undef\)',r'\1, float 0.000000e+00)',level)
assert n==2
counts=s.replace('  %227 = bitcast','  %probe_count = sitofp i32 %149 to float\n  %227 = bitcast',1)
for channel,value in enumerate(('%probe_count','%57','%227','%230')):
 pattern=rf'(@dx.op.storeOutput.f32\(i32 5, i32 1, i32 0, i8 {channel}, float )[^)]+(\))'
 counts,n=re.subn(pattern,lambda m:m[1]+value+m[2],counts);assert n==1
manifest=[dict(pass_id=0,name='original_unorm_unpredicated'),dict(pass_id=1,name='original_float_unpredicated')]
for i,text,name in ((2,level,'original_sample_level_zero'),(3,counts,'original_count_min_selected')):
 ll=a.output/f'{i}.ll';ll.write_text(text,encoding='utf8');subprocess.run([a.assembler,str(ll),str(a.output/f'{i}.dxil')],check=True)
 manifest.append(dict(pass_id=i,name=name))
source=pathlib.Path(__file__).parent/'tests/pick-samples.hlsl'
for y in range(-2,3):
 for x in range(-2,3):
  i=4+(y+2)*5+x+2
  subprocess.run([a.dxc,'-T','ps_6_4','-E','main','-D',f'OFFSET_X={x}','-D',f'OFFSET_Y={y}','-Fo',str(a.output/f'{i}.dxil'),str(source)],check=True)
  manifest.append(dict(pass_id=i,name='fixed_sample_id_depth_varyings',offset=[x,y]))
(a.output/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
(a.output/'enabled.flag').write_text('capture-only picking controls\n',encoding='utf8')
print('Prepared',len(manifest),'controls; original guest pass is unchanged')
