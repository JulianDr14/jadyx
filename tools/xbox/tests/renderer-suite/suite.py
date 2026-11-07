# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""The renderer's regression suite. See docs/xbox/xbox_renderer_suite.md.

    python tools/xbox/tests/renderer-suite/suite.py [--layers gates,scenes,perf] [--only NAME ...]
        [--no-package] [--update-baseline] [--list] [--console 192.168.1.50 --version 0.3.9.159]

Layers:
    gates   every component gate that touches the renderer, one process each.
    scenes  the guest scenes (payload/) through the whole emulator, on this PC's GPU, compared with
            what each scene states it must draw, with the reference pictures and with the
            D3D12 debug layer.
    perf    the same emulator on a draw-heavy guest scene: counters exactly, timings with a margin.

--console runs the scenes on the console through the Device Portal (console.py) and leaves the payload
installed there; the console's pictures baseline is pictures-series.zip.

The result says what changed against baselines/: REGRESSION fails the run, IMPROVED asks for
--update-baseline, KNOWN is a failure the baseline already has. Run from anywhere; exit code 0 means
no regressions. Output goes to build-uwp/renderer-suite/<time>/.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
BASELINES = os.path.join(HERE, "baselines")
VSWHERE_DIR = os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft Visual Studio", "Installer")

# Component gates that touch the renderer. Those needing a capture or game data are not here
# (multipass-position, noperspective-clip: both take a capture file as input).
GATES = [
    "async-download-supersede", "async-pipelines", "bc-block-copy", "bc-npot", "cached-staging",
    "compute-pipeline-claim", "cpu-texture-writes", "demote-helper", "depth-region-copy",
    "draw-preparation", "draw-sampler-cache", "draw-targets", "etc2-decode", "frame-gen",
    "frame-gen-suite", "full-capture", "geometry-passthrough", "gpu-unswizzle", "heap-compaction",
    "heap-growth", "index-rewrite-cache", "memory-edit", "memory-import-probe",
    "noperspective-lowering", "pipeline-key-fold", "pipeline-key-tracking",
    "pipeline-transition-order", "present-queue", "pso-blobs", "pso-cache", "recorder-d3d12",
    "reduction-sampling", "render-corrections", "srv-dimensions", "texture-replay",
    "timed-wait-profile", "transfer-gates", "upload-copy", "xfb-stream-output",
]
# Standalone unit tests of the renderer without a run script of their own: run-unit.ps1 builds them.
UNITS = [
    "bc45-decode", "cache-pressure", "command-recorder", "descriptor-copy", "fsr-plan", "linked-shader-cache",
    "memory-guard", "overlay-memory", "pipeline-residency", "prewarm-budget", "spsc-wake-queue",
]


def load_baseline(name):
    try:
        with open(os.path.join(BASELINES, name), encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None


def save_baseline(name, data):
    os.makedirs(BASELINES, exist_ok=True)
    with open(os.path.join(BASELINES, name), "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")


def kill_tree(pid):
    subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], capture_output=True)


