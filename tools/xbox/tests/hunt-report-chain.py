#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate for hunt-report.py's frame chain: each kind of slowdown gets its verdict, and a capture
reads its own chain and CPU profile.

    python tools/xbox/tests/hunt-report-chain.py
"""
import importlib.util
import os
import subprocess
import sys
import tempfile

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
REPORT = os.path.join(TOOLS, "hunt-report.py")

spec = importlib.util.spec_from_file_location("hunt_report", REPORT)
hunt_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hunt_report)


def chain(fps, gpu_ms, asked, idle_ms=0.0, seconds=5.0):
    """A window of `seconds` at `fps`, with the GPU and the GPU thread's idle time per frame."""
    frames = fps * seconds
    return {"window_ms": seconds * 1e3, "GuestFramesQueued": frames,
            "GpuBusyUs": gpu_ms * 1e3 * frames, "GpuThreadIdleUs": idle_ms * 1e3 * frames,
            "swap_interval_start": asked, "swap_interval_end": asked}


def cpu(frame_ms, **links_ms):
    """CPU ms per second of threads that take these ms per frame."""
    return {name: ms / frame_ms * 1e3 for name, ms in links_ms.items()}


CASES = [
    # Slower than asked, and the guest core fills the frame.
    ("guest core 2 sets the pace", chain(20, 11, 2, idle_ms=30), cpu(50, CPUCore_2=47, D3D12Recorder=8)),
    # The GPU thread's wall time fills the frame though the guest has room.
    ("GPU thread (recording draws) sets the pace", chain(20, 11, 2, idle_ms=2), cpu(50, CPUCore_2=20)),
    # The game dropped to 20 fps by itself because 30 did not fit.
    ("asked for 20 FPS", chain(20, 11, 3, idle_ms=15), cpu(50, CPUCore_2=34)),
    # The same interval with room to spare is the game's own cap.
    ("the game's own cap", chain(30, 8, 2, idle_ms=28), cpu(33.3, CPUCore_2=5)),
    ("Nothing limits", chain(60, 8, 1, idle_ms=10), cpu(16.67, CPUCore_2=6)),
    ("The GPU is the limit", chain(20, 45, 2, idle_ms=2), cpu(50, CPUCore_2=10)),
    # Slower than asked with no link near the frame: waits between links.
    ("No link fills the frame", chain(20, 11, 2, idle_ms=30), cpu(50, CPUCore_2=20)),
    ("No CPU profile", chain(20, 11, 2), {}),
]


def write_hunt(path):
    """Two captures with their chains, the second one written before the first's chain."""
    lines = []
    for number, fps in ((1, 20), (2, 30)):
        lines += [f"hunt 1 capture {number} frames 1 records 1 dropped 0 timestamps 0",
                  "frame 0 33.3", "meta frequency 0 counters_ms 40.000", "perf GpuBusyUs 8000",
                  "r 1 0 0 0 -1 00 00 3 1 4 1000 0 - 0"]
    window = chain(30, 7.5, 2, idle_ms=2)
    lines += [f"chain 2 {key} {value}" for key, value in window.items()]
    window = chain(20, 11, 2, idle_ms=2)
    lines += [f"chain 1 {key} {value}" for key, value in window.items()]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def write_profile(path, thread, cpu_ms):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# jadyx sampling profile v1\n"
                "window 1 seconds 5.000 hz 500 ticks 2500\n"
                f"thread {thread} id 1 cpu_ms {cpu_ms} running 2400 waiting 100\n")


def guest_code_failures(folder):
    """A spinning block is told from a working one, and samples go to the busiest core's blocks."""
    spin = bytes.fromhex("08fc5f88" "e8ffff35")  # ldaxr w8, [x0]; cbnz w8, <itself>
    work = bytes.fromhex("2000028b" "200000f9" "c0035fd6")  # add x0, x1, x2; str x0, [x1]; ret
    path = os.path.join(folder, "guest.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write("window 1 seconds 5.000 hz 500 ticks 2500\n"
                "thread CPUCore_0 id 1 cpu_ms 4800 running 2400 waiting 0\n"
                "thread CPUCore_1 id 2 cpu_ms 100 running 100 waiting 0\n"
                "guest CPUCore_0 0x80001000 0x80001008 800 0\n"
                "guest CPUCore_0 0x80002000 0x8000200c 150 50\n"
                "guest CPUCore_0 0x0 0x0 100 0\n"
                "guest CPUCore_1 0x80002000 0x8000200c 90 0\n"
                "guestthunk CPUCore_0 0x80002000 0x8000200c 100\n"
                "guestthunk CPUCore_0 0x80003000 0x80003010 300\n"
                "guestthunk CPUCore_1 0x80002000 0x8000200c 50\n"
                "guestbase 0x80000000\n"
                f"guestcode 0x80001000 {spin.hex()}\nguestcode 0x80002000 {work.hex()}\n")
    out = []
    hunt_report.guest_section(path, {"CPUCore_0": 960.0, "CPUCore_1": 20.0}, 15, out)
    text = "\n".join(out)
    failures = []
    for expected in ("(CPUCore_0)", "1500 samples", "**67% in the game's blocks, 27% in memory access "
                     "thunks, 7% in the rest of the JIT**", "| 53.3% | 0x80001000 | +0x1000 | 0% |",
                     "| 20.0% | 0x80003000 | +0x3000 | 100% |", "wait?, loop, exclusive",
                     "ldaxr w8, [x0]"):
        if expected not in text:
            failures.append(f"guest section lacks '{expected}':\n{text}")
    work_info = hunt_report.describe_block(0x80002000, work)
    if work_info["waiting"] or work_info["stores"] != 1:
        failures.append(f"a working block was taken for a wait: {work_info}")
    return failures


def main():
    failed = 0
    for expected, window, threads in CASES:
        verdict = hunt_report.chain_verdict(window, threads)
        if expected not in verdict:
            print(f"FAIL: expected '{expected}' in: {verdict}")
            failed += 1
    with tempfile.TemporaryDirectory() as folder:
        if hunt_report.capstone is None:
            print("guest code checks skipped: capstone is not installed")
        else:
            for failure in guest_code_failures(folder):
                print(f"FAIL: {failure}")
                failed += 1
        hunt = os.path.join(folder, "jadyx_hunt.txt")
        write_hunt(hunt)
        write_profile(os.path.join(folder, "jadyx_hunt_profile_1.txt"), "CPUCore_1", 4800)
        write_profile(os.path.join(folder, "jadyx_hunt_profile_2.txt"), "CPUCore_2", 1000)
        for number, expected in ((1, "| guest core 1 |"), (2, "| guest core 2 |")):
            result = subprocess.run([sys.executable, REPORT, hunt, "--capture", str(number),
                                     "--exe", os.path.join(folder, "none.exe")],
                                    capture_output=True, text=True, encoding="utf-8")
            if result.returncode != 0 or expected not in result.stdout or \
                    f"Why the GPU sits idle — capture {number}" not in result.stdout:
                print(f"FAIL: capture {number} lacks '{expected}':\n{result.stdout}{result.stderr}")
                failed += 1
    print("hunt-report-chain: FAILED" if failed else "hunt-report-chain: OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
