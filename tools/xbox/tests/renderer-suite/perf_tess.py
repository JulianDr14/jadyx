# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""What emulated tessellation costs on the console: the payload's tessellation loop (perf_tess.cpp) run
tessellated and flat, the same triangles either way, for each shape given.

    python tools/xbox/tests/renderer-suite/perf_tess.py --console 192.168.1.50 --version 0.3.9.N
        [--shape DRAWS,PATCHES,LEVEL ...] [--frames 1200] [--no-package]

Installs the payload once with gpu_profile=1 (above the installed version: never uninstalls), then per
run writes mode.txt, launches it and reads the emulator's perf windows from the log. The first window
(pipeline creation) is left out; the rest give medians per frame.
"""
import argparse
import importlib.util
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
NRO = os.path.join(REPO, "build-uwp", "payloads", "renderer-suite", "renderer_suite.nro")
sys.path.insert(0, HERE)
import console  # noqa: E402


def load_perf_parser():
    spec = importlib.util.spec_from_file_location("perf_summary", os.path.join(REPO, "tools", "xbox", "perf-summary.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--console", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--shape", action="append", default=[], help="draws,patches,level")
    parser.add_argument("--frames", type=int, default=1200)
    parser.add_argument("--seconds", type=int, default=420, help="backstop for one run")
    parser.add_argument("--no-package", action="store_true")
    parser.add_argument("--out", default=os.path.join(REPO, "build-uwp", "perf-tess"))
    args = parser.parse_args()
    shapes = [tuple(int(v) for v in s.split(",")) for s in args.shape] or [(1000, 1, 4), (200, 16, 8), (50, 64, 16)]
    args.scene_seconds = args.seconds
    os.makedirs(args.out, exist_ok=True)
    report = lambda text: print(text, flush=True)
    perf = load_perf_parser()

    target = console.ConsoleTarget(REPO, NRO, args, ["gpu_profile=1"], report)
    results = []
    try:
        for draws, patches, level in shapes:
            for flat in (0, 1):
                mode = os.path.join(target.work, "mode.txt")
                with open(mode, "w", newline="\n") as f:
                    f.write(f"tess {args.frames} {draws} {patches} {level} {flat}\n")
                target.portal.upload(target.package, console.SDMC, mode)
                name = f"{draws}x{patches}-L{level}-{'flat' if flat else 'tess'}"
                report(f"run {name}")
                exited, log = target.run()
                path = os.path.join(args.out, f"log-{name}.txt")
                with open(path, "w", encoding="utf-8") as f:
                    f.write(log)
                windows = perf.parse(path)[1:]
                done = "PERF_DONE" in log
                if not windows:
                    report(f"  no perf windows (exited {exited}, done {done})")
                    results.append({"name": name, "done": done})
                    continue
                med = lambda key: statistics.median(w[key] for w in windows if key in w)
                row = {"name": name, "done": done, "windows": len(windows), "fps": med("fps"), "p50": med("p50"),
                       "p99": med("p99"), "gpu_ms": med("gpu"), "idle_ms": med("idle"), "draws": med("draws"),
                       "app_mib": max((w.get("app_mib", 0) for w in windows), default=0)}
                results.append(row)
                report("  " + json.dumps(row))
    finally:
        target.portal.delete(target.package, console.SDMC, "mode.txt")
        target.close()

    with open(os.path.join(args.out, "results.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1)
    print(f"{'run':<22} {'fps':>6} {'p50':>7} {'p99':>7} {'gpu ms':>7} {'idle ms':>8} {'MiB':>6}")
    for r in results:
        if "fps" in r:
            print(f"{r['name']:<22} {r['fps']:6.1f} {r['p50']:7.2f} {r['p99']:7.2f} {r['gpu_ms']:7.2f} "
                  f"{r['idle_ms']:8.2f} {r['app_mib']:6}")
        else:
            print(f"{r['name']:<22} no measurement")
    return 0


if __name__ == "__main__":
    sys.exit(main())
