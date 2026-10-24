# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run JIT host regressions and the unchanged portable trace baselines offline."""

import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
scripts = (
    "jit-exception-home.py",
    "jit-exception-filter.py",
    "jit-rel32.py",
    "jit-sm4.py",
    "jit-round-thunk.py",
    "jit-unsigned-saturation.py",
    "jit-round-exceptions.py",
    "jit-macro-immediate.py",
    "pipeline-build-trace.py",
    "address-wait-trace.py",
    "gc-trace.py",
)
for script in scripts:
    print(f"Running {script}", flush=True)
    subprocess.run([sys.executable, "-B", str(root / script)], check=True)
print("PASS: 8 JIT regressions and 3 existing Jadyx baselines")
