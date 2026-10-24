# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline macOS/Linux host gate for translation filters and existing mod logic."""

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LOCALES = [
    "src/jadyx/i18n/localization.cpp",
    *[
        f"src/jadyx/i18n/locales/{name}.cpp"
        for name in ("en", "es", "pt_br", "registry")
    ],
]
SUITES = {
    "mod-language-filter": [],
    "mods-page": [],
    "localization-catalogs": LOCALES,
    "mod-analysis": ["src/jadyx/mods/mod_inspect.cpp"],
    "mod-layout": ["src/jadyx/mods/mod_layout.cpp"],
    "mod-priority": [],
    "archive-volumes": [],
}


def main():
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    with tempfile.TemporaryDirectory(prefix="jadyx-mod-language-") as output:
        for name, sources in SUITES.items():
            binary = str(Path(output) / name)
            flags = ["-std=c++20", "-DJADYX_NO_WINRT", "-Isrc"]
            if name == "mod-language-filter":
                flags += [
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Wconversion",
                    "-Werror",
                    "-Wno-missing-field-initializers",
                ]
            subprocess.run(
                [
                    *compiler,
                    *flags,
                    f"tools/xbox/tests/{name}.cpp",
                    *sources,
                    "-o",
                    binary,
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([binary], cwd=ROOT, check=True)
    subprocess.run(
        [sys.executable, "-B", "tools/xbox/tests/integration-locales.py"],
        cwd=ROOT,
        check=True,
    )
    print("PASS: 7 C++ harnesses and locale source-order test")


if __name__ == "__main__":
    main()
