# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sums up one console run's 300-frame perf windows (gpu_profile=1) from a fetched log folder.

    python tools/xbox/perf-summary.py <log folder> [--skip SECONDS] [--csv windows.csv]

One row per window: log time, FPS and frame-time percentiles, what the window was bound by, the
GPU thread's idle and pipeline-stall time, GPU busy time, draws, guest waits on the GPU, PSO
creates, JIT cache clears and app memory. The footer gives the gameplay verdict over the windows
after --skip (default: the first window with draws over 100 per frame): mean and worst FPS,
windows under 28 FPS, and the share of frames over 50 ms.
"""
import argparse
import os
import re
import sys

WINDOW = re.compile(r"^\[ *([0-9.]+)\].*ReportPerfWindow: D3D12 perf over (\d+) frames \((\d+) ms\), ([^|]*)\|"
                    r" GPU thread: idle ([0-9.]+) ms.*?pipeline stalls (\d+) \(([0-9.]+) ms\).*?"
                    r"new resources (\d+) \(([0-9.]+) ms\).*?GPU busy ([0-9.]+) ms, (\d+) submits \| (\d+) draws, (\d+) dispatches"
                    r" \| uploads (\d+) \(([0-9.]+) MiB.*?downloads (\d+) \(([0-9.]+) MiB")
FRAMES = re.compile(r"D3D12 frame times: ([0-9.]+) FPS, p50 ([0-9.]+) ms, p95 ([0-9.]+) ms, p99 ([0-9.]+) ms")
GUEST_WAIT = re.compile(r"guest waited for the GPU (\d+) times \(([0-9.]+) ms")
PSO = re.compile(r"driver creates (\d+) / ([0-9.]+) ms")
JIT_CLEARS = re.compile(r"guest JIT: (\d+) full code cache clears")
MEMORY = re.compile(r"D3D12 memory: GPU (\d+) MiB.*app (\d+) of (\d+) MiB")
CORES = re.compile(r"emulated cores idle ([0-9.]+) ms")
MACROS = re.compile(r"macros (\d+) / ([0-9.]+) ms")


def parse(path):
    windows, current = [], None
    with open(path, encoding="utf-8", errors="replace") as log:
        for line in log:
            if "d3d12_perf_report" not in line:
                continue
            m = FRAMES.search(line)
            if m:
                current = {"fps": float(m[1]), "p50": float(m[2]), "p95": float(m[3]), "p99": float(m[4])}
                continue
            m = WINDOW.search(line)
            if m:
                w = current or {}
                current = None
                frames, ms = int(m[2]), int(m[3])
                w.update(t=float(m[1]), frames=frames, ms=ms, bound=m[4].strip().replace("mostly ", ""),
                         idle=float(m[5]) / frames, stalls=int(m[6]), stall_ms=float(m[7]) / frames,
                         new_res=int(m[8]), gpu=float(m[10]) / frames, draws=int(m[12]) // frames,
                         up_mib=float(m[15]), down=int(m[16]), down_mib=float(m[17]))
                w.setdefault("fps", frames * 1000.0 / ms)
                windows.append(w)
                continue
            if not windows:
                continue
            w = windows[-1]
            for regex, keys in ((GUEST_WAIT, ("gwait", "gwait_ms")), (PSO, ("pso", "pso_ms")),
                                (JIT_CLEARS, ("jit_clears",)), (CORES, ("cores_idle",)),
                                (MACROS, ("macros", "macro_ms"))):
                m = regex.search(line)
                if m and keys[0] not in w:
                    for k, v in zip(keys, m.groups()):
                        w[k] = float(v)
            m = MEMORY.search(line)
            if m:
                w["gpu_mib"], w["app_mib"] = int(m[1]), int(m[2])
    return windows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--skip", type=float, default=None)
    ap.add_argument("--csv")
    args = ap.parse_args()
    path = os.path.join(args.folder, "jadyx_log.txt")
    windows = parse(path)
    if not windows:
        print("no perf windows (gpu_profile=1 missing, or the game never drew)")
        return 1
    print(f"{'t':>7} {'fps':>5} {'p50':>6} {'p95':>6} {'p99':>7} {'idle':>6} {'stall':>6} {'gpu':>5} "
          f"{'draws':>5} {'pso':>4} {'gwait':>6} {'jitclr':>6} {'app':>5}  bound")
    for w in windows:
        print(f"{w['t']:7.0f} {w['fps']:5.1f} {w.get('p50', 0):6.1f} {w.get('p95', 0):6.1f} {w.get('p99', 0):7.1f} "
              f"{w['idle']:6.1f} {w['stall_ms']:6.1f} {w['gpu']:5.1f} {w['draws']:5d} {int(w.get('pso', 0)):4d} "
              f"{w.get('gwait_ms', 0) / w['frames']:6.1f} {int(w.get('jit_clears', 0)):6d} {w.get('app_mib', 0):5d}  {w['bound']}")
    skip = args.skip
    if skip is None:
        skip = next((w["t"] - 1 for w in windows if w["draws"] > 100), windows[0]["t"] - 1)
    play = [w for w in windows if w["t"] > skip]
    if play:
        frames = sum(w["frames"] for w in play)
        ms = sum(w["ms"] for w in play)
        low = [w for w in play if w["fps"] < 28.0]
        print(f"\ngameplay ({len(play)} windows after {skip:.0f} s): mean {frames * 1000.0 / ms:.1f} FPS, "
              f"worst window {min(w['fps'] for w in play):.1f}, under 28 FPS: {len(low)}, "
              f"worst p99 {max(w.get('p99', 0) for w in play):.0f} ms, "
              f"stall {sum(w['stall_ms'] * w['frames'] for w in play) / 1000:.1f} s, "
              f"peak app {max(w.get('app_mib', 0) for w in play)} MiB")
    if args.csv:
        keys = sorted({k for w in windows for k in w})
        with open(args.csv, "w", encoding="utf-8") as out:
            out.write(",".join(keys) + "\n")
            for w in windows:
                out.write(",".join(str(w.get(k, "")) for k in keys) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
