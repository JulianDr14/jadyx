#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate full GPU archives and compare candidates; live captures are not exact replays."""
import argparse
from collections import Counter, defaultdict
import hashlib
from functools import lru_cache
import json
from pathlib import Path
import struct


def load_capture(root):
    root = Path(root)
    summary = json.loads((root / 'summary.json').read_text(encoding='utf-8'))
    if summary.get('schema') != 1:
        raise ValueError('unsupported capture schema')
    records, digests, counts = [], {}, Counter()
    blob_size = (root / 'blobs.bin').stat().st_size
    last_us = -1
    view_data, views = defaultdict(list), {}
    with (root / 'blobs.bin').open('rb') as blobs:
        @lru_cache(maxsize=256)
        def read_chunk(offset, size):
            blobs.seek(offset)
            data = blobs.read(size)
            if len(data) != size:
                raise ValueError('short blob')
            return data
        with (root / 'events.jsonl').open(encoding='utf-8') as index:
            for line in index:
                event = json.loads(line)
                if event['seq'] != len(records) or event['us'] < last_us:
                    raise ValueError('event gap or non-monotonic time')
                last_us = event['us']
                counts[event['kind']] += 1
                detail = event['detail']
                if 'blob_size' in detail:
                    segments = detail.get('segments') or [dict(offset=detail['blob_offset'],
                                                               size=detail['blob_size'], fnv64=detail['fnv64'])]
                    digest, logical_size = hashlib.sha256(), 0
                    for segment in segments:
                        offset, size = segment['offset'], segment['size']
                        if not isinstance(offset, int) or not isinstance(size, int) or offset < 0 or size <= 0 or offset + size > blob_size:
                            raise ValueError('missing or truncated blob')
                        key = offset, size
                        chunk = read_chunk(offset, size)
                        # fnv64w hashes 64-bit little-endian words (current writer); fnv64 hashes bytes.
                        words = 'fnv64w' in segment
                        if (key, words) not in digests:
                            fnv = 0xcbf29ce484222325
                            whole = len(chunk) // 8 * 8 if words else 0
                            for (word,) in struct.iter_unpack('<Q', chunk[:whole]):
                                fnv = ((fnv ^ word) * 0x100000001b3) & 0xffffffffffffffff
                            for byte in chunk[whole:]:
                                fnv = ((fnv ^ byte) * 0x100000001b3) & 0xffffffffffffffff
                            digests[key, words] = str(fnv)
                        if segment['fnv64w' if words else 'fnv64'] != digests[key, words]:
                            raise ValueError('blob checksum mismatch')
                        digest.update(chunk)
                        logical_size += size
                    if logical_size != detail['blob_size']:
                        raise ValueError('missing chunks in logical resource')
                    event['sha256'] = digest.hexdigest()
                if event['kind'] == 'image_view' and not detail['after']:
                    views[detail['op'], detail['binding']] = detail
                if event['kind'] == 'image_data':
                    image = detail['context']['image']
                    if not image['after'] and image['role'].startswith('view_'):
                        view_data[image['op'], int(image['role'][5:])].append(event)
                if event['kind'] == 'image_view_reference':
                    view = detail['view']
                    source_key = detail['source_op'], detail['source_binding']
                    source, source_view = view_data.get(source_key, []), views.get(source_key)
                    if (view['after'] or view['storage'] or not source_view or
                            view['address'] != source_view['address'] or
                            not source or len(source) != detail['subresources'] or
                            len({e['detail']['context']['image']['subresource'] for e in source}) != len(source)):
                        raise ValueError('missing or invalid image snapshot reference')
                    event['referenced_sha256'] = [e['sha256'] for e in source]
                records.append(event)
    if not records or records[0]['kind'] != 'begin' or records[-1]['kind'] != 'end':
        raise ValueError('capture lacks begin/end')
    if summary['events'] != len(records) or summary['unique_bytes'] != blob_size:
        raise ValueError('summary/index/blob size mismatch')
    if records[-1]['detail']['intervals_complete'] != summary['intervals_complete']:
        raise ValueError('interval completeness mismatch')
    expected_complete = bool(summary['intervals_complete'] and not summary['omissions'] and not summary['errors'])
    if summary['data_complete'] != expected_complete or counts['omitted'] > summary['omissions']:
        raise ValueError('invalid completeness declaration')
    return root, summary, records, counts


