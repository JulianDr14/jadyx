# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile production methods with synthetic service/OS boundaries; no network or guest assets."""

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run(name):
    driver = (HERE / f"{name}.cpp").read_text()
    if name == "page-writers":
        header = (ROOT / "src/common/page_table.h").read_text()
        header = "\n".join(
            line
            for line in header.splitlines()
            if not line.startswith(("#include", "#pragma"))
        )
        source = (ROOT / "src/core/memory.cpp").read_text()
        methods = "\n".join(
            method(source, signature)
            for signature in (
                "void MarkRegionDebug(",
                "void RasterizerMarkRegionCached(",
                "void MapPages(",
            )
        )
        driver = driver.replace("// PAGE_TABLE", header).replace("// WRITERS", methods)
    elif name == "bsd-read":
        source = (ROOT / "src/core/hle/service/sockets/bsd.cpp").read_text()
        driver = driver.replace(
            "// HANDLERS",
            "\n".join(
                method(source, signature)
                for signature in (
                    "void BSD_USA::Read(",
                    "void BSD_USA::RecvWork::Execute(",
                    "void BSD_USA::RecvWork::Response(",
                    "std::pair<s32, Errno> BSD_USA::RecvImpl(",
                )
            ),
        )
    elif name == "storage-read":
        source = (
            ROOT / "src/core/hle/service/filesystem/fsp/fs_i_storage.cpp"
        ).read_text()
        driver = driver.replace("// HANDLER", method(source, "Result IStorage::Read("))
    with tempfile.TemporaryDirectory(prefix=f"jadyx-{name}-") as temporary:
        cpp = Path(temporary) / "test.cpp"
        binary = Path(temporary) / "test"
        cpp.write_text(driver)
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-pthread",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wshadow",
                "-Wconversion",
                "-Wno-sign-conversion",
                "-Isrc",
                str(cpp),
                "-o",
                str(binary),
            ],
            cwd=ROOT,
            check=True,
        )
        subprocess.run([str(binary)], check=True, timeout=30)
    print(f"PASS {name}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "tests",
        nargs="*",
        default=["page-writers", "scratch-buffer", "bsd-read", "storage-read"],
    )
    for test in parser.parse_args().tests:
        run(test)
