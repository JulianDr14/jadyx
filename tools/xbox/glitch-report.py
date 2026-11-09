# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads a geometry glitch capture (LocalState/jadyx/log/glitch/glitch_*.txt) and tells which
hypothesis it supports: why geometry deforms and why objects flicker.

    python tools/xbox/glitch-report.py glitch_20261108_120000.txt [--json out.json]

Line kinds written by the renderer (d3d12_glitch_hunt.cpp, d3d12_rasterizer_glitch.cpp):
    D  a draw: outcome, shaders, geometry, render condition, vertex slots lost to a list restart
    I  index range of an indexed draw
    B  one binding the draw read: guest hash, cache state (gm GPU-modified, cm CPU-modified),
       vertex view checks, float stats
    M  host copy of a clean binding differs from guest memory (the strongest evidence)
    H  (v2) what the GPU left in a GPU-written vertex stream or transform feedback buffer:
       zero words, NaN, huge values, xyz bounding box, hash
    X  (v2) transform feedback buffer registers and host binding of a draw that captures it
    T  (v2) D3D12 stream output filled sizes after the pass (bytes past each binding)
    P  (v3) what a recorded draw rasterized on the host: occlusion samples (occ) and pipeline
       statistics (ia primitives in, vsi vertex shader runs, ci/cp primitives into/out of
       clipping, psi pixel shader runs); "-" when the guest's own counter held that query type
    C  a compute dispatch
    F  frame end, R totals
