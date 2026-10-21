# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile

spec = importlib.util.spec_from_file_location('full_capture', Path(__file__).parents[1] / 'analyze-full-capture.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
root = Path(sys.argv[1])
complete = module.load_capture(root / 'complete')
assert complete[1]['data_complete'] and complete[1]['unique_bytes'] == 8 + 3 * 65536
assert complete[2][1]['sha256'] == complete[2][2]['sha256']
assert complete[2][4]['sha256'] == complete[2][5]['sha256']
assert complete[2][5]['sha256'] != complete[2][6]['sha256']
assert not module.report(complete)['ready_for_gpu_comparison']  # Not a GPU fixture.
assert not module.load_capture(root / 'incomplete')[1]['data_complete']
concurrent = module.load_capture(root / 'concurrent')
assert concurrent[3]['worker'] == 400 and concurrent[1]['unique_bytes'] == 4
assert not module.load_capture(root / 'interrupted')[1]['data_complete']
fake = list(complete[2]) + [dict(kind='present', detail=dict(frame=1, width=10, height=10, format=28)),
                            dict(kind='dispatch_begin', detail=dict(op=99))]
fake_report = module.report((complete[0], complete[1], fake, complete[3]))
assert not fake_report['present_coverage_complete']
assert fake_report['unclosed_producers'] == [99]
for mode in ('checksum', 'checksum_word', 'truncate', 'index', 'missing_summary', 'summary_lie'):
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / 'capture'
        shutil.copytree(root / 'complete', target)
        if mode in ('checksum', 'checksum_word'):
            data = bytearray((target / 'blobs.bin').read_bytes())
            # Byte 0 is in a 4-byte tail-only chunk; byte 100 is inside a hashed 64-bit word.
            data[0 if mode == 'checksum' else 100] ^= 1
            (target / 'blobs.bin').write_bytes(data)
        elif mode == 'truncate':
            (target / 'blobs.bin').write_bytes(b'')
        elif mode == 'index':
            events = (target / 'events.jsonl').read_text().splitlines()
            del events[1]
            (target / 'events.jsonl').write_text('\n'.join(events) + '\n')
        elif mode == 'missing_summary':
            (target / 'summary.json').unlink()
        else:
            summary = json.loads((target / 'summary.json').read_text())
            summary['omissions'] = 1
            (target / 'summary.json').write_text(json.dumps(summary))
        try:
            module.load_capture(target)
        except (OSError, ValueError):
            pass
        else:
            raise AssertionError(f'Accepted invalid capture: {mode}')
print('PASS: exact checksums/dedup, pitch, concurrent records, incomplete captures and six corruption cases')

# A reference must resolve to earlier, complete image bytes, never to another
# reference or to a missing/partial snapshot. It contributes the original hashes.
for mode in ('valid', 'missing', 'partial', 'wrong_address'):
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / 'capture'
        shutil.copytree(root / 'complete', target)
        events = [json.loads(line) for line in (target / 'events.jsonl').read_text().splitlines()]
        view = dict(op=1, binding=0, after=False, storage=False, address=123)
        image = dict(op=1, after=False, role='view_0', subresource=0)
        data = dict(events[1]['detail'])
        data['context'] = dict(image=image)
        extra = [dict(kind='image_view', detail=view), dict(kind='image_data', detail=data),
                 dict(kind='image_view_reference', detail=dict(view=dict(view, op=2),
                      source_op=1, source_binding=0, subresources=1))]
        if mode == 'missing': extra[-1]['detail']['source_op'] = 99
        if mode == 'partial': extra[-1]['detail']['subresources'] = 2
        if mode == 'wrong_address': extra[-1]['detail']['view']['address'] = 999
        events[-1:-1] = extra
        for seq, event in enumerate(events):
            event['seq'] = seq
            event['us'] = seq
        (target / 'events.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in events))
        summary = json.loads((target / 'summary.json').read_text())
        summary['events'] = len(events)
        (target / 'summary.json').write_text(json.dumps(summary))
        try:
            parsed = module.load_capture(target)
        except ValueError:
            assert mode != 'valid'
        else:
            assert mode == 'valid'
            assert parsed[2][-2]['referenced_sha256'] == [parsed[2][1]['sha256']]
print('PASS: exact snapshot references and missing/partial/wrong-resource rejection')
