# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Private, validated LLVM controls for the known picking kernel. Not a production optimizer."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def simplify_forwarders(source):
    """Remove only single-predecessor, unconditional forwarding blocks; repair PHI edges."""
    source = re.sub(r'%([0-9]+)\b', r'%v\1', source)
    source = re.sub(r'^; <label>:([0-9]+)[^\n]*$', r'v\1:', source, flags=re.M)
    start = source.index('define void @main() #0 {') + len('define void @main() #0 {')
    end = source.index('\n}', start)
    body = 'v0:\n' + source[start:end].lstrip('\n')
    blocks = {}
    for match in re.finditer(r'^([A-Za-z_][\w.]*):\n(.*?)(?=^[A-Za-z_][\w.]*:\n|\Z)', body, re.M | re.S):
        blocks[match[1]] = match[2]
    removed = 0
    while True:
        predecessors = {name: [] for name in blocks}
        for name, text in blocks.items():
            for dest in re.findall(r'label %([\w.]+)', text):
                predecessors[dest].append(name)
        candidate = None
        for name, text in blocks.items():
            instructions = [line.split(';')[0].strip() for line in text.splitlines()]
            instructions = [line for line in instructions if line]
            if name == 'v0' or len(predecessors[name]) != 1 or len(instructions) != 1:
                continue
            branch = re.fullmatch(r'br label %([\w.]+)', instructions[0])
            if not branch:
                continue
            parent, target = predecessors[name][0], branch[1]
            if target == name or parent == name or parent in predecessors[target]:
                continue
            candidate = name, parent, target
            break
        if candidate is None:
            break
        name, parent, target = candidate
        blocks[parent] = re.sub(rf'label %{re.escape(name)}\b', 'label %' + target, blocks[parent])
        blocks[target] = re.sub(rf', %{re.escape(name)}\s*\]', ', %' + parent + ' ]', blocks[target])
        del blocks[name]
        removed += 1
    if not removed:
        raise ValueError('No eligible forwarders')
    body = '\n'.join(name + ':\n' + text.rstrip() for name, text in blocks.items())
    return source[:start] + '\n' + body + source[end:], removed


def controls(source):
    if source.count('alloca [588 x i32]') != 1 or '%probe_count = sitofp i32 %149 to float' not in source:
        raise ValueError('Expected the validated original count-output control')
    volatile = source.replace('load i32, i32*', 'load volatile i32, i32*').replace('store i32 ', 'store volatile i32 ')
    counter_ssa, n = re.subn(r'%159 = load i32, i32\* %158, align 4', '%159 = sub i32 3, %10', source)
    if n != 1:
        raise ValueError('Unexpected outer counter')
    separate = source.replace('  %1 = alloca [588 x i32], align 16',
                              '  %1 = alloca [588 x i32], align 16\n  %probe_counter = alloca i32, align 4')
    separate, n = re.subn(r'getelementptr inbounds \[588 x i32\], \[588 x i32\]\* %1, i32 0, i32 200',
                          'getelementptr inbounds i32, i32* %probe_counter, i32 0', separate)
    if n != 3:
        raise ValueError('Unexpected counter aliases')
    init = ''.join(f'  %probe_init{i} = getelementptr inbounds [588 x i32], [588 x i32]* %1, i32 0, i32 {i}\n'
                   f'  store i32 0, i32* %probe_init{i}, align 4\n' for i in range(588))
    initialized = source.replace('  %1 = alloca [588 x i32], align 16\n',
                                 '  %1 = alloca [588 x i32], align 16\n' + init)
    simplified, removed = simplify_forwarders(source)
    simplified_volatile, _ = simplify_forwarders(volatile)
    return [(29, 'volatile_all_local', volatile),
            (30, 'outer_counter_ssa_same_table', counter_ssa),
            (31, 'outer_counter_separate_alloca', separate),
            (32, 'volatile_local_loads', source.replace('load i32, i32*', 'load volatile i32, i32*')),
            (33, 'volatile_local_stores', source.replace('store i32 ', 'store volatile i32 ')),
            (34, 'zero_initialize_local_array', initialized),
            (35, 'local_array_201_words', source.replace('[588 x i32]', '[201 x i32]')),
            (36, 'local_array_1024_words', source.replace('[588 x i32]', '[1024 x i32]')),
            (37, f'forwarders_removed_{removed}_same_math_memory', simplified),
            (38, f'forwarders_removed_{removed}_volatile_local', simplified_volatile)]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assembler', required=True)
    args = parser.parse_args()
    manifest_path = args.output / 'manifest.json'
    manifest = [r for r in json.loads(manifest_path.read_text(encoding='utf8')) if r['pass_id'] < 29]
    for number, name, text in controls(args.source.read_text(encoding='utf8')):
        path = args.output / f'{number}.ll'
        path.write_text(text, encoding='utf8')
        subprocess.run([args.assembler, str(path), str(args.output / f'{number}.dxil')], check=True)
        manifest.append(dict(pass_id=number, name=name, comparison_baseline=3))
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf8')
    print('Prepared and validated', len(manifest), 'capture controls')