def run_gate(name, out_dir, timeout):
    if name.startswith("unit-"):
        command = ["-File", os.path.join(HERE, "run-unit.ps1"), "-Name", name[5:]]
    else:
        command = ["-File", os.path.join("tools", "xbox", "tests", f"run-{name}.ps1")]
    log_path = os.path.join(out_dir, f"gate-{name}.log")
    env = dict(os.environ)
    env["PATH"] = VSWHERE_DIR + os.pathsep + env.get("PATH", "")
    # Some shells hide the current directory from cmd; the gate scripts run their exe by bare name.
    env.pop("NoDefaultCurrentDirectoryInExePath", None)
    start = time.monotonic()
    with open(log_path, "wb") as log:
        process = subprocess.Popen(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass"] + command,
            cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try:
            code = process.wait(timeout=timeout)
            status = "pass" if code == 0 else "fail"
        except subprocess.TimeoutExpired:
            kill_tree(process.pid)
            process.wait()
            status = "timeout"
    return {"status": status, "seconds": round(time.monotonic() - start, 1), "log": log_path}


def run_gates(args, out_dir):
    names = [g for g in GATES + [f"unit-{u}" for u in UNITS] if not args.only or g in args.only]
    results = {}
    for index, name in enumerate(names, 1):
        result = run_gate(name, out_dir, args.gate_timeout)
        results[name] = result
        print(f"[{index}/{len(names)}] gate {name}: {result['status']} ({result['seconds']} s)", flush=True)
    return results


def compare_status(current, baseline):
    """Pass/fail items against the baseline's statuses: new failures are regressions."""
    regressions, improved, known, new = [], [], [], []
    for name, status in sorted(current.items()):
        before = (baseline or {}).get(name)
        if status == "pass":
            if before not in (None, "pass"):
                improved.append(name)
            elif before is None:
                new.append(name)
        elif before is None or before == "pass":
            regressions.append(f"{name}: {status}")
        else:
            known.append(f"{name}: {status}")
    return regressions, improved, known, new


def report_section(title, regressions, improved, known, new, lines):
    lines.append(f"--- {title}: {len(regressions)} regressions, {len(improved)} improved, "
                 f"{len(known)} known failures, {len(new)} without a baseline")
    lines.extend(f"  REGRESSION {item}" for item in regressions)
    lines.extend(f"  IMPROVED   {item}" for item in improved)
    lines.extend(f"  KNOWN      {item}" for item in known)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--layers", default="gates,scenes,perf")
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--no-package", action="store_true", help="scenes/perf: reuse the installed layout")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--gate-timeout", type=int, default=1200)
    parser.add_argument("--no-debug-layer", action="store_true", help="scenes: run without the D3D12 debug layer")
    parser.add_argument("--scene-seconds", type=int, default=180, help="scenes: backstop for one payload run")
    parser.add_argument("--perf-frames", type=int, default=1500)
    parser.add_argument("--perf-seconds", type=int, default=400, help="perf: backstop for one payload run")
    parser.add_argument("--perf-runs", type=int, default=2, help="perf: runs; the best one counts")
    parser.add_argument("--console", help="scenes: run on the console at this address instead of this PC")
    parser.add_argument("--version", help="--console: package version, above the one installed there")
    parser.add_argument("--boot-cfg", nargs="*", default=[], help="scenes: extra boot.cfg lines, for diagnosis")
    args = parser.parse_args()
    layers = set(args.layers.split(","))
    if args.console and not args.version:
        parser.error("--console needs --version")

    sys.path.insert(0, HERE)
    if args.list:
        print("gates:", " ".join(GATES + [f"unit-{u}" for u in UNITS]))
        return 0

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join(REPO, "build-uwp", "renderer-suite", stamp)
    os.makedirs(out_dir, exist_ok=True)
    lines, failed = [], False

    if "gates" in layers:
        results = run_gates(args, out_dir)
        statuses = {name: r["status"] for name, r in results.items()}
        baseline = load_baseline("gates.json")
        regressions, improved, known, new = compare_status(statuses, baseline)
        report_section("gates", regressions, improved, known, new, lines)
        failed |= bool(regressions)
        if args.update_baseline:
            merged = dict(baseline or {})
            merged.update(statuses)
            save_baseline("gates.json", merged)
        with open(os.path.join(out_dir, "gates.json"), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)

    if layers & {"scenes", "perf"}:
        import scenes
        failed |= scenes.run_layers(layers, args, out_dir, lines, load_baseline, save_baseline, compare_status,
                                    report_section)

    lines.append("RESULT: " + ("REGRESSIONS" if failed else "no regressions") + f" | {out_dir}")
    summary = "\n".join(lines)
    print(summary)
    with open(os.path.join(out_dir, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(summary + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
