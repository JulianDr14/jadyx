#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the portable frontend gates with the host C++20 compiler, without WinRT."""

import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

TESTS = (
    "files-browser",
    "frontend-input",
    "controller-selection",
    "controller-preference",
    "stick-motion",
    "player-slots",
    "stick-navigation",
    "game-menu",
    "game-keyboard",
    "game-language",
    "system-settings",
    "cheats-page",
    "mods-page",
    "mod-language-filter",
    "text-entry",
    "localization-catalogs",
    "play-history",
    "game-information",
    "themes",
    "theme-settings",
    "themes-integration",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tests", nargs="*", choices=(*TESTS, "all"), default=["all"])
    args = parser.parse_args()
    tests = TESTS if "all" in args.tests else args.tests
    root = Path(__file__).resolve().parents[3]
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    catalogs = sorted((root / "src/jadyx/i18n/locales").glob("*.cpp"))
    failures = []
    with tempfile.TemporaryDirectory(prefix="jadyx-frontend-") as output:
        for name in tests:
            executable = Path(output) / name
            sources = [f"tools/xbox/tests/{name}.cpp"]
            if name == "cheats-page":
                sources.append("src/core/memory/cheat_text.cpp")
            command = [
                *compiler,
                "-std=c++20",
                "-DJADYX_NO_WINRT",
                "-Isrc",
                *sources,
                "src/jadyx/i18n/localization.cpp",
                *map(str, catalogs),
                "-o",
                str(executable),
            ]
            built = subprocess.run(
                command, cwd=root, capture_output=True, text=True, check=False
            )
            result = (
                subprocess.run(
                    [str(executable)],
                    cwd=root,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if built.returncode == 0
                else built
            )
            if result.returncode:
                failures.append(name)
                print(f"FAIL {name} (exit {result.returncode})", flush=True)
                print(result.stdout + result.stderr, end="", flush=True)
            else:
                print(f"PASS {name}", flush=True)
    print(f"{len(tests) - len(failures)} passed, {len(failures)} failed", flush=True)
    if failures:
        print("Failed: " + ", ".join(failures))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
