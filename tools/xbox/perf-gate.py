# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Performance and stability gate for one console run, against a per-title baseline.

    python tools/xbox/perf-gate.py <log folder or jadyx_log.txt> [--title ID] [--min-minutes 10]
    python tools/xbox/perf-gate.py <log> --record          # store this run as the title's baseline

Fetch the log first (fetch-logs.ps1). The gate reads what every build logs, with no boot.cfg keys:
the 300-frame "D3D12 frame times" windows and the "D3D12 corrections" counters after each of them.

It fails when:
  - the median p50 frame time rises over --p50-slack (5 %) or the median p95 over --p95-slack (10 %);
  - any correction counter costs more per frame than the baseline allows (x1.25 plus 1 per frame),
    or a counter the baseline has never seen does nonzero work: new per-draw work needs a new baseline,
    recorded on purpose after an A/B on the console;
  - the log has a Critical line (device removed, presentation disabled, failed frame tick);
  - the session is shorter than --min-minutes of gameplay (a soak, not a boot check).

Baselines live in tests/perf-baselines.json, keyed by title ID only.
"""
import argparse
import datetime
import json
import os
import re
import statistics
import sys

BASELINES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "perf-baselines.json")
STAMP = re.compile(r"^\[ *([0-9.]+)\]")
FRAMES = re.compile(r"D3D12 frame times: ([0-9.]+) FPS, p50 ([0-9.]+) ms, p95 ([0-9.]+) ms, p99 ([0-9.]+) ms "
                    r"\((\d+) presented")
CORRECTIONS = re.compile(r"D3D12 corrections: (.*)$")
COUNTER = re.compile(r"([0-9]+(?:\.[0-9]+)?) ([a-z][a-z -]*[a-z])")
WAIT_MS = re.compile(r"/ ([0-9.]+) ms")
TITLE = re.compile(r"title_id=([0-9A-F]{16})")
HITCH = re.compile(r"Hitch: (\d+) ms frame, normally (\d+) ms; likely ([^|]*)")
CRITICAL = "<Critical>"
# Counters that describe the scene, not correction work: never judged per frame.
NOT_WORK = {"draws"}


def parse_corrections(text):
    """'1201860 draws, 0 tessellated, ...; copies: 0 logic op (0.00 MiB whole), ...' -> {label: value}."""
    counters = {}
    for piece in re.split(r"[,;]", text):
        piece = piece.split(":", 1)[-1]
        m = COUNTER.search(piece)
        if not m:
            continue
        label = m[2].strip()
        counters[label] = float(m[1])
        m = WAIT_MS.search(piece)
        if m:
            counters[label + " ms"] = float(m[1])
    return counters


def parse(path):
    if os.path.isdir(path):
        path = os.path.join(path, "jadyx_log.txt")
    run = {"windows": [], "critical": [], "hitches": [], "title": None, "first": None, "last": 0.0}
    window = None
    with open(path, encoding="utf-8", errors="replace") as log:
        for line in log:
            m = STAMP.match(line)
            if m:
                run["last"] = float(m[1])
            if run["title"] is None:
                m = TITLE.search(line)
                if m:
                    run["title"] = m[1]
            if CRITICAL in line:
                run["critical"].append(line.strip())
            m = HITCH.search(line)
            if m:
                run["hitches"].append((int(m[1]), int(m[2]), m[3].strip()))
            m = FRAMES.search(line)
            if m:
                window = {"t": run["last"], "fps": float(m[1]), "p50": float(m[2]), "p95": float(m[3]),
                          "p99": float(m[4]), "frames": int(m[5])}
                run["windows"].append(window)
                if run["first"] is None:
                    run["first"] = run["last"]
                continue
            m = CORRECTIONS.search(line)
            if m and window is not None and "work" not in window:
                window["work"] = parse_corrections(m[1])
    return run


def summarize(run):
    # The first window holds boot, JIT warm-up and the first shader builds: it is not gameplay.
    windows = run["windows"][1:] or run["windows"]
    if not windows:
        return None
    per_frame = {}
    for w in windows:
        for label, value in w.get("work", {}).items():
            per_frame.setdefault(label, []).append(value / w["frames"])
    return {
        "windows": len(windows),
        "p50": statistics.median(w["p50"] for w in windows),
        "p95": statistics.median(w["p95"] for w in windows),
        "fps": statistics.median(w["fps"] for w in windows),
        "per_frame": {k: round(statistics.median(v), 4) for k, v in per_frame.items() if k not in NOT_WORK},
        "draws_per_frame": round(statistics.median(per_frame.get("draws", [0])), 1),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--title", help="title ID; read from the log when omitted")
    ap.add_argument("--baselines", default=BASELINES)
    ap.add_argument("--min-minutes", type=float, default=10.0)
    ap.add_argument("--p50-slack", type=float, default=0.05)
    ap.add_argument("--p95-slack", type=float, default=0.10)
    ap.add_argument("--work-factor", type=float, default=1.25)
    ap.add_argument("--work-slack", type=float, default=1.0, help="extra corrections per frame allowed")
    ap.add_argument("--record", action="store_true", help="store this run as the title's baseline")
    ap.add_argument("--build", default="", help="build version, stored with --record")
    args = ap.parse_args()

    run = parse(args.log)
    title = args.title or run["title"]
    now = summarize(run)
    if not title or now is None:
        print("FAIL: the log has no title ID or no frame-time windows (was a game played?)")
        return 1
    minutes = (run["last"] - (run["first"] or run["last"])) / 60.0
    print(f"title {title}: {now['windows']} gameplay windows, {minutes:.1f} min, {now['draws_per_frame']} draws/frame")
    print(f"  median p50 {now['p50']:.2f} ms, p95 {now['p95']:.2f} ms, {now['fps']:.2f} FPS")
    slow = [w for w in run["windows"][1:] if w["fps"] < 20.0]
    if slow:
        print(f"  {len(slow)} windows under 20 FPS (loads or stalls): " + ", ".join(f"{w['t']:.0f}s" for w in slow))
    for ms, normal, cause in sorted(run["hitches"], reverse=True)[:3]:
        print(f"  hitch {ms} ms (normally {normal} ms): {cause}")

    failures = []
    if run["critical"]:
        failures.append(f"{len(run['critical'])} Critical lines, first: {run['critical'][0][:200]}")
    if minutes < args.min_minutes:
        failures.append(f"session of {minutes:.1f} min, the soak needs {args.min_minutes:.0f}")

    baselines = {}
    if os.path.exists(args.baselines):
        with open(args.baselines, encoding="utf-8") as f:
            baselines = json.load(f)

    if args.record:
        if failures:
            print("not recording a baseline from a failing run:")
            for f in failures:
                print("  - " + f)
            return 1
        baselines[title] = {"build": args.build, "recorded": datetime.date.today().isoformat(), **now}
        with open(args.baselines, "w", encoding="utf-8", newline="\n") as f:
            json.dump(baselines, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"recorded baseline for {title} in {args.baselines}")
        return 0

    base = baselines.get(title)
    if base is None:
        failures.append(f"no baseline for {title}: run with --record after the build is accepted on the console")
    else:
        print(f"  baseline {base.get('build') or '?'}: p50 {base['p50']:.2f} ms, p95 {base['p95']:.2f} ms")
        if now["p50"] > base["p50"] * (1 + args.p50_slack):
            failures.append(f"p50 {now['p50']:.2f} ms vs baseline {base['p50']:.2f} ms "
                            f"(+{100 * (now['p50'] / base['p50'] - 1):.1f} %)")
        if now["p95"] > base["p95"] * (1 + args.p95_slack):
            failures.append(f"p95 {now['p95']:.2f} ms vs baseline {base['p95']:.2f} ms "
                            f"(+{100 * (now['p95'] / base['p95'] - 1):.1f} %)")
        for label, value in sorted(now["per_frame"].items()):
            known = base["per_frame"].get(label)
            if known is None:
                if value > 0:
                    failures.append(f"new correction work '{label}': {value:.2f} per frame, not in the baseline")
                continue
            limit = known * args.work_factor + args.work_slack
            if value > limit:
                failures.append(f"'{label}' {value:.2f} per frame vs baseline {known:.2f} (limit {limit:.2f})")

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
