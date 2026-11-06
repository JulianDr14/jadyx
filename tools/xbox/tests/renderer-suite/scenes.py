# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""The scenes and perf layers of suite.py: the guest payload through the whole emulator on this PC.

Scenes: the payload runs every scene and reports START/PROBE/END lines (sdmc results.txt, and the same
lines in the emulator's log). A scene that hangs or takes the emulator down is the last one started:
it goes into skip.txt and the payload runs again without it. Each scene is then compared on three
things against baselines/:
    status    pass / fail / crash, from what the scene itself checks (scenes.json, any machine)
    picture   its last read-back target, pixel by pixel (pictures-<machine>.zip)
    debug     D3D12 debug-layer message ids and renderer errors logged while it ran (debug.json)

Perf: the payload's fixed draw-heavy loop under gpu_profile=1. Its 300-frame windows give counters
that must not grow (draws, pipeline creates, stalls, downloads) and timings with a margin
(perf-<machine>.json).
"""
import glob
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import time
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
NRO = os.path.join(REPO, "build-uwp", "payloads", "renderer-suite", "renderer_suite.nro")
MACHINE = os.environ.get("COMPUTERNAME", "pc").lower()

KINDS = ["unorm", "snorm", "uint", "sint", "float", "raw"]
SCENE_LINE = re.compile(r"\b(START|END|PROBE|SKIP|ALL_DONE|DEKO_ERROR|PERF_START|PERF_DONE)\b ?(.*)$")
# Scenes need every draw drawn: with asynchronous pipelines a draw whose pipeline is still building is
# skipped, and the scene would test the compile workers instead of the renderer. They read shader results
# back right after a fence, which is only exact with exact GPU reads (off by default): --boot-cfg
# exact_gpu_reads=0 runs what players get, and the scenes listed in xbox_renderer_suite.md then fail.
SCENE_BOOT_CFG = ["async_pipelines=off", "exact_gpu_reads=1"]
DEBUG_ID = re.compile(r"D3D12 debug layer \[(\d+)\]")
RENDER_ERROR = re.compile(r"\bRender(?:\.\w+)? <(Error|Critical)>")
# Perf windows: the first one includes start-up; timings may move this much before it is a regression.
FPS_MARGIN = 0.15
P95_MARGIN = 0.15


def powershell(command):
    out = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True)
    return out.stdout.strip()


def local_state():
    family = powershell("(Get-AppxPackage -Name Jadyx).PackageFamilyName")
    if not family:
        return None
    return os.path.join(os.environ["LOCALAPPDATA"], "Packages", family, "LocalState")


def sdmc_dir(state):
    # The payload's sdmc:/renderer_suite. Found where an earlier run left it, else the usual place.
    found = glob.glob(os.path.join(state, "**", "renderer_suite", "results.txt"), recursive=True)
    if found:
        return os.path.dirname(found[0])
    return os.path.join(state, "jadyx", "sdmc", "renderer_suite")


def jadyx_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq jadyx.exe", "/NH"], capture_output=True, text=True)
    return "jadyx.exe" in out.stdout


def stop_jadyx():
    subprocess.run(["taskkill", "/F", "/IM", "jadyx.exe"], capture_output=True)


def wait_for_exit(diag, deadline):
    """True if the app wrote its exit (or crash) line before the deadline."""
    while time.monotonic() < deadline:
        if os.path.exists(diag):
            with open(diag, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if "exiting" in text or "CRASH" in text:
                return True
        time.sleep(0.5)
    return False


def launch(package, run_seconds, debug_layer, boot_cfg, state):
    """Runs the payload once. package=True repackages the layout through local-run.ps1 (needed when
    the boot.cfg changes); otherwise the registered package is launched as it is. Returns (exited,
    log text)."""
    stop_jadyx()
    diag = os.path.join(state, "jadyx", "log", "jadyx_diag.txt") if state else None
    timeout = run_seconds + 90
    if package:
        command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                   os.path.join(REPO, "tools", "xbox", "local-run.ps1"), "-NoBuild", "-RequireShaders", "-BootNro", NRO,
                   "-RunSeconds", str(run_seconds), "-TimeoutSec", str(timeout)]
        if debug_layer:
            command.append("-DebugLayer")
        if boot_cfg:
            command += ["-BootCfg", ",".join(boot_cfg)]
        # Register under the version already on the PC: a lower one is refused, and the suite never
        # uninstalls.
        installed = powershell("(Get-AppxPackage -Name Jadyx).Version")
        if installed:
            command += ["-Version", installed]
        out = subprocess.run(command, cwd=REPO, capture_output=True, text=True, stdin=subprocess.DEVNULL)
        if out.returncode != 0:
            raise RuntimeError("local-run.ps1 failed:\n" + (out.stdout + out.stderr)[-3000:])
        state = local_state()
        diag = os.path.join(state, "jadyx", "log", "jadyx_diag.txt")
        exited = wait_for_exit(diag, time.monotonic() + 1)
    else:
        if os.path.exists(diag):
            os.remove(diag)
        family = os.path.basename(os.path.dirname(state))
        powershell(f"Start-Process 'shell:AppsFolder\\{family}!App'")
        exited = wait_for_exit(diag, time.monotonic() + timeout)
    if not exited:
        stop_jadyx()
    log_path = os.path.join(state, "jadyx", "log", "jadyx_log.txt")
    log = ""
    if os.path.exists(log_path):
        with open(log_path, encoding="utf-8", errors="replace") as f:
            log = f.read()
    return exited, log


# ---------------------------------------------------------------------------------------- scenes


def parse_results(lines):
    """Scene statuses and probes from the payload's report lines, the scene it stopped in, and whether
    deko3d rejected that scene."""
    statuses, probes, current, done, rejected = {}, {}, None, False, False
    for line in lines:
        m = SCENE_LINE.search(line)
        if not m:
            continue
        word, rest = m[1], m[2].strip()
        if word == "START":
            current = rest
        elif word == "END":
            name, _, status = rest.rpartition(" ")
            statuses[name] = status
            current = None
        elif word == "PROBE":
            name, _, text = rest.partition(" ")
            probes.setdefault(name, []).append(text)
        elif word == "ALL_DONE":
            done = True
        elif word == "DEKO_ERROR" and current:
            rejected = True
            probes.setdefault(current, []).append("deko3d rejected it: " + rest)
    return statuses, probes, current, done, rejected


def debug_by_scene(log):
    """D3D12 debug-layer ids and renderer errors, each under the scene running when it was logged."""
    found, scene = {}, "(start-up)"
    for line in log.splitlines():
        if "Debug_Emulated" in line or "Debug.Emulated" in line:
            m = SCENE_LINE.search(line)
            if m and m[1] == "START":
                scene = m[2].strip()
            elif m and m[1] in ("END", "ALL_DONE"):
                scene = "(between scenes)"
            continue
        m = DEBUG_ID.search(line)
        if m:
            found.setdefault(scene, set()).add(f"d3d12-{m[1]}")
        elif RENDER_ERROR.search(line):
            text = line.split(">", 1)[-1].split(":", 3)[-1].strip()
            text = re.sub(r"0x[0-9a-fA-F]+|\d+", "#", text)[:120]
            found.setdefault(scene, set()).add("error: " + text)
    return {scene: sorted(ids) for scene, ids in found.items()}


class PcTarget:
    """Runs the payload on this PC, registered loose through local-run.ps1."""

    def __init__(self, args):
        self.args = args
        self.state = local_state()
        self.package = not args.no_package or self.state is None
        self.sdmc = sdmc_dir(self.state) if self.state else None
        if self.sdmc and os.path.isdir(self.sdmc):
            shutil.rmtree(self.sdmc)

    def prepare(self, skip, only):
        if not self.sdmc:
            return
        os.makedirs(self.sdmc, exist_ok=True)
        with open(os.path.join(self.sdmc, "skip.txt"), "w", newline="\n") as f:
            f.write("".join(name + "\n" for name in skip))
        with open(os.path.join(self.sdmc, "only.txt"), "w", newline="\n") as f:
            f.write("".join(name + "\n" for name in only))
        results_path = os.path.join(self.sdmc, "results.txt")
        if os.path.exists(results_path):
            os.remove(results_path)

    def run(self):
        exited, log = launch(self.package, self.args.scene_seconds, not self.args.no_debug_layer,
                             scene_boot_cfg(self.args), self.state)
        self.package = False
        self.state = local_state()
        self.sdmc = sdmc_dir(self.state)
        return exited, log

    def results(self):
        results_path = os.path.join(self.sdmc, "results.txt")
        if not os.path.exists(results_path):
            return []
        with open(results_path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()

    def pictures(self, dest):
        for path in glob.glob(os.path.join(self.sdmc, "*.bin")):
            shutil.copy(path, dest)

    def dumps(self, dest):
        log_dir = os.path.join(self.state, "jadyx", "log") if self.state else ""
        for pattern in ("shader_*", "dxil_*"):
            for path in glob.glob(os.path.join(log_dir, pattern)):
                shutil.copy(path, dest)

    def close(self):
        pass


def scene_boot_cfg(args):
    """The scenes' boot.cfg plus --boot-cfg's lines (diagnosis: dump_shader=..., log_filter=...)."""
    return SCENE_BOOT_CFG + list(args.boot_cfg)


def run_scenes(args, out_dir, report):
    if args.console:
        import console
        target = console.ConsoleTarget(REPO, NRO, args, scene_boot_cfg(args), report)
    else:
        target = PcTarget(args)
    try:
        return run_scenes_on(target, args, out_dir, report)
    finally:
        target.close()


def run_scenes_on(target, args, out_dir, report):
    skip, crashed, statuses, probes, log_all = [], {}, {}, {}, []
    for attempt in range(1, 40):
        # --only runs just those scenes, so what one of them logs is not cut by the others.
        target.prepare(skip, args.only)
        exited, log = target.run()
        log_all.append(log)
        with open(os.path.join(out_dir, f"scenes-log-{attempt}.txt"), "w", encoding="utf-8") as f:
            f.write(log)
        got, got_probes, stopped_in, done, rejected = parse_results(target.results())
        statuses.update(got)
        probes.update(got_probes)
        report(f"scenes run {attempt}: {len(got)} scenes ended" + ("" if done else f", stopped in {stopped_in}")
               + ("" if exited else " (hung, killed)"))
        if done:
            break
        if not stopped_in or stopped_in in skip:
            report("scenes: the payload stopped outside any scene; see " + out_dir)
            statuses["(payload)"] = "crash"
            break
        # deko3d stopping the program means the scene asks for something the hardware cannot do: a bug
        # in the scene, not in the emulator.
        crashed[stopped_in] = "invalid" if rejected else "crash" if exited else "hang"
        skip.append(stopped_in)
    for name, how in crashed.items():
        statuses[name] = how
    pictures = os.path.join(out_dir, "pictures")
    os.makedirs(pictures, exist_ok=True)
    target.pictures(pictures)
    # Shader dumps (--boot-cfg dump_shader=...) land in the log folder.
    target.dumps(out_dir)
    debug = debug_by_scene("\n".join(log_all)) if not args.no_debug_layer else None
    return statuses, probes, pictures, debug


def read_picture(data):
    width, height, size, kind_bits = struct.unpack_from("<4I", data)
    return width, height, size, KINDS[(kind_bits >> 8) & 0xFF], kind_bits & 0xFF, data[16:]


def channel_values(kind, bits, size, pixels):
    """Every channel of every pixel as a float, and the tolerance one pixel may move by."""
    if kind == "raw" or kind in ("uint", "sint"):
        return list(pixels), 0.0
    if kind in ("unorm", "snorm") and bits == 8:
        fmt = "b" if kind == "snorm" else "B"
        return list(struct.unpack(f"<{len(pixels)}{fmt}", pixels)), 2.0
    if kind in ("unorm", "snorm") and bits == 16:
        fmt = "h" if kind == "snorm" else "H"
        return list(struct.unpack(f"<{len(pixels) // 2}{fmt}", pixels)), 2.0 * 257
    if kind == "float" and bits == 16:
        return list(struct.unpack(f"<{len(pixels) // 2}e", pixels)), 0.005
    if kind == "float" and bits == 32:
        return list(struct.unpack(f"<{len(pixels) // 4}f", pixels)), 1e-4
    return list(pixels), 0.0


def compare_picture(expected, actual):
    """None if the same within tolerance, else what differs."""
    a, b = read_picture(expected), read_picture(actual)
    if a[:5] != b[:5]:
        return f"size or format changed: {a[:5]} -> {b[:5]}"
    values_a, tolerance = channel_values(a[3], a[4], a[2], a[5])
    values_b, _ = channel_values(b[3], b[4], b[2], b[5])
    per_pixel = max(1, len(values_a) // (a[0] * a[1]))
    differing = set()
    for i, (x, y) in enumerate(zip(values_a, values_b)):
        if x != y and not (abs(x - y) <= tolerance):
            differing.add(i // per_pixel)
    if not differing:
        return None
    first = min(differing)
    return f"{len(differing)} of {a[0] * a[1]} pixels differ (first at {first % a[0]},{first // a[0]})"


def write_png(path, width, height, rgba):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def picture_png(data, path):
    """A viewable copy of a read-back: each channel mapped to 0-255, missing ones as 0 (alpha 255)."""
    width, height, size, kind, bits, pixels = read_picture(data)
    values, _ = channel_values(kind, bits, size, pixels)
    per_pixel = max(1, len(values) // (width * height))
    if kind == "raw":
        return
    top = {"unorm": 255.0 if bits == 8 else 65535.0, "snorm": 127.0 if bits == 8 else 32767.0}.get(kind)
    if top is None:
        top = max(1.0, max((abs(v) for v in values), default=1.0))
    out = bytearray()
    for p in range(width * height):
        px = [values[p * per_pixel + c] if c < per_pixel else (top if c == 3 else 0) for c in range(4)]
        out += bytes(max(0, min(255, int(round(v / top * 255)))) for v in px)
    write_png(path, width, height, bytes(out))


def compare_pictures(pictures, baseline_zip, ran, update, report):
    regressions, new = [], []
    expected = {}
    if os.path.exists(baseline_zip):
        with zipfile.ZipFile(baseline_zip) as z:
            expected = {name: z.read(name) for name in z.namelist()}
    current = {}
    for path in sorted(glob.glob(os.path.join(pictures, "*.bin"))):
        with open(path, "rb") as f:
            current[os.path.basename(path)] = f.read()
    for name, data in current.items():
        if name not in expected:
            new.append(name)
            continue
        difference = compare_picture(expected[name], data)
        if difference:
            scene = name[:-4]
            regressions.append(f"{scene}: picture {difference}")
            picture_png(expected[name], os.path.join(pictures, f"{scene}.expected.png"))
            picture_png(data, os.path.join(pictures, f"{scene}.actual.png"))
    # Only the scenes this run ran (--only runs a few) can miss their picture.
    for name in sorted(n for n in set(expected) - set(current) if n[:-4] in ran):
        regressions.append(f"{name[:-4]}: picture missing")
    if update:
        merged = dict(expected)
        merged.update(current)
        with zipfile.ZipFile(baseline_zip, "w", zipfile.ZIP_DEFLATED) as z:
            for name in sorted(merged):
                z.writestr(name, merged[name])
    return regressions, new


def compare_debug(current, baseline):
    """Debug-layer ids or renderer errors a scene did not have before."""
    regressions, gone = [], []
    for scene, ids in sorted(current.items()):
        before = set((baseline or {}).get(scene, []))
        for item in ids:
            if baseline is not None and item not in before:
                regressions.append(f"{scene}: {item}")
    for scene, ids in sorted((baseline or {}).items()):
        for item in ids:
            if item not in current.get(scene, []):
                gone.append(f"{scene}: {item}")
    return regressions, gone


# ------------------------------------------------------------------------------------------ perf


def load_perf_parser():
    spec = importlib.util.spec_from_file_location("perf_summary", os.path.join(REPO, "tools", "xbox", "perf-summary.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_perf(args, out_dir, report):
    """One measurement: median over the windows after the first."""
    state = local_state()
    sdmc = sdmc_dir(state) if state else None
    if sdmc:
        os.makedirs(sdmc, exist_ok=True)
        with open(os.path.join(sdmc, "mode.txt"), "w", newline="\n") as f:
            f.write(f"perf {args.perf_frames}\n")
    try:
        exited, log = launch(True, args.perf_seconds, False, ["gpu_profile=1"], state)
    finally:
        sdmc = sdmc_dir(local_state())
        mode = os.path.join(sdmc, "mode.txt")
        if os.path.exists(mode):
            os.remove(mode)
    path = os.path.join(out_dir, f"perf-log-{int(time.time())}.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write(log)
    if "PERF_DONE" not in log:
        report("perf: the loop did not finish" + ("" if exited else " (hung, killed)"))
        return None
    windows = [w for w in load_perf_parser().parse(path) if w.get("draws", 0) >= 1000]
    if len(windows) < 2:
        report(f"perf: only {len(windows)} windows with the loop's draws; raise --perf-frames")
        return None
    steady = windows[1:]

    def median(key):
        values = sorted(w.get(key, 0) for w in steady)
        return values[len(values) // 2]

    return {"fps": median("fps"), "p95": median("p95"), "gpu_ms": median("gpu"), "idle_ms": median("idle"),
            "draws": median("draws"), "pso": max(w.get("pso", 0) for w in steady),
            "stalls": max(w.get("stalls", 0) for w in steady), "new_res": max(w.get("new_res", 0) for w in steady),
            "downloads": max(w.get("down", 0) for w in steady), "windows": len(steady)}


def compare_perf(current, baseline):
    regressions, improved = [], []
    if not baseline:
        return regressions, improved
    if current["draws"] != baseline["draws"]:
        regressions.append(f"draws per frame {baseline['draws']} -> {current['draws']}")
    for key in ("pso", "stalls", "new_res", "downloads"):
        if current[key] > baseline[key]:
            regressions.append(f"{key} per window {baseline[key]} -> {current[key]}")
    if current["fps"] < baseline["fps"] * (1 - FPS_MARGIN):
        regressions.append(f"fps {baseline['fps']:.1f} -> {current['fps']:.1f}")
    elif current["fps"] > baseline["fps"] * (1 + FPS_MARGIN):
        improved.append(f"fps {baseline['fps']:.1f} -> {current['fps']:.1f}")
    if current["p95"] > baseline["p95"] * (1 + P95_MARGIN) + 1.0:
        regressions.append(f"p95 {baseline['p95']:.1f} ms -> {current['p95']:.1f} ms")
    return regressions, improved


# ---------------------------------------------------------------------------------------- driver


def run_layers(layers, args, out_dir, lines, load_baseline, save_baseline, compare_status, report_section):
    """Runs the scenes and perf layers; returns True when anything regressed."""
    def report(text):
        print(text, flush=True)

    for name, default in (("no_debug_layer", False), ("scene_seconds", 180), ("perf_frames", 1500),
                          ("perf_seconds", 400), ("perf_runs", 2), ("console", None)):
        if not hasattr(args, name):
            setattr(args, name, default)
    machine = MACHINE
    if args.console:
        # The console has no debug layer, and its perf runs go through tools/xbox/perf-run.ps1.
        args.no_debug_layer = True
        machine = "series"
        if "perf" in layers:
            lines.append("--- perf: not on the console; use tools/xbox/perf-run.ps1")
            layers = layers - {"perf"}
    if not os.path.exists(NRO):
        lines.append(f"--- scenes/perf: payload not built ({NRO}); run build-payload.ps1")
        return True
    failed = False

    if "scenes" in layers:
        statuses, probes, pictures, debug = run_scenes(args, out_dir, report)
        if args.only:
            statuses = {k: v for k, v in statuses.items() if k in args.only}
        baseline = load_baseline("scenes.json")
        regressions, improved, known, new = compare_status(statuses, baseline)
        regressions = [r + "".join(f"\n      {p}" for p in probes.get(r.split(":")[0], [])) for r in regressions]
        report_section("scenes", regressions, improved, known, new, lines)
        failed |= bool(regressions)

        picture_regressions, picture_new = compare_pictures(
            pictures, os.path.join(os.path.dirname(os.path.abspath(__file__)), "baselines", f"pictures-{machine}.zip"),
            set(statuses), args.update_baseline and not args.only, report)
        report_section("pictures", picture_regressions, [], [], picture_new, lines)
        failed |= bool(picture_regressions)

        if debug is not None:
            debug_baseline = load_baseline("debug.json")
            debug_regressions, gone = compare_debug(debug, debug_baseline)
            report_section("debug layer", debug_regressions, gone, [], [], lines)
            failed |= bool(debug_regressions)
        with open(os.path.join(out_dir, "scenes.json"), "w", encoding="utf-8") as f:
            json.dump({"statuses": statuses, "probes": probes, "debug": debug}, f, indent=1, sort_keys=True)
        # The statuses baseline is the PC's: the console's results are compared with it, never saved over it.
        if args.update_baseline and not args.console:
            merged = dict(baseline or {})
            merged.update(statuses)
            save_baseline("scenes.json", merged)
            if debug is not None and not args.only:
                save_baseline("debug.json", debug)

    if "perf" in layers:
        runs = [r for r in (run_perf(args, out_dir, report) for _ in range(args.perf_runs)) if r]
        name = f"perf-{MACHINE}.json"
        if not runs:
            lines.append("--- perf: no measurement")
            lines.append("  REGRESSION perf loop did not finish")
            failed = True
        else:
            best = max(runs, key=lambda r: r["fps"])
            best["p95"] = min(r["p95"] for r in runs)
            baseline = load_baseline(name)
            regressions, improved = compare_perf(best, baseline)
            report_section("perf", regressions, improved, [], [] if baseline else ["perf"], lines)
            lines.append("  " + ", ".join(f"{k} {v:.2f}" if isinstance(v, float) else f"{k} {v}"
                                          for k, v in sorted(best.items())))
            failed |= bool(regressions)
            with open(os.path.join(out_dir, "perf.json"), "w", encoding="utf-8") as f:
                json.dump(runs, f, indent=1)
            if args.update_baseline:
                save_baseline(name, best)
    return failed
