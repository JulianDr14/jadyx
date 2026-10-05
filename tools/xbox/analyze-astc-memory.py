# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report measured ASTC resources separately from counterfactual replacement and process RAM."""
import argparse
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--log', type=Path, required=True)
parser.add_argument('--process', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
reports = []
for line in args.log.read_text(encoding='utf-8', errors='replace').splitlines():
    if 'D3D12: ASTC guest memory ' not in line:
        continue
    values = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
    required = {'payload', 'raw_alloc', 'baseline_native_alloc', 'fallback_native_alloc'}
    if not required.issubset(values):
        continue
    values['counterfactual_difference_bytes'] = (
        values['baseline_native_alloc'] - values['raw_alloc'] - values['fallback_native_alloc'])
    values['raw_and_companion_to_payload'] = (
        (values['raw_alloc'] + values['fallback_native_alloc']) / values['payload']
        if values['payload'] else None)
    reports.append(values)
result = {
    'scope': 'Measured live resource sizes; native replacement is counterfactual, not a process A/B.',
    'exclusions': ['driver/shader memory', 'retired resources', 'texture pool slack',
                   'draw metadata in staging', 'guest memory', 'other renderer resources'],
    'reports': len(reports),
    'latest': reports[-1] if reports else None,
    'latest_nonzero': next((v for v in reversed(reports) if v['payload']), None),
    'min_counterfactual_difference_bytes': min(
        (v['counterfactual_difference_bytes'] for v in reports), default=None),
    'max_counterfactual_difference_bytes': max(
        (v['counterfactual_difference_bytes'] for v in reports), default=None),
    'process': json.loads(args.process.read_text(encoding='utf-8-sig'))
        if args.process and args.process.exists() else None,
    'process_memory_savings_measured': False,
    'process_snapshot_complete': None,
    'series_verified': False,
}
if result['process'] is not None:
    result['process_snapshot_complete'] = result['process'].get('running') is False
args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: v for k, v in result.items() if k not in {'exclusions'}}, indent=2))
