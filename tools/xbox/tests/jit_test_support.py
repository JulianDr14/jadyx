# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile selected production functions with portable host adapters, without dependencies."""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BACKEND = ROOT / "src/dynarmic/src/dynarmic/backend/x64"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_cpp(source):
    with tempfile.TemporaryDirectory(prefix="jadyx-jit-") as directory:
        path = Path(directory)
        (path / "test.cpp").write_text(source)
        subprocess.run(
            [
                os.environ.get("CXX", "clang++"),
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-fsanitize=undefined",
                str(path / "test.cpp"),
                "-o",
                str(path / "test"),
            ],
            check=True,
        )
        subprocess.run([str(path / "test")], check=True)
