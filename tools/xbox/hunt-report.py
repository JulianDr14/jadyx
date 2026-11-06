# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bottleneck report from a hunt capture ("Capture performance" in the game menu of a package
built with package-appx.ps1 -Hunt, or F8 with developer hotkeys).

    python tools/xbox/hunt-report.py jadyx_hunt.txt [--capture N] [--top 15] [--out report.md]
        [--profile jadyx_hunt_profile_N.txt] [--exe build-uwp/bin/jadyx.exe] [--no-profile]

The files are in LocalState/jadyx/log on the console. The report opens with why the GPU sits idle:
what the game asked for and which link of the frame chain (guest cores, GPU thread, recorder, GPU)
sets the pace, over the seconds of the CPU profile. Next, which guest code the busiest emulated
core runs: its hottest blocks, disassembled with capstone if installed, and those that look like a
wait. Then it says who limits the captured frames,
where the draws come from and what they cost: per kind and skip reason, per render target (pass),
per pipeline, per guest macro, runs of draws that could be batched, and repeated draw signatures.
With the capture's CPU profile next to it (or --profile), profile-report.py's stacks are appended.

GPU times are timestamp deltas: each call's time runs from the previous call of the same command
list, so barriers, copies and uploads recorded in between count for the call after them. The first
call of a command list has a time since the list opened (newer captures). "list tail" is what a
list records after its last call: the composite, query resolves and copies before the submit.
"dispatch setup" is what a dispatch's bindings record before it (newer captures): uploads, copies
and barriers, kept apart from the shader's own time.
Timestamps are bottom-of-pipe, so overlapping draws share time loosely; a pass's total is sound.
"""
import argparse
import collections
import importlib.util
import os
import re
import subprocess
import sys

try:
    import capstone
except ImportError:
    capstone = None

KINDS = ["draw", "draw indexed", "draw indirect", "draw texture", "clear", "dispatch", "2D blit",
         "DMA copy", "list tail", "dispatch setup"]
LIST_END, DISPATCH_SETUP = 8, 9
OUTCOMES = ["recorded", "skipped", "conditional", "culled", "no pipeline", "PSO rejected",
            "skipped shader", "empty", "no target", "expanded on CPU"]
RECORDED, EXPANDED = 0, 9
CPU_INDIRECT, EMULATED_TOPOLOGY = 1, 2
TOPOLOGIES = ["points", "lines", "line loop", "line strip", "triangles", "triangle strip",
              "triangle fan", "quads", "quad strip", "polygon", "lines adj", "line strip adj",
              "triangles adj", "triangle strip adj", "patches"]
DXGI = {0: "-", 2: "RGBA32F", 10: "RGBA16F", 11: "RGBA16", 16: "RG32F", 20: "D32S8",
        24: "RGB10A2", 26: "R11G11B10F", 28: "RGBA8", 29: "RGBA8_SRGB", 34: "RG16F", 35: "RG16",
        40: "D32F", 41: "R32F", 42: "R32U", 45: "D24S8", 49: "RG8", 54: "R16F", 55: "D16",
        56: "R16", 61: "R8", 87: "BGRA8", 91: "BGRA8_SRGB"}


class Record:
    __slots__ = ("kind", "outcome", "flags", "frame", "pass_", "vs", "ps", "count", "instances",
                 "topology", "cpu_ns", "pipeline_ns", "gpu_ns", "macro")

    def __init__(self, fields):
        self.kind, self.outcome, self.flags, self.frame = (int(x) for x in fields[:4])
        self.pass_ = int(fields[4])
        self.vs, self.ps = fields[5], fields[6]
        self.count, self.instances, self.topology = int(fields[7]), int(fields[8]), int(fields[9])
        self.cpu_ns, self.pipeline_ns = int(fields[10]), int(fields[11])
        self.gpu_ns = None if fields[12] == "-" else int(fields[12])
        self.macro = int(fields[13], 16)


class Capture:
    def __init__(self, number):
        self.number = number
        self.frames = []
        self.passes = {}
        self.perf = {}
        self.meta = {}
        self.records = []
        self.dropped = 0
        self.chain = {}


def parse(path):
    captures = []
    current = None
    chains = collections.defaultdict(dict)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            tag = p[0]
            if tag == "hunt":
                current = Capture(int(p[3]))
                current.dropped = int(p[9])
                captures.append(current)
            elif tag == "chain":
                chains[int(p[1])][p[2]] = float(p[3])
            elif current is None:
                continue
            elif tag == "r":
                current.records.append(Record(p[1:]))
            elif tag == "frame":
                current.frames.append(float(p[2]))
            elif tag == "pass":
                current.passes[int(p[1])] = describe_pass(p[2:])
            elif tag == "perf":
                current.perf[p[1]] = int(p[2])
            elif tag == "meta":
                it = iter(p[1:])
                current.meta = {k: float(v) for k, v in zip(it, it)}
    for capture in captures:
        capture.chain = chains.get(capture.number, {})
    return captures


def profile_threads(path):
    """CPU ms per second of each thread in a sampling profile; empty when there is no file."""
    if not path or not os.path.exists(path):
        return {}
    spec = importlib.util.spec_from_file_location(
        "profile_report", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       "profile-report.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    windows = module.parse(path).windows
    seconds = sum(w.seconds for w in windows)
    totals = collections.Counter()
    for window in windows:
        for name, stats in window.threads.items():
            totals[name] += stats[0]
    return {name: cpu_ms / seconds for name, cpu_ms in totals.items()} if seconds else {}


def parse_guest_code(path):
    """The guest code of a sampling profile: samples per thread and block, bytes of the hottest."""
    samples = collections.defaultdict(collections.Counter)  # thread -> guest pc -> samples
    calls = collections.Counter()  # (thread, guest pc) -> samples inside its callbacks
    thunks = collections.Counter()  # (thread, guest pc of the caller) -> samples in memory thunks
    code, base = {}, None
    if not path or not os.path.exists(path):
        return samples, calls, thunks, code, base
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "guest":
                pc = int(p[2], 16)
                samples[p[1]][pc] += int(p[4]) + int(p[5])
                calls[(p[1], pc)] += int(p[5])
            elif p[0] == "guestthunk":
                pc = int(p[2], 16)
                thunks[(p[1], pc)] += int(p[4])
                samples[p[1]][pc] += 0  # a block seen only through its thunk calls still counts
            elif p[0] == "guestbase":
                base = int(p[1], 16)
            elif p[0] == "guestcode" and len(p) > 2:
                code[int(p[1], 16)] = bytes.fromhex(p[2])
    return samples, calls, thunks, code, base


BRANCH_TARGET = re.compile(r"#(0x[0-9a-f]+)$")
SIMD_REGISTER = re.compile(r"\b[vqdsh]\d+\b")
EXCLUSIVE = re.compile(r"^(ldx|ldax|stx|stlx|cas|swp|ldadd|ldset|ldclr)")


def describe_block(pc, data):
    """What a guest block does, from its instructions; None without capstone or code."""
    if capstone is None or not data:
        return None
    engine = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    insns = list(engine.disasm(data, pc))
    if not insns:
        return None
    mnemonics = [i.mnemonic.split(".")[0] for i in insns]
    last = insns[-1]
    target = BRANCH_TARGET.search(last.op_str)
    loops = bool(target) and int(target.group(1), 16) == pc and last.mnemonic.split(".")[0] in \
        ("b", "cbz", "cbnz", "tbz", "tbnz")
    loads = sum(m.startswith(("ldr", "ldp", "ldur", "ld1", "ld2", "ld3", "ld4")) for m in mnemonics)
    stores = sum(m.startswith(("str", "stp", "stur", "st1", "st2", "st3", "st4")) for m in mnemonics)
    exclusive = sum(bool(EXCLUSIVE.match(m)) for m in mnemonics)
    wait = sum(m in ("wfe", "wfi", "yield") for m in mnemonics)
    # A wait reads and tests, and writes nothing but perhaps its exclusive: short and closed.
    waiting = wait > 0 or (loops and len(insns) <= 12 and stores == 0)
    return dict(instructions=len(insns), loads=loads, stores=stores, exclusive=exclusive,
                simd=sum(bool(SIMD_REGISTER.search(i.op_str)) for i in insns), loops=loops,
                waiting=waiting, svc=mnemonics.count("svc"),
                text=[f"{i.address:x}: {i.mnemonic} {i.op_str}" for i in insns])


def guest_section(profile, threads, top, out):
    samples, calls, thunks, code, base = parse_guest_code(profile)
    cores = [name for name in samples if name.startswith("CPUCore_")]
    if not cores:
        out.append("# Guest code\n\nNo guest code in this profile: older build, or no CPU profile.\n")
        return
    core = max(cores, key=lambda name: threads.get(name, 0.0) or sum(samples[name].values()))
    blocks = samples[core]
    own = sum(n for pc, n in blocks.items() if pc)
    outside = blocks.get(0, 0)
    in_thunks = {pc: n for (name, pc), n in thunks.items() if name == core}
    thunk_total = sum(in_thunks.values())
    total = own + outside + thunk_total
    known = own + thunk_total - in_thunks.get(0, 0)
    ranked = sorted(((n + in_thunks.get(pc, 0), pc) for pc, n in blocks.items() if pc), reverse=True)
    share = lambda count: 100 * sum(n for n, _ in ranked[:count]) / max(1, known)
    percent = lambda part: f"{100 * part / max(1, total):.0f}%"
    out.append(f"# Guest code of the busiest core ({core})\n")
    out.append(f"- {total} samples in JIT code: **{percent(own)} in the game's blocks, "
               f"{percent(thunk_total)} in memory access thunks, {percent(outside)} in the rest of "
               f"the JIT** (dispatcher, memory fallbacks; with what they call). Without thunks in "
               f"the build, their accesses are inside the blocks.")
    out.append(f"- Thunk time includes waiting for the guest's data, which costs the same inline; "
               f"{in_thunks.get(0, 0)} thunk samples have no known caller.")
    out.append(f"- Concentration over {len(ranked)} blocks, thunk calls included: the hottest "
               f"{share(1):.0f}%, top 10 {share(10):.0f}%, "
               f"top 50 {share(50):.0f}%.")
    described = {pc: describe_block(pc, code.get(pc)) for _, pc in ranked if pc in code}
    if capstone is None:
        out.append("- capstone is not installed: no disassembly (pip install capstone).")
    else:
        looked_at = sum(n for n, pc in ranked if described.get(pc))
        waiting = sum(n for n, pc in ranked if (described.get(pc) or {}).get("waiting"))
        out.append(f"- Blocks that look like a wait (WFE/YIELD, or a short closed loop that stores "
                   f"nothing): **{100 * waiting / max(1, looked_at):.0f}%** of the "
                   f"{100 * looked_at / max(1, known):.0f}% of samples with code in the file. A "
                   "longer polling loop goes unnoticed: read the hottest blocks.")
    rows = []
    for n, pc in ranked[:top]:
        info = described.get(pc)
        offset = f"+0x{pc - base:x}" if base is not None and pc >= base else "-"
        cells = [f"{100 * n / max(1, total):.1f}%", f"0x{pc:x}", offset,
                 f"{100 * in_thunks.get(pc, 0) / max(1, n):.0f}%",
                 f"{100 * calls[(core, pc)] / max(1, n):.0f}%"]
        if info:
            flags = [name for name, on in (("wait?", info["waiting"]), ("loop", info["loops"]),
                                           ("exclusive", info["exclusive"]), ("svc", info["svc"]))
                     if on]
            cells += [info["instructions"], info["loads"], info["stores"], info["simd"],
                      ", ".join(flags) or "-"]
        else:
            cells += ["-"] * 5
        rows.append(cells)
    out.append("")
    out.append(table(["samples", "guest PC", "from code start", "in thunks", "in callbacks",
                      "instructions",
                      "loads", "stores", "SIMD/FP", "flags"], rows))
    disassembled = [(n, pc) for n, pc in ranked[:5] if described.get(pc)]
    for n, pc in disassembled:
        out.append(f"\n## Block 0x{pc:x} ({100 * n / max(1, total):.1f}% of the core)\n\n```")
        out += described[pc]["text"]
        out.append("```")
    out.append("")


def chain_links(chain, threads, frames):
    """(link, ms per frame) of each link that hands work to the next one, busiest first. The GPU
    thread counts as wall time without its waits: on a shared core it takes longer than its CPU
    time. The others have only their CPU time, so preemption makes them look lighter."""
    window_ms = chain["window_ms"]
    waits_us = sum(chain.get(name, 0) for name in ("GpuThreadIdleUs", "FenceWaitUs",
                                                   "GpuThreadFlushUs", "PipelineStallUs"))
    links = [("GPU thread (recording draws)", max(0.0, window_ms - waits_us / 1e3) / frames)]
    for name, cpu_ms_per_s in threads.items():
        if name.startswith("CPUCore_") or name.startswith("D3D12Recorder"):
            label = f"guest core {name[8:]}" if name.startswith("CPUCore_") else "command list recorder"
            links.append((label, cpu_ms_per_s * window_ms / frames / 1e3))
    return sorted(links, key=lambda link: -link[1])


def chain_verdict(chain, threads):
    """Why the GPU sits idle, in one conclusion; correlation over the window, not proof."""
    frames = chain.get("GuestFramesQueued", 0)
    if not frames:
        return "The game queued no frames in this window: nothing to conclude."
    frame_ms = chain["window_ms"] / frames
    refresh = 1000 / 60
    asked = int(chain.get("swap_interval_end", 0))
    gpu_ms = chain.get("GpuBusyUs", 0) / 1e3 / frames
    if gpu_ms >= 0.75 * frame_ms:
        return f"**The GPU is the limit**: busy {gpu_ms:.1f} of {frame_ms:.1f} ms per frame."
    if not threads:
        return "No CPU profile for this capture: the links other than the GPU thread are unknown."
    name, busiest = chain_links(chain, threads, frames)[0]
    idle = f"The GPU works {gpu_ms:.1f} ms and has nothing to do for {frame_ms - gpu_ms:.1f} ms."
    # The game's own pace: at or under what it asked for, it is waiting on purpose.
    if asked >= 1 and frame_ms <= asked * refresh * 1.03:
        if asked == 1:
            return f"**Nothing limits this window**: it holds the 60 FPS the game asks for. {idle}"
        faster = (asked - 1) * refresh
        reason = (f"{name} takes {busiest:.1f} ms, over the {faster:.1f} ms that "
                  f"{60 / (asked - 1):.0f} FPS needs: a likely reason it chose this rate."
                  if busiest >= 0.9 * faster else
                  f"The busiest link, {name}, takes {busiest:.1f} ms and fits the {faster:.1f} ms "
                  f"of {60 / (asked - 1):.0f} FPS: this looks like the game's own cap, not a "
                  "shortage of power.")
        return (f"**The game asked for {60 / asked:.0f} FPS** (swap interval {asked}) and gets it. "
                f"{reason} {idle}")
    # Slower than asked. Above 85% a link fills the frame; below, the time goes to waits between
    # links, and the wait table says which.
    if busiest >= 0.85 * frame_ms:
        return (f"**{name} sets the pace**: {busiest:.1f} of {frame_ms:.1f} ms per frame. The GPU "
                f"cannot work more than this link feeds it. {idle}")
    return (f"**No link fills the frame**: the busiest, {name}, takes {busiest:.1f} of "
            f"{frame_ms:.1f} ms. The time goes to waits between links: see the wait table. {idle}")


def chain_section(c, threads, out):
    chain = c.chain
    frames = chain.get("GuestFramesQueued", 0)
    out.append(f"# Why the GPU sits idle — capture {c.number}\n")
    if "window_ms" not in chain:
        out.append("No frame chain for this capture: the game closed before its window ended, "
                   "another capture overlapped it, or the build is older.\n")
        return
    out.append(f"- {chain_verdict(chain, threads)}")
    if not frames:
        out.append("")
        return
    window_ms = chain["window_ms"]
    frame_ms = window_ms / frames
    asked = (int(chain.get("swap_interval_start", 0)), int(chain.get("swap_interval_end", 0)))
    out.append(f"- Over {window_ms / 1e3:.1f} s: **{1000 / frame_ms:.1f} FPS** ({frame_ms:.1f} ms "
               f"per frame), GPU busy **{100 * chain.get('GpuBusyUs', 0) / 1e3 / window_ms:.0f}%**.")
    out.append(f"- The game asked for swap interval {asked[0]} at the start and {asked[1]} at the "
               f"end, changing it {chain.get('GuestSwapIntervalChanges', 0):.0f} times; "
               f"{chain.get('VsyncsLost', 0):.0f} vsyncs lost, "
               f"{chain.get('CompositesLate', 0):.0f} late composites.")
    out.append(f"- {chain.get('Draws', 0) / frames:.0f} draws and "
               f"{chain.get('Submits', 0) / frames:.1f} submits per frame, "
               f"{chain.get('PsoCreates', 0):.0f} PSOs created in the window.")
    per_frame = lambda name: chain.get(name, 0) / 1e3 / frames
    rows = [("GPU (hardware)", per_frame("GpuBusyUs"))] + chain_links(chain, threads, frames)
    out.append("\n## Work per frame\n")
    out.append(table(["link", "ms/frame", "% of frame"],
                     [[name, f"{value:.1f}", f"{100 * value / frame_ms:.0f}%"]
                      for name, value in sorted(rows, key=lambda row: -row[1])]))
    waits = [("GPU thread waited for the game's commands", "GpuThreadIdleUs"),
             ("GPU thread waited for the GPU (fences)", "FenceWaitUs"),
             ("GPU thread flushed guest readbacks", "GpuThreadFlushUs"),
             ("GPU thread waited for pipelines", "PipelineStallUs"),
             ("GPU thread created resources", "ResourceCreateUs"),
             ("GPU thread decoded textures", "TextureDecodeUs"),
             ("Vsync thread waited for the GPU thread", "VsyncComposeWaitUs"),
             ("Present waited: free frame", "PresentFreeWaitUs"),
             ("Present waited: render", "PresentRenderWaitUs"),
             ("Present waited: copy", "PresentCopyWaitUs"),
             ("Game waited for a free framebuffer", "GuestDequeueWaitUs"),
             ("Game waited for the GPU (syncpoints)", "GuestGpuWaitUs"),
             ("Emulated cores idle (all cores added)", "GuestCoreIdleUs")]
    out.append("\n## Waits per frame\n")
    out.append(table(["who", "ms/frame"],
                     [[label, f"{per_frame(name):.1f}"] for label, name in
                      sorted(waits, key=lambda wait: -per_frame(wait[1]))]))
    out.append("\nThe first 8 frames of the window carry the per-draw timers below, so they run a "
               "little slower than the rest. The waits overlap and do not add up to the frame.\n")


def describe_pass(p):
    size, samples, colors, color, depth = p
    text = f"{size} {DXGI.get(int(color[1:]), 'fmt' + color[1:])}"
    if int(colors[1:]) > 1:
        text += f" x{colors[1:]} RT"
    if int(depth[1:]):
        text += f" + {DXGI.get(int(depth[1:]), 'fmt' + depth[1:])}"
    if int(samples[1:]) > 1:
        text += f" MSAA{samples[1:]}"
    return text


def ms(ns):
    return ns / 1e6


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(out)


def shader(vs, ps):
    if int(vs, 16) == 0:
        return f"CS {ps}" if int(ps, 16) else "-"
    return f"VS {vs} / PS {ps}"


def verdict(c, frames, avg):
    perf, counters_ms = c.perf, c.meta.get("counters_ms", 0.0)
    if counters_ms <= 0:
        return ["No counter time: old capture?"], {}

    def share(name, scale):
        return perf.get(name, 0) / scale / counters_ms

    shares = {
        "gpu": share("GpuBusyUs", 1e3),
        "idle": share("GpuThreadIdleUs", 1e3),
        "fence": share("FenceWaitUs", 1e3),
        "flush": share("GpuThreadFlushUs", 1e3),
        "pipeline": share("PipelineStallUs", 1e3),
        "guest_wait": share("GuestGpuWaitUs", 1e3),
    }
    recorded = [r for r in c.records if r.outcome != EXPANDED]
    calls_ms = ms(sum(r.cpu_ns for r in recorded)) / frames
    lines = [f"- Frame: **{avg:.2f} ms** average ({1000 / avg:.1f} FPS) over {frames} frames; "
             f"worst {max(c.frames):.2f} ms."]
    lines.append(f"- GPU busy: **{shares['gpu'] * 100:.0f}%** of the time "
                 f"({shares['gpu'] * avg:.2f} ms per frame).")
    lines.append(f"- GPU thread: {calls_ms:.2f} ms per frame inside draws/clears/dispatches, "
                 f"{shares['idle'] * 100:.0f}% idle waiting for the guest, "
                 f"{shares['fence'] * 100:.0f}% waiting on the GPU fence, "
                 f"{shares['flush'] * 100:.0f}% in guest readback flushes, "
                 f"{shares['pipeline'] * 100:.0f}% waiting for pipelines.")
    # Steady frames on a multiple of the 60 Hz refresh: the game waits for vsync (or its own frame
    # cap), so the idle shares above are that wait, not a limit.
    refresh = 1000 / 60
    vsyncs = round(avg / refresh)
    locked = vsyncs >= 1 and abs(avg - vsyncs * refresh) < 0.3 and max(c.frames) - min(c.frames) < 1.0
    if locked and vsyncs == 1:
        cause = "**Nothing limits this capture**: every frame lands on the 60 Hz refresh, so the " \
                "waits above are vsync. Capture again during a slowdown; the tables below still " \
                "show what the frame costs."
    elif locked:
        cause = f"**Locked to {60 / vsyncs:.0f} FPS**: steady frames on {vsyncs} refreshes. If the " \
                "game targets that rate this is its cap, not a bottleneck; otherwise the shares " \
                "above say which side is slow."
    # Present and submit gaps keep a GPU-bound frame a little short of 100% busy; a GPU busy for
    # far longer than the GPU thread records calls is the limit even below that.
    elif shares["gpu"] >= 0.75 or shares["gpu"] * avg >= 2 * calls_ms:
        cause = "**GPU-bound**: the GPU is busy almost all the time. Cut GPU work (passes and " \
                "pipelines with most GPU ms below), not draw count alone."
    elif shares["idle"] >= 0.30:
        cause = "**Guest CPU-bound**: the GPU thread waits for the game's command lists. The " \
                "draws below are not the limit; look at the CPU profile (JIT, HLE services)."
    elif shares["fence"] + shares["flush"] >= 0.25:
        cause = "**Synchronization-bound**: the GPU thread blocks on the GPU (fence waits or " \
                "guest readbacks). Look for flushes and downloads, not draw cost."
    else:
        cause = "**GPU-thread CPU-bound**: recording calls fills the frame while the GPU has room. " \
                "Fewer or cheaper draws pay off directly: see the counts and CPU columns below."
    lines.append(f"- Verdict: {cause}")
    return lines, shares


def report(c, top, out):
    frames = max(1, len(c.frames))
    avg = sum(c.frames) / frames if c.frames else 0.0
    out.append(f"# Bottleneck hunt — capture {c.number}\n")
    if not c.records:
        out.append("No records in this capture.")
        return
    lines, shares = verdict(c, frames, avg)
    out.append("## Verdict\n")
    out += lines
    known = [r for r in c.records if r.outcome == RECORDED]
    timed = [r for r in known if r.gpu_ns is not None]
    gpu_total = sum(r.gpu_ns for r in timed)
    busy_ms = shares.get("gpu", 0.0) * avg
    coverage = f", {100 * ms(gpu_total) / frames / busy_ms:.0f}% of GPU busy" if busy_ms > 0 else ""
    out.append(f"- GPU time attributed to calls: {ms(gpu_total) / frames:.2f} ms per frame "
               f"({len(timed)} of {len(known)} recorded calls timed{coverage}).")
    tail = [r for r in timed if r.kind == LIST_END]
    if tail:
        out.append(f"- Command list tails (composite, resolves, copies after the last call): "
                   f"{ms(sum(r.gpu_ns for r in tail)) / frames:.2f} ms per frame.")
    if c.dropped:
        out.append(f"- **{c.dropped} records dropped** (capacity): the tail of the capture is "
                   "missing.")

    # Draw accounting.
    out.append("\n## Calls per frame\n")
    by_kind = collections.Counter()
    for r in c.records:
        by_kind[(r.kind, r.outcome)] += 1
    rows = []
    for kind in range(len(KINDS)):
        recorded = by_kind[(kind, RECORDED)]
        skipped = {OUTCOMES[o]: n for (k, o), n in by_kind.items()
                   if k == kind and o not in (RECORDED, EXPANDED)}
        expanded = by_kind[(kind, EXPANDED)]
        if not recorded and not skipped and not expanded:
            continue
        cpu = sum(r.cpu_ns for r in c.records if r.kind == kind and r.outcome != EXPANDED)
        gpu = sum(r.gpu_ns or 0 for r in known if r.kind == kind)
        skip_text = ", ".join(f"{k} {v / frames:.1f}" for k, v in
                              sorted(skipped.items(), key=lambda kv: -kv[1])) or "-"
        if expanded:
            skip_text += f"; {expanded / frames:.1f} expanded on CPU"
        rows.append([KINDS[kind], f"{recorded / frames:.1f}", skip_text,
                     f"{ms(cpu) / frames:.2f}", f"{ms(gpu) / frames:.2f}",
                     f"{cpu / max(1, recorded) / 1e3:.1f}"])
    out.append(table(["kind", "recorded", "skipped (per frame)", "CPU ms", "GPU ms",
                      "CPU µs/call"], rows))
    cpu_indirect = [r for r in c.records if r.flags & CPU_INDIRECT]
    emulated = [r for r in known if r.flags & EMULATED_TOPOLOGY]
    expanded = [r for r in c.records if r.outcome == EXPANDED]
    if expanded:
        out.append(f"\n- Indirect calls expanded on the CPU: {len(expanded) / frames:.1f} per "
                   f"frame became {len(cpu_indirect) / frames:.1f} draws "
                   f"({ms(sum(r.cpu_ns for r in expanded)) / frames:.2f} ms CPU per frame, "
                   "guest-memory reads included).")
    if emulated:
        out.append(f"- Emulated topologies (quads/loops/fans rebuilt on the CPU): "
                   f"{len(emulated) / frames:.1f} draws per frame, "
                   f"{ms(sum(r.cpu_ns for r in emulated)) / frames:.2f} ms CPU.")
    pipeline_wait = sum(r.pipeline_ns for r in c.records)
    out.append(f"- Pipeline lookup/wait inside calls: {ms(pipeline_wait) / frames:.2f} ms per frame.")
    tiny = [r for r in known if r.kind in (0, 1) and r.count * max(1, r.instances) <= 6]
    if tiny:
        out.append(f"- Tiny draws (≤ 6 vertices in total): {len(tiny) / frames:.1f} per frame, "
                   f"{ms(sum(r.cpu_ns for r in tiny)) / frames:.2f} ms CPU.")

    # GPU-thread stages from the counters.
    stages = [("DrawTexturesNs", "texture bindings"), ("DrawBuffersNs", "buffers"),
              ("DrawDescriptorsNs", "descriptors"), ("DrawTargetsNs", "render targets"),
              ("DrawSamplersNs", "samplers"), ("DrawRecordNs", "state + record"),
              ("ClearNs", "clears"), ("DispatchNs", "dispatches"),
              ("TextureDecodeUs", "texture decode"), ("ResourceCreateUs", "resource creation"),
              ("CacheInvalidationNs", "cache invalidation")]
    counters_ms = c.meta.get("counters_ms", 0.0)
    if counters_ms > 0 and avg > 0:
        rows = []
        for name, label in stages:
            value = c.perf.get(name, 0)
            value_ms = value / (1e3 if name.endswith("Us") else 1e6)
            per_frame = value_ms / counters_ms * avg
            if per_frame >= 0.01:
                rows.append([label, f"{per_frame:.2f}"])
        if rows:
            out.append("\n## Where the GPU thread's draw time goes (ms per frame)\n")
            out.append(table(["stage", "ms"], sorted(rows, key=lambda r: -float(r[1]))))
        per_frame = lambda name: c.perf.get(name, 0) / counters_ms * avg
        out.append(f"\nSubmits {per_frame('Submits'):.1f}/frame, PSO creations "
                   f"{per_frame('PsoCreates'):.1f}, texture uploads "
                   f"{per_frame('TextureUploads'):.1f} "
                   f"({per_frame('TextureUploadBytes') / 1048576:.2f} MiB), downloads "
                   f"{per_frame('TextureDownloads'):.1f}, guest readback flushes "
                   f"{per_frame('GpuThreadFlushes'):.1f}, resources created "
                   f"{per_frame('ResourcesCreated'):.1f}.")

    # Passes.
    group(out, "Render targets (passes)", known, frames, top,
          key=lambda r: r.pass_, label=lambda k: c.passes.get(k, "no target (dispatches, blits, DMA, list tails)"),
          extra=pass_entries(c.records))
    group(out, "Pipelines", known, frames, top,
          key=lambda r: (r.vs, r.ps, r.kind == DISPATCH_SETUP),
          label=lambda k: shader(k[0], k[1]) + (" setup" if k[2] else ""))
    macro_records = [r for r in c.records if r.outcome != EXPANDED]
    group(out, "Guest macros that issue the calls", macro_records, frames, top,
          key=lambda r: r.macro, label=lambda k: f"macro 0x{k:x}" if k else "no macro (direct)")

    # Batching.
    out.append("\n## Batching candidates\n")
    runs = collections.defaultdict(lambda: [0, 0, 0])  # draws in runs, runs, cpu ns
    previous, length, cpu = None, 0, 0

    def close():
        if previous is not None and length >= 4:
            entry = runs[previous]
            entry[0] += length
            entry[1] += 1
            entry[2] += cpu

    for r in known:
        key = (r.frame, r.pass_, r.vs, r.ps, r.kind)
        if r.kind in (0, 1) and key == previous:
            length += 1
            cpu += r.cpu_ns
            continue
        close()
        previous, length, cpu = (key if r.kind in (0, 1) else None), 1, r.cpu_ns
    close()
    merged = collections.defaultdict(lambda: [0, 0, 0])
    for (frame, pass_, vs, ps, kind), (draws, count, cpu_ns) in runs.items():
        entry = merged[(pass_, vs, ps)]
        entry[0] += draws
        entry[1] += count
        entry[2] += cpu_ns
    rows = sorted(merged.items(), key=lambda kv: -kv[1][0])[:top]
    if rows:
        out.append("Back-to-back draws with the same target and pipeline (runs of 4 or more): "
                   "instancing or merging them saves the per-draw CPU cost.\n")
        out.append(table(["pipeline", "target", "draws/frame", "runs/frame", "avg run",
                          "CPU ms/frame"],
                         [[shader(vs, ps), c.passes.get(pass_, "-"), f"{d / frames:.1f}",
                           f"{n / frames:.1f}", f"{d / max(1, n):.1f}", f"{ms(t) / frames:.2f}"]
                          for (pass_, vs, ps), (d, n, t) in rows]))
    else:
        out.append("No runs of 4 or more equal draws.")

    signatures = collections.Counter((r.frame, r.pass_, r.vs, r.ps, r.count, r.instances,
                                      r.topology) for r in known if r.kind in (0, 1))
    repeated = collections.Counter()
    for (frame, pass_, vs, ps, count, instances, topology), n in signatures.items():
        if n > 1:
            repeated[(pass_, vs, ps, count, instances, topology)] += n
    rows = repeated.most_common(top)
    if rows:
        out.append("\nRepeated draw signatures (same target, pipeline, count and instances in a "
                   "frame; the data may still differ):\n")
        out.append(table(["pipeline", "target", "vertices", "instances", "topology", "per frame"],
                         [[shader(vs, ps), c.passes.get(pass_, "-"), count, instances,
                           TOPOLOGIES[t] if t < len(TOPOLOGIES) else t, f"{n / frames:.1f}"]
                          for (pass_, vs, ps, count, instances, t), n in rows]))

    # Frames.
    out.append("\n## Frames\n")
    rows = []
    for f, interval in enumerate(c.frames):
        rs = [r for r in known if r.frame == f]
        rows.append([f, f"{interval:.2f}", len(rs), f"{ms(sum(r.cpu_ns for r in rs)):.2f}",
                     f"{ms(sum(r.gpu_ns or 0 for r in rs)):.2f}"])
    out.append(table(["frame", "interval ms", "recorded calls", "CPU ms", "GPU ms"], rows))


def pass_entries(records):
    """How many times per pass the stream switches into it (render pass changes)."""
    entries = collections.Counter()
    previous = None
    for r in records:
        if r.outcome != RECORDED or r.pass_ < 0:
            continue
        if r.pass_ != previous:
            entries[r.pass_] += 1
        previous = r.pass_
    return entries


def group(out, title, records, frames, top, key, label, extra=None):
    # calls, cpu, gpu, vertices, pipeline, draws (clears count pixels and dispatches groups, so
    # only direct draws feed the vertex average)
    stats = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0])
    for r in records:
        s = stats[key(r)]
        s[0] += 1
        s[1] += r.cpu_ns
        s[2] += r.gpu_ns or 0
        s[4] += r.pipeline_ns
        if r.kind in (0, 1):
            s[3] += r.count * max(1, r.instances)
            s[5] += 1
    if not stats:
        return
    total_gpu = sum(s[2] for s in stats.values()) or 1
    total_cpu = sum(s[1] for s in stats.values()) or 1
    header = ["", "calls/frame", "GPU ms", "GPU %", "CPU ms", "CPU %", "pipeline ms",
              "avg vertices"]
    if extra is not None:
        header.append("entered/frame")
    out.append(f"\n## {title}\n")
    for order, name in ((2, "by GPU time"), (1, "by GPU-thread CPU time"), (0, "by calls")):
        ranked = sorted(stats.items(), key=lambda kv: -kv[1][order])[:top]
        rows = []
        for k, s in ranked:
            row = [label(k), f"{s[0] / frames:.1f}", f"{ms(s[2]) / frames:.2f}",
                   f"{100 * s[2] / total_gpu:.0f}%", f"{ms(s[1]) / frames:.2f}",
                   f"{100 * s[1] / total_cpu:.0f}%", f"{ms(s[4]) / frames:.2f}",
                   f"{s[3] / s[5]:.0f}" if s[5] else "-"]
            if extra is not None:
                row.append(f"{extra.get(k, 0) / frames:.1f}")
            rows.append(row)
        out.append(f"\n### {name}\n")
        out.append(table(header, rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("hunt", help="jadyx_hunt.txt")
    parser.add_argument("--capture", type=int, help="capture number; the last one by default")
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--out", help="write the report here instead of printing it")
    parser.add_argument("--profile", help="CPU profile; jadyx_hunt_profile_N.txt next to it by default")
    parser.add_argument("--exe", default=os.path.join("build-uwp", "bin", "jadyx.exe"))
    parser.add_argument("--no-profile", action="store_true", help="skip the CPU profile")
    args = parser.parse_args()

    captures = parse(args.hunt)
    if not captures:
        sys.exit(f"{args.hunt}: no hunt captures")
    if args.capture is None:
        capture = captures[-1]
    else:
        matches = [c for c in captures if c.number == args.capture]
        if not matches:
            sys.exit(f"capture {args.capture} not found; there are "
                     f"{', '.join(str(c.number) for c in captures)}")
        capture = matches[0]
    profile = args.profile or os.path.join(os.path.dirname(os.path.abspath(args.hunt)),
                                           f"jadyx_hunt_profile_{capture.number}.txt")
    out = []
    threads = profile_threads(None if args.no_profile else profile)
    chain_section(capture, threads, out)
    if not args.no_profile:
        guest_section(profile, threads, args.top, out)
    report(capture, args.top, out)

    if not args.no_profile and os.path.exists(profile):
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profile-report.py")
        result = subprocess.run([sys.executable, script, profile, "--exe", args.exe,
                                 "--top", str(args.top)], capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        out.append("\n# CPU profile (every thread, during the capture)\n")
        if result.returncode == 0:
            out.append(result.stdout.replace("\n# ", "\n## "))
        else:
            out.append(f"profile-report.py failed:\n\n```\n{result.stderr.strip()}\n```")
    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"wrote {args.out}")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)


if __name__ == "__main__":
    main()
