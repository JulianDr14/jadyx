# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native macOS gate using the real virtual gamepad, poller, converter and HID filter."""

import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
sources = [
    "tools/xbox/tests/motion-delivery.cpp",
    "src/input_common/drivers/virtual_gamepad.cpp",
    "src/input_common/input_engine.cpp",
    "src/input_common/input_poller.cpp",
    "src/hid_core/frontend/input_converter.cpp",
    "src/hid_core/frontend/motion_input.cpp",
    "src/common/param_package.cpp",
    "src/common/string_util.cpp",
    "src/common/uuid.cpp",
    "src/common/random.cpp",
]
with tempfile.TemporaryDirectory(prefix="jadyx-motion-") as temporary:
    binary = Path(temporary) / "motion-delivery"
    subprocess.run(
        [
            "clang++",
            "-std=c++20",
            "-DFMT_HEADER_ONLY",
            "-include",
            "array",
            "-include",
            "algorithm",
            "-include",
            "cstddef",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-Wno-deprecated-declarations",
            "-Isrc",
            "-I/opt/homebrew/include",
            *sources,
            "-o",
            str(binary),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=10)
