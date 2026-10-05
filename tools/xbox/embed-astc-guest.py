# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embed a glslang module and its exact ID relocations using the Khronos grammar."""
import json
from pathlib import Path
import struct
import sys

binary, grammar, output, symbol = sys.argv[1:]
data = Path(binary).read_bytes()
words = struct.unpack('<' + 'I' * (len(data) // 4), data)
schema = json.loads(Path(grammar).read_text(encoding='utf-8'))
instructions = {i['opcode']: i for i in schema['instructions']}
kinds = {k['kind']: k for k in schema['operand_kinds']}
relocations = []

def operand(kind, pos, end):
    definition = kinds[kind]
    category = definition['category']
    if category == 'Id':
        relocations.append(pos)
        return pos + 1
    if category == 'Composite':
        for base in definition['bases']:
            pos = operand(base, pos, end)
        return pos
    if kind == 'LiteralString':
        while pos < end:
            value = words[pos]
            pos += 1
            if 0 in value.to_bytes(4, 'little'):
                return pos
        raise ValueError('Unterminated SPIR-V string')
    if category in ('ValueEnum', 'BitEnum'):
        value = words[pos]
        pos += 1
        seen = set()
        for enumerant in definition['enumerants']:
            ev = int(str(enumerant['value']), 0)
            if ev in seen:
                continue
            if (category == 'ValueEnum' and value == ev) or (category == 'BitEnum' and ev and value & ev == ev):
                seen.add(ev)
                for param in enumerant.get('parameters', []):
                    pos = operand(param['kind'], pos, end)
        return pos
    # This module uses only 32-bit literals; fail instead of guessing an unhandled layout.
    return pos + 1

position = 5
while position < len(words):
    count, opcode = words[position] >> 16, words[position] & 65535
    assert count > 0
    end = position + count
    cursor = position + 1
    for item in instructions[opcode].get('operands', []):
        quantifier = item.get('quantifier')
        if quantifier == '*':
            while cursor < end:
                cursor = operand(item['kind'], cursor, end)
        elif quantifier != '?' or cursor < end:
            cursor = operand(item['kind'], cursor, end)
    assert cursor == end, (opcode, cursor, end)
    position = end
assert position == len(words)
header = '// Generated from our shader and the Khronos SPIR-V grammar.\n#pragma once\n#include <array>\n#include <cstdint>\nnamespace D3D12::AstcGuestEmbedded {\n'
for name, values in ((symbol, words), (symbol + '_ids', relocations)):
    header += f'inline constexpr std::array<std::uint32_t, {len(values)}> {name}{{{{\n'
    for offset in range(0, len(values), 12):
        header += ','.join(str(v) + 'u' for v in values[offset:offset + 12]) + ',\n'
    header += '}};\n'
header += '}\n'
Path(output).write_text(header, encoding='utf-8', newline='\n')