def report(capture):
    root, summary, records, counts = capture
    omissions = Counter(e['detail']['reason'] for e in records if e['kind'] == 'omitted')
    producers = counts['draw_begin'] + counts['draw_indirect_begin'] + counts['dispatch_begin']
    unclosed = []
    for begin, end in [('draw_begin', 'draw_call_end'), ('dispatch_begin', 'dispatch_call_end'),
                       ('draw_indirect_begin', 'draw_indirect_call_end')]:
        ended = {e['detail']['op'] for e in records if e['kind'] == end}
        unclosed += [e['detail']['op'] for e in records
                     if e['kind'] == begin and e['detail']['op'] not in ended]
    present_coverage = True
    for present in (e['detail'] for e in records if e['kind'] == 'present'):
        tiles = sorted((e for e in records if e['kind'] == 'present_data' and
                        e['detail']['context']['frame'] == present['frame']),
                       key=lambda e: e['detail']['context']['origin_y'])
        row = 0
        for tile in tiles:
            c = tile['detail']['context']
            present_coverage &= (c['origin_y'] == row and c['height'] > 0 and
                                 c['width'] == present['width'] and c['format'] == present['format'] and
                                 tile['detail']['blob_size'] == c['height'] * c['row_bytes'])
            row += c['height']
        present_coverage &= row == present['height']
    return dict(path=str(root), summary=summary, events_by_kind=dict(counts),
                omission_reasons=dict(omissions), unclosed_producers=unclosed,
                present_coverage_complete=present_coverage,
                integrity_verified=True,
                ready_for_gpu_comparison=bool(summary['data_complete'] and producers and
                                              counts['present'] == 2 and counts['session'] == 1 and
                                              counts['adapter'] == 1 and not unclosed and present_coverage),
                session=[e['detail'] for e in records if e['kind'] == 'session'],
                note='Synchronous capture changes scheduling; not an FPS or race-freedom test.')


def shader_map(capture):
    by_op = defaultdict(list)
    for e in capture[2]:
        if e['kind'] in ('dxil', 'compute_dxil'):
            c = e['detail']['context']
            by_op[c['op']].append((c.get('stage', 'compute'), e['sha256']))
    return by_op


def compare(left, right):
    # Comparing shader bytes is safe across live runs. Resources/addresses/animation state
    # are not interchangeable inputs, even when the user aims at the same object.
    left_shaders = {item for shaders in shader_map(left).values() for item in shaders}
    right_shaders = {item for shaders in shader_map(right).values() for item in shaders}
    kind_hashes = []
    for capture in (left, right):
        by_kind = defaultdict(Counter)
        for e in capture[2]:
            if 'sha256' in e:
                by_kind[e['kind']][e['sha256']] += 1
            for digest in e.get('referenced_sha256', []):
                by_kind['image_data'][digest] += 1
        kind_hashes.append(by_kind)
    candidates = {}
    for kind in sorted(set(kind_hashes[0]) | set(kind_hashes[1])):
        a, b = kind_hashes[0][kind], kind_hashes[1][kind]
        candidates[kind] = dict(unique_left=len(a), unique_right=len(b),
                                shared=len(a.keys() & b.keys()),
                                only_left=len(a.keys() - b.keys()),
                                only_right=len(b.keys() - a.keys()))
    return dict(left=report(left), right=report(right),
                shaders_only_left=sorted(left_shaders - right_shaders, key=str),
                shaders_only_right=sorted(right_shaders - left_shaders, key=str),
                blob_candidates_by_kind=candidates,
                causal_conclusion=False,
                note='Live runs can differ normally. Replay a Series producer on PC with its exact inputs before attributing an output difference to the backend/driver.')


def check_self_test(capture):
    if not capture[1]['data_complete']:
        raise ValueError('GPU self-test capture is incomplete')
    records = capture[2]
    images = {e['detail']['context']['op']: e['sha256'] for e in records
              if e['kind'] == 'selftest_expected_image'}
    slices = {(e['detail']['context']['op'], e['detail']['context']['layer']): e['sha256']
              for e in records if e['kind'] == 'selftest_expected_slice'}
    buffer = [e['sha256'] for e in records if e['kind'] == 'selftest_expected_buffer']
    batches = {e['detail']['context']['subresource']: e['sha256'] for e in records
               if e['kind'] == 'selftest_expected_batch'}
    image_count = buffer_count = batch_count = 0
    for event in records:
        if event['kind'] == 'image_data':
            context = event['detail']['context']['image']
            if context['role'] == 'selftest_batch':
                if event['sha256'] != batches[context['subresource']]:
                    raise ValueError('batched array bytes differ from upload oracle')
                batch_count += 1
                continue
            expected = slices[context['op'], context['subresource']] if context['role'] == 'selftest_slice' else images[context['op']]
            if event['sha256'] != expected:
                raise ValueError('GPU image bytes differ from upload oracle')
            image_count += 1
        elif event['kind'] == 'buffer_data':
            if event['sha256'] != buffer[0]:
                raise ValueError('GPU buffer bytes differ from upload oracle')
            buffer_count += 1
    if len(images) != 4 or len(slices) != 3 or image_count != 13 or buffer_count != 2 or len(buffer) != 1:
        raise ValueError('GPU self-test coverage is missing')
    if batches and (len(batches) != 581 or batch_count != 581):
        raise ValueError('83-layer batched array coverage is missing')
    return dict(passed=True, image_checks=image_count, buffer_checks=buffer_count,
                batched_subresources=batch_count,
                paths=['RGBA8', 'UINT32', 'depth', 'volume', 'reinterpret', 'depth_feedback', 'slice_array'],
                integrity_verified=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    try:
        left = load_capture(args.capture)
        result = check_self_test(left) if args.self_test else compare(left, load_capture(args.compare)) if args.compare else report(left)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(2, f'Capture rejected: {exc}\n')
    text = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
    ready = result['passed'] if args.self_test else result['left']['ready_for_gpu_comparison'] and result['right']['ready_for_gpu_comparison'] if args.compare else result['ready_for_gpu_comparison']
    return 0 if ready else 1


if __name__ == '__main__':
    raise SystemExit(main())
