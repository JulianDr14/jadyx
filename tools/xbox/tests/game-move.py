#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standalone move policy/data/UI gates; no WinRT, game files or network.

Uses the existing compiler and OpenSSL headers/library via pkg-config.
"""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
crypto_flags = shlex.split(
    subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libcrypto"], text=True
    )
)
with tempfile.TemporaryDirectory(prefix="jadyx-game-move-") as temporary:
    for name in ("game-move", "game-move-data", "game-move-options"):
        binary = str(Path(temporary) / name)
        command = [
            os.environ.get("CXX", "clang++"),
            "-std=c++20",
            "-Wall",
            "-Wextra",
            "-Wshadow",
            "-Wconversion",
            "-Wno-sign-conversion",  # Existing storage_path.h range-for conversion.
            "-Werror",
            "-DJADYX_NO_WINRT",
            "-I",
            "src",
            f"tools/xbox/tests/{name}.cpp",
            "-o",
            binary,
        ]
        if name == "game-move-data":
            command += crypto_flags
        subprocess.run(command, cwd=ROOT, check=True)
        environment = dict(os.environ)
        environment.pop("NXBOX_MOVE_TO_USB", None)
        subprocess.run([binary], cwd=ROOT, env=environment, check=True)
