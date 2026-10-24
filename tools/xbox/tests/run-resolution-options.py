# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run portable resolution, FSR and settings tests without a UWP SDK or downloads."""

from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[3]
    catalogs = ["src/jadyx/i18n/localization.cpp"] + [
        str(path.relative_to(root))
        for path in sorted((root / "src/jadyx/i18n/locales").glob("*.cpp"))
    ]
    failures = 0
    with tempfile.TemporaryDirectory(prefix="jadyx-resolution-") as directory:
        for name in (
            "resolution-options",
            "upscaling",
            "localization-catalogs",
            "fsr-plan",
            "game-menu",
            "system-settings",
        ):
            binary = str(Path(directory) / name)
            extra = (
                ["-Iexternals/FidelityFX-FSR/ffx-fsr", "src/video_core/fsr.cpp"]
                if name == "fsr-plan"
                else catalogs
            )
            command = [
                "clang++",
                "-std=c++20",
                "-Isrc",
                f"tools/xbox/tests/{name}.cpp",
                *extra,
                "-o",
                binary,
            ]
            built = subprocess.run(command, cwd=root, capture_output=True, text=True)
            result = (
                subprocess.run([binary], cwd=root, capture_output=True, text=True)
                if built.returncode == 0
                else built
            )
            print(f"{name}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
            print(result.stdout + result.stderr, end="", flush=True)
            failures += result.returncode != 0
    print(f"6 tests, {failures} failures")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
