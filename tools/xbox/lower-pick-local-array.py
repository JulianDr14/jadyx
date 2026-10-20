# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Private picking experiment: lower local-array accesses to scalar slots and balanced selection."""
import argparse
from pathlib import Path
import re


def lower(source):
    if source.count('alloca [588 x i32]') != 1:
        raise ValueError('Expected known 588-word local array')
    source = re.sub(r'%([0-9]+)\b', r'%v\1', source)
    source = re.sub(r'^; <label>:([0-9]+)[^\n]*$', r'v\1:', source, flags=re.M)
    start = source.index('define void @main() #0 {') + len('define void @main() #0 {')
    end = source.index('\n}', start)
    body = 'v0:\n' + source[start:end].lstrip('\n')
    pointers = {}
    for m in re.finditer(r'^  (%\w+) = getelementptr inbounds \[588 x i32\], \[588 x i32\]\* %v1, i32 0, i32 ([%\w-]+).*$', body, re.M):
        pointers[m[1]] = m[2]
    if len(pointers) != 19:
        raise ValueError(f'Unexpected pointer count {len(pointers)}')
    lines = []
    serial = 0
    for line in body.splitlines():
        if 'alloca [588 x i32]' in line:
            lines += [f'  %slot{i} = alloca i32, align 4' for i in range(588)]
            continue
        if 'getelementptr inbounds [588 x i32]' in line:
            continue
        load = re.fullmatch(r'  (%\w+) = load i32, i32\* (%\w+), align 4.*', line)
        store = re.fullmatch(r'  store i32 ([%\w-]+), i32\* (%\w+), align 4.*', line)
        if not load and not store:
            lines.append(line)
            continue
        pointer = load[2] if load else store[2]
        if pointer not in pointers:
            raise ValueError('Unexpected pointer user')
        index = pointers[pointer]
        if not index.startswith('%'):
            if not 0 <= int(index) < 588:
                raise ValueError('Constant out of bounds')
            lines.append(f'  {load[1]} = load i32, i32* %slot{index}, align 4' if load else
                         f'  store i32 {store[1]}, i32* %slot{index}, align 4')
            continue
        serial += 1
        prefix = f'local{serial}'
        if load:
            def tree(lo, hi):
                value = f'%{prefix}_value{lo}_{hi}'
                if hi - lo == 1:
                    lines.append(f'  {value} = load i32, i32* %slot{lo}, align 4')
                else:
                    mid = (lo + hi) // 2
                    left, right = tree(lo, mid), tree(mid, hi)
                    cond = f'%{prefix}_less{lo}_{hi}'
                    lines.append(f'  {cond} = icmp ult i32 {index}, {mid}')
                    lines.append(f'  {value} = select i1 {cond}, i32 {left}, i32 {right}')
                return value
            value = tree(0, 588)
            lines.append(f'  {load[1]} = or i32 {value}, 0')
        else:
            for i in range(588):
                old, cond, value = (f'%{prefix}_{part}{i}' for part in ('old', 'match', 'next'))
                lines.append(f'  {old} = load i32, i32* %slot{i}, align 4')
                lines.append(f'  {cond} = icmp eq i32 {index}, {i}')
                lines.append(f'  {value} = select i1 {cond}, i32 {store[1]}, i32 {old}')
                lines.append(f'  store i32 {value}, i32* %slot{i}, align 4')
    result = '\n'.join(lines)
    if 'getelementptr' in result:
        raise ValueError('Unexpected residual pointer operation')
    return source[:start] + '\n' + result + source[end:]


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    a.output.write_text(lower(a.source.read_text(encoding='utf8')), encoding='utf8')