"""

import argparse
import collections
import json
import re
import sys

TOKEN = re.compile(r'(\w[\w.]*)=(\S+)')


def parse(path):
    draws, bindings, mismatches, frames, indices, dispatches = [], [], [], [], [], []
    totals = {}
    inspects, xfb, counters, probes, losses = [], [], [], [], []
    with open(path, encoding='utf-8', errors='replace') as file:
        for raw in file:
            line = raw.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            kind, _, rest = line.partition(' ')
            fields = dict(TOKEN.findall(rest))
            fields['_raw'] = line
            if kind == 'D':
                draws.append(fields)
            elif kind == 'B':
                fields['_null'] = ' null ' in f' {rest} '
                fields['_stale'] = 'stale(' in rest
                fields['_uav'] = ':uav' in rest
                bindings.append(fields)
            elif kind == 'M':
                mismatches.append(fields)
            elif kind == 'F':
                frames.append(fields)
            elif kind == 'I':
                indices.append(fields)
            elif kind == 'C':
                dispatches.append(fields)
            elif kind == 'R':
                totals = fields
            elif kind == 'H':
                inspects.append(fields)
            elif kind == 'X':
                xfb.append(fields)
            elif kind == 'T':
                counters.append(fields)
            elif kind == 'P':
                probes.append(fields)
            elif kind == 'W':
                losses.append(fields)
    parse.v2 = (inspects, xfb, counters)
    parse.v3 = probes
    parse.v10 = losses
    return draws, bindings, mismatches, frames, indices, dispatches, totals


def key(draw):
    fb = draw.get('fb', 'none').split(':')
    size = fb[1] if len(fb) > 1 else 'none'
    return (draw.get('vs'), draw.get('ps'), draw.get('topo'), draw.get('cnt'), size)


def short(identity):
    vs, ps, topo, cnt, size = identity
    return f'vs={vs} ps={ps} topo={topo} cnt={cnt} fb={size}'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('capture')
    parser.add_argument('--json', help='also write the findings as JSON')
    parser.add_argument('--top', type=int, default=25)
    args = parser.parse_args()

    draws, bindings, mismatches, frames, indices, dispatches, totals = parse(args.capture)
    if not draws:
        print('no draws in the capture: was the game running when it was taken?')
        return 1
    frame_ids = sorted({int(d['f']) for d in draws})
    nframes = len(frame_ids)
    by_call = {(d['f'], d['n']): d for d in draws}
    bind_by_call = collections.defaultdict(list)
    for b in bindings:
        bind_by_call[(b['f'], b['n'])].append(b)
    findings = {}
    out = []

    def section(title):
        out.append('')
        out.append(f'== {title}')

    out.append(f'capture: {args.capture}')
    out.append(f'frames {nframes}, draws {len(draws)}, dispatches {len(dispatches)}, '
               f'bindings {len(bindings)}, host/guest compares {totals.get("compares", "?")}, '
               f'mismatches {len(mismatches)}, over budget {totals.get("over_budget", "?")}')
    if frames:
        ms = [float(f.get('ms', 0)) for f in frames]
        out.append(f'frame ms: min {min(ms):.1f} avg {sum(ms) / len(ms):.1f} max {max(ms):.1f}')
        toggles = {k: frames[-1].get(k) for k in ('force_visible',)}
        out.append(f'toggles at the end: {toggles}')

    # H7: vertex slots dropped by a mid-draw command list restart.
    section('H7 lost vertex slots (mid-draw command list restart)')
    lost = [d for d in draws if d.get('lost', '0') not in ('0', '')]
    lost_frames = sum(int(f.get('lost_vb_draws', 0)) for f in frames)
    lost_total = frames[-1].get('lost_total', '?') if frames else '?'
    out.append(f'draws in the capture with lost slots: {len(lost)}; lost-slot draws over the '
               f'captured frames: {lost_frames}; since boot: {lost_total}')
    for d in lost[:args.top]:
        out.append(f'  f={d["f"]} n={d["n"]} lost={d["lost"]} {short(key(d))}')
    findings['h7_lost_draws'] = len(lost)
    findings['h7_lost_since_boot'] = lost_total

    # Host/guest mismatches on bindings the cache considered clean.
    section('Host copy differs from guest memory (clean bindings)')
    roles = collections.Counter()
    for m in mismatches:
        role = re.sub(r'\d+(\.\d+)?$', '', m.get('r', '?'))
        roles[role] += 1
    out.append(f'mismatches by binding kind: {dict(roles) or "none"}')
    for m in mismatches[:args.top]:
        d = by_call.get((m['f'], m['n']))
        b = next((b for b in bind_by_call.get((m['f'], m['n']), []) if b.get('r') == m.get('r')),
                 None)
        state = f'gm={b.get("gm")} cm={b.get("cm")}' if b else ''
        out.append(f'  f={m["f"]} n={m["n"]} {m.get("r")} words={m.get("words")} '
                   f'first={m.get("first")} host={m.get("host")} guest={m.get("guest")} {state} '
                   f'{short(key(d)) if d else ""}')
    findings['mismatches'] = dict(roles)

    # Vertex views that do not point where the binding is.
    section('Stale vertex views / unset slots on recorded draws')
    stale = [b for b in bindings if b['_stale']]
    unset = [b for b in bindings if b.get('r', '').startswith('vb') and b.get('set') == '0'
             and by_call.get((b['f'], b['n']), {}).get('o') == 'recorded']
    out.append(f'stale vertex views: {len(stale)}; enabled vertex slots not set on the list: '
               f'{len(unset)}')
    for b in (stale + unset)[:args.top]:
        out.append(f'  f={b["f"]} n={b["n"]} {b.get("r")} {b["_raw"].split(" view=")[-1]}')
    findings['stale_views'] = len(stale)
    findings['unset_slots'] = len(unset)

    # Uniforms: null, short against the shader, non-finite values.
    section('Uniform buffers')
    null_cb = [b for b in bindings if b.get('r', '').startswith('cb') and b['_null']]
    short_cb = [b for b in bindings if b.get('short') == '1']
    nan_cb = [b for b in bindings if b.get('r', '').startswith('cb') and b.get('nan', '0') != '0']
    paths = collections.Counter(b.get('path') for b in bindings if b.get('r', '').startswith('cb'))
    out.append(f'null {len(null_cb)}, shorter than the shader reads {len(short_cb)}, with NaN/Inf '
               f'{len(nan_cb)}; paths {dict(paths)} (s streamed, c cached)')
    for b in (short_cb + nan_cb)[:args.top]:
        out.append(f'  f={b["f"]} n={b["n"]} {b.get("r")} size={b.get("size")} '
                   f'ssize={b.get("ssize")} nan={b.get("nan")} max={b.get("max")}')
    findings['cb_null'] = len(null_cb)
    findings['cb_short'] = len(short_cb)
    findings['cb_nan'] = len(nan_cb)

    # Vertex data with non-finite or huge values (exploded vertices).
    section('Vertex data values')
    vb_nan = [b for b in bindings if b.get('r', '').startswith('vb') and b.get('nan', '0') != '0']
    vb_huge = [b for b in bindings if b.get('r', '').startswith('vb')
               and float(b.get('max', '0') or 0) > 1e6]
    out.append(f'vertex ranges with NaN/Inf words: {len(vb_nan)}; with |value| > 1e6: '
               f'{len(vb_huge)} (packed integer attributes can look huge; compare across frames)')
    for b in (vb_nan + vb_huge)[:args.top]:
        d = by_call.get((b['f'], b['n']))
        out.append(f'  f={b["f"]} n={b["n"]} {b.get("r")} nan={b.get("nan")} max={b.get("max")} '
                   f'gm={b.get("gm")} {short(key(d)) if d else ""}')
    findings['vb_nan'] = len(vb_nan)

    # GPU-written vertex/index data (compute skinning, stream out): read-after-write hazards.
    section('GPU-written geometry inputs')
    gpu_geo = [b for b in bindings if (b.get('r', '').startswith('vb') or b.get('r') == 'ib')
               and b.get('gm') == '1']
    out.append(f'vertex/index bindings over GPU-modified memory: {len(gpu_geo)} '
               '(not compared: their truth is on the GPU). Deformation only on these draws points '
               'at a missing barrier: try uav_sync=on.')
    shaders = collections.Counter()
    for b in gpu_geo:
        d = by_call.get((b['f'], b['n']))
        if d:
            shaders[(d.get('vs'), b.get('r'))] += 1
    for (vs, role), count in shaders.most_common(10):
        out.append(f'  vs={vs} {role}: {count}')
    findings['gpu_written_geometry'] = len(gpu_geo)

    # v2: transform feedback passes and what they left in their buffers.
    inspects, xfb, counters = parse.v2
    section('Transform feedback (stream output) passes')
    if not xfb:
        out.append('no draw captured transform feedback (or a v1 capture)')
    modes = collections.Counter(x.get('mode') for x in xfb if x.get('en') == '1')
    out.append(f'enabled buffer bindings by mode: {dict(modes) or "none"}')
    overflow, empty_pass = [], []
    for t in counters:
        words = [int(w) for w in t.get('words', '').split(',') if w]
        sizes = [int(w) for w in t.get('sizes', '').split(',') if w]
        for index, (filled, size) in enumerate(zip(words, sizes)):
            if size == 0:
                continue
            if filled > size:
                overflow.append((t, index, filled, size))
        if sizes and all(w == 0 for w, sz in zip(words, sizes) if sz):
            empty_pass.append(t)
    exact = sum(1 for t in counters
                for w, sz in zip(t.get('words', '').split(','), t.get('sizes', '').split(','))
                if w and sz and sz != '0' and w == sz)
    out.append(f'stream output counter readbacks: {len(counters)}; buffers written past their '
               f'binding (output dropped): {len(overflow)}; filled exactly to their size '
               f'(normal): {exact}; passes with every counter at 0: {len(empty_pass)}')
    for t, index, filled, size in overflow[:args.top]:
        d = by_call.get((t['f'], t['n']))
        out.append(f'  OVER f={t["f"]} n={t["n"]} buffer {index}: filled {filled} > size {size} '
                   f'{short(key(d)) if d else ""}')
    for t in empty_pass[:5]:
        out.append(f'  EMPTY f={t["f"]} n={t["n"]} words={t.get("words")} sizes={t.get("sizes")}')
    xfb_h = [h for h in inspects if h.get('r', '').startswith('xfb')]
    zero_xfb = [h for h in xfb_h if h.get('words') not in (None, '0') and h.get('zero') == h.get('words')]
    out.append(f'transform feedback buffers read back: {len(xfb_h)}; all zeros: {len(zero_xfb)}')
    for h in zero_xfb[:args.top]:
        d = by_call.get((h['f'], h['n']))
        out.append(f'  ZEROS f={h["f"]} n={h["n"]} {h["r"]} {h.get("gva")} mode={h.get("mode")} '
                   f'len={h.get("len")} {short(key(d)) if d else ""}')
    findings['xfb_modes'] = dict(modes)
    findings['xfb_overflow'] = len(overflow)
    findings['xfb_all_zero'] = len(zero_xfb)

    # v2: what the GPU-written vertex streams actually held when consumers drew from them.
    section('GPU-written vertex streams as the draw saw them (host readback)')
    host_ranges = []
    for x in xfb:
        host = x.get('host', '0+0:0')
        if x.get('en') == '1' and not host.startswith('0+'):
            ident, _, rest = host.partition('+')
            off, _, size = rest.partition(':')
            host_ranges.append((ident, int(off, 16), int(off, 16) + int(size), x))
    dispatch_of = {(c['f'], c['n']): c for c in dispatches}
    compute_ranges = []
    for b in bindings:
        if b.get('r', '').startswith('csb') and b.get('w') == '1' and b.get('a'):
            begin = int(b['a'], 16)
            c = dispatch_of.get((b['f'], b['n']), {})
            compute_ranges.append((begin, begin + int(b.get('len', '0')), c.get('cs', '?')))
    buf_of = {}
    for b in bindings:
        if b.get('r', '').startswith('vb') and ':ins' in b.get('_raw', ''):
            raw = b['_raw'].split(' buf=', 1)
            if len(raw) == 2:
                ident, _, off = raw[1].split(':', 1)[0].partition('+')
                buf_of[(b['f'], b['n'], b['r'])] = (ident, int(off, 16))
    vb_h = [h for h in inspects if h.get('r', '').startswith('vb')]
    groups = collections.defaultdict(list)
    writer = {}
    for h in vb_h:
        d = by_call.get((h['f'], h['n']))
        group = (d.get('vs') if d else '?', h.get('r'), h.get('gva'))
        groups[group].append(h)
        if group in writer:
            continue
        host = buf_of.get((h['f'], h['n'], h.get('r')))
        if host:
            for ident, begin, end, x in host_ranges:
                if ident == host[0] and begin <= host[1] < end:
                    writer[group] = f'xfb{x.get("i")} {x.get("mode")}'
                    break
        if group not in writer and h.get('gva'):
            address = int(h['gva'], 16)
            for begin, end, cs in compute_ranges:
                if begin <= address < end:
                    writer[group] = f'compute cs={cs}'
                    break
    out.append(f'GPU-written vertex streams read back: {len(vb_h)} in {len(groups)} '
               '(vertex shader, slot, address) groups')
    bad = []
    for (vs, role, gva), items in groups.items():
        hashes = collections.Counter(h.get('h') for h in items)
        zeros = sum(1 for h in items if h.get('zero') == h.get('words'))
        # Packed attributes (stride under 12: normals, colours, bone indices) look like NaN or
        # huge floats; only float vectors are judged by value.
        floats = [h for h in items if int(h.get('stride', 0)) >= 12]
        nan = sum(int(h.get('nan', 0)) for h in floats)
        big = sum(int(h.get('big', 0)) for h in floats)
        zel = sum(int(h.get('zelems', 0)) for h in items)
        boxes = collections.Counter(h.get('box') for h in items)
        source = writer.get((vs, role, gva), 'unknown writer')
        flag = zeros or nan or big or zel
        if flag:
            bad.append((vs, role, gva))
        out.append(f'  {"!!" if flag else "  "} vs={vs} {role} gva={gva} from={source} reads={len(items)} '
                   f'hashes={len(hashes)} all-zero={zeros} zero-xyz-elems={zel} nan={nan} big={big} '
                   f'boxes={len(boxes)} e.g. {next(iter(boxes))}')
    findings['gpu_stream_groups'] = len(groups)
    findings['gpu_stream_bad'] = len(bad)

    # v3: draws the game issued every frame that rasterized nothing in some of them.
    probes = getattr(parse, 'v3', [])
    section('Flicker on the host: draws issued but drawing nothing in some frames')
    vanish = []
    if not probes:
        out.append('no draw probes (a v1/v2 capture)')
    else:
        probe_of = {(p['f'], p['n']): p for p in probes}
        kinds = collections.Counter(('occ' if p.get('occ') != '-' else '') +
                                    ('+stats' if p.get('psi') != '-' else '') for p in probes)
        out.append(f'draws probed: {len(probes)} of {len(draws)}; queries available: '
                   f'{dict(kinds)}')

        def pixels(p):
            for field in ('occ', 'psi'):
                if p.get(field, '-') != '-':
                    return int(p[field])
            return None

        # Mesh identity (shaders, target, index and vertex buffer addresses) + ordinal of that
        # mesh within its frame: the draw order shuffles between frames, the mesh does not.
        series = collections.defaultdict(dict)
        ordinal = collections.Counter()
        for d in draws:
            if d.get('o') != 'recorded':
                continue
            binds = {b['r']: b for b in bind_by_call.get((d['f'], d['n']), [])}
            identity = key(d) + (binds.get('ib', {}).get('a'), binds.get('vb0', {}).get('a'))
            index = ordinal[(d['f'], identity)]
            ordinal[(d['f'], identity)] += 1
            p = probe_of.get((d['f'], d['n']))
            if p is not None and pixels(p) is not None:
                series[(identity, index)][int(d['f'])] = (d, p)
        for (identity, index), by_frame in series.items():
            values = {f: pixels(p) for f, (d, p) in by_frame.items()}
            lit = [f for f, v in values.items() if v > 0]
            dark = [f for f, v in values.items() if v == 0]
            if lit and dark and len(by_frame) >= max(3, nframes // 2):
                flips = 0
                ordered = sorted(values)
                for a, b in zip(ordered, ordered[1:]):
                    flips += (values[a] > 0) != (values[b] > 0)
                vanish.append((flips, len(dark), identity, index, by_frame, values))
        vanish.sort(key=lambda v: (-v[0], -v[1]))
        # Partial flicker (alpha-cut foliage): same primitives in, surviving samples swing.
        swings = []
        for (identity, index), by_frame in series.items():
            occ = {f: int(p['occ']) for f, (d, p) in by_frame.items() if p.get('occ', '-') != '-'}
            ia = {p.get('ia') for d, p in by_frame.values()}
            ordered = sorted(occ)
            jumps = [(a, b) for a, b in zip(ordered, ordered[1:])
                     if max(occ[a], occ[b]) >= 64 and
                     abs(occ[a] - occ[b]) > 0.5 * max(occ[a], occ[b])]
            if jumps and len(ia) == 1:
                swings.append((len(jumps), max(occ.values()), identity, index, occ))
        swings.sort(key=lambda v: (-v[0], -v[1]))
        out.append(f'meshes with the same primitives whose surviving samples swing >50% between '
                   f'frames: {len(swings)}')
        for njumps, peak, identity, index, occ in swings[:args.top]:
            values = ' '.join(str(occ.get(f, '.')) for f in frame_ids)
            out.append(f'  jumps={njumps} {short(identity[:5])} ib={identity[5]} '
                       f'vb0={identity[6]} #{index}: {values}')
        out.append(f'objects drawn in most frames that rasterized nothing in some: {len(vanish)}')

        def bind_map(d):
            return {b['r']: b for b in bind_by_call.get((d['f'], d['n']), [])}

        IGNORE = {'f', 'n', '_raw'}
        for flips, ndark, identity, index, by_frame, values in vanish[:args.top]:
            line = ''.join('.' if f not in values else ('#' if values[f] > 0 else 'o')
                           for f in frame_ids)
            out.append(f'  {line} flips={flips} {short(identity[:5])} ib={identity[5]} '
                       f'vb0={identity[6]} #{index}')
            dark_f = next(f for f in sorted(values) if values[f] == 0)
            lit_f = min((f for f in values if values[f] > 0), key=lambda f: abs(f - dark_f))
            d_dark, p_dark = by_frame[dark_f]
            d_lit, p_lit = by_frame[lit_f]
            out.append(f'      lit f={lit_f} n={d_lit["n"]}: occ={p_lit.get("occ")} '
                       f'ia={p_lit.get("ia")} cp={p_lit.get("cp")} psi={p_lit.get("psi")}')
            out.append(f'      dark f={dark_f} n={d_dark["n"]}: occ={p_dark.get("occ")} '
                       f'ia={p_dark.get("ia")} cp={p_dark.get("cp")} psi={p_dark.get("psi")}')
            if p_dark.get('cp') not in (None, '-') and p_dark.get('cp') == '0' and \
                    p_dark.get('ia') not in ('0', '-'):
                out.append('      -> every primitive was clipped away: vertex positions '
                           '(deformed/zeroed geometry or a stale transform)')
            elif p_dark.get('occ') == '0' and p_dark.get('psi') not in (None, '-', '0'):
                out.append('      -> pixels were shaded but none survived: discard/alpha test '
                           'or the depth/stencil test')
            elif p_dark.get('cp') not in (None, '-', '0'):
                out.append('      -> primitives reached the rasterizer but no sample passed: '
                           'depth/stencil, scissor/viewport, or discard')
            changed = [f'{k}:{d_lit.get(k)}->{d_dark.get(k)}' for k in d_lit
                       if k not in IGNORE and not k.startswith('_') and
                       d_lit.get(k) != d_dark.get(k)]
            if changed:
                out.append(f'      draw state changed: {" ".join(changed)}')
            lit_b, dark_b = bind_map(d_lit), bind_map(d_dark)
            diffs = []
            for role in sorted(set(lit_b) | set(dark_b)):
                a, b = lit_b.get(role), dark_b.get(role)
                if not a or not b:
                    diffs.append(f'{role}:{"absent" if not a else "set"}->'
                                 f'{"absent" if not b else "set"}')
                    continue
                for field in ('h', 'size', 'len', 'gm', 'cm', 'nan', 'max', 'view'):
                    if a.get(field) != b.get(field):
                        diffs.append(f'{role}.{field}:{a.get(field)}->{b.get(field)}')
            if diffs:
                out.append(f'      bindings changed: {" ".join(diffs[:16])}'
                           f'{" ..." if len(diffs) > 16 else ""}')
    findings['vanishing_draws'] = len(vanish)

    # Flicker: draws present in some frames and not in others, and how they ended.
    section('Flicker: draws that come and go between frames')
    per_frame = collections.defaultdict(collections.Counter)
    outcomes = collections.defaultdict(collections.Counter)
    conds = collections.defaultdict(collections.Counter)
    for d in draws:
        identity = key(d)
        outcomes[identity][d.get('o')] += 1
        if d.get('o') == 'recorded':
            per_frame[identity][int(d['f'])] += 1
        cond = d.get('cond', 'none:1').split(':')
        if cond[0] not in ('none', 'fixed'):
            conds[identity][f'{cond[0]}:{"render" if cond[1] == "1" else "skip"}'] += 1
    unstable = []
    for identity, counts in per_frame.items():
        values = [counts.get(f, 0) for f in frame_ids]
        if min(values) != max(values):
            unstable.append((identity, values))
    for identity in outcomes:
        if identity not in per_frame:
            unstable.append((identity, [0] * nframes))
    unstable.sort(key=lambda item: -sum(1 for v in item[1] if v == 0))
    out.append(f'draw identities whose recorded count changes between frames: {len(unstable)}')
    for identity, values in unstable[:args.top]:
        pattern = ''.join('#' if v else '.' for v in values)
        out.append(f'  {pattern} {short(identity)} outcomes={dict(outcomes[identity])} '
                   f'cond={dict(conds[identity])}')
    path_totals = collections.Counter(d.get('cond', 'none').split(':')[0] for d in draws)
    skipped = collections.Counter(d.get('cond', 'none').split(':')[0] for d in draws
                                  if d.get('o') == 'conditional')
    out.append(f'condition paths over all draws: {dict(path_totals)}; skipped by condition: '
               f'{dict(skipped)}')
    findings['flicker_identities'] = len(unstable)
    findings['conditional_skips'] = dict(skipped)

    # Other outcomes that drop draws.
    section('Draws that did not reach the GPU')
    dropped = collections.Counter(d.get('o') for d in draws if d.get('o') != 'recorded')
    out.append(f'{dict(dropped) or "none"}')
    findings['dropped'] = dict(dropped)

    # Verdicts.
    section('CPU writes over GPU-written buffer bytes (W, v10)')
    losses = getattr(parse, 'v10', [])
    # GPU-written ranges the capture saw: compute outputs and storage the GPU filled.
    gpu_ranges = {}
    for b in bindings:
        if not b.get('a') or not b.get('size'):
            continue
        role = b.get('r', '')
        if (role.startswith('csb') and b.get('w') == '1') or (role.startswith('sb') and b.get('gm') == '1'):
            start = int(b['a'], 16)
            gpu_ranges[(start, int(b['size']))] = role
    hits = collections.Counter()
    for w in losses:
        a, n = int(w['a'], 16), int(w['len'])
        inside = [r for (start, size), r in gpu_ranges.items() if a < start + size and start < a + n]
        hits[(w['f'], ','.join(sorted(set(inside))) or 'other')] += 1
    if not losses:
        out.append('no W lines (v9 capture, or no gathered CPU write touched GPU bytes)')
    else:
        per_frame = collections.Counter(w['f'] for w in losses)
        out.append(f'{len(losses)} writes; per frame: '
                   + ' '.join(f'f{f}={c}' for f, c in sorted(per_frame.items(), key=lambda x: int(x[0]))))
        for (f, where), count in sorted(hits.items(), key=lambda x: (int(x[0][0]), x[0][1]))[:args.top]:
            out.append(f'  f={f} {count:4d} into {where}')
        sample = collections.Counter((w['a'], w['len'], w['gpu']) for w in losses)
        for (a, n, g), count in sample.most_common(10):
            out.append(f'  a={a} len={n} gpu={g} x{count}')
    findings['gpu_losses'] = len(losses)

    section('Verdict')
    verdicts = []
    if lost or (lost_total not in ('?', '0')):
        verdicts.append('H7 CONFIRMED: draws ran without vertex buffers after a command list '
                        'restart.')
    if roles:
        verdicts.append('Host buffers differ from guest memory on clean ranges: a cache sync bug '
                        f'({", ".join(roles)}). The M lines name the draws.')
    if stale:
        verdicts.append('Vertex views point away from the bound guest stream: stale binding.')
    if short_cb or null_cb:
        verdicts.append('Uniform buffers are shorter than the shader reads or null: vertices '
                        'read zeros (deformation if bone matrices live there).')
    if gpu_geo and not lost and not roles:
        verdicts.append('Geometry comes from GPU-written buffers: A/B uav_sync to test a barrier.')
    if skipped:
        verdicts.append('Draws were skipped by the render condition '
                        f'({dict(skipped)}): A/B occlusion_force_visible in boot.cfg.')
    if unstable and not skipped:
        verdicts.append('Draws come and go but none were skipped here: the game itself did not '
                        'issue them (CPU culling from wrong readbacks, or query results).')
    if overflow:
        verdicts.append('Stream output buffers filled up: later passes wrote nothing and the '
                        'consumers read old vertices (deformation). Counters append across draws.')
    if zero_xfb or any(b for b in bad):
        verdicts.append('GPU-written vertex data reads back as zeros/NaN/huge on the host: the '
                        'producing pass (see from=) did not write what the consumer reads.')
    if vanish:
        verdicts.append('The host drew nothing for objects the game drew every frame: see '
                        '"Flicker on the host" for what changed between their lit and dark frames.')
    if losses and any(where != 'other' for (_, where) in hits):
        verdicts.append('CPU writes land on buffers the GPU filled and upload guest bytes over '
                        'them (W lines): A/B precise_writes on the glitch page; if flicker stops, this is it.')
    if not verdicts:
        verdicts.append('Nothing stands out: capture again while the glitch is on screen, with '
                        'more frames (glitch_frames=60).')
    out.extend(f'- {v}' for v in verdicts)
    findings['verdicts'] = verdicts

    print('\n'.join(out))
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as file:
            json.dump(findings, file, indent=2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
