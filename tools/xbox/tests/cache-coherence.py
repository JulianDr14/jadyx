# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile production coherence paths with synthetic guest memory and caches."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TESTS = Path(__file__).resolve().parent


def function_body(path, signature):
    source = (ROOT / path).read_text()
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


def compile_run(fixture, replacements):
    source = (TESTS / fixture).read_text()
    for marker, body in replacements.items():
        source = source.replace(marker, body)
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / "test.cpp").write_text(source)
        subprocess.run(
            [
                *shlex.split(os.environ.get("CXX", "c++")),
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Wshadow",
                "-Wconversion",
                "-Werror",
                "-fsanitize=address,undefined",
                str(path / "test.cpp"),
                "-o",
                str(path / "test"),
            ],
            check=True,
        )
        subprocess.run([str(path / "test")], check=True, cwd=directory)


class CacheCoherenceTests(unittest.TestCase):
    def test_dma_constant_fill(self):
        source = (ROOT / "src/video_core/engines/maxwell_dma.cpp").read_text()
        start = source.index("            const u32 component_size =")
        end = source.index("\n        } else {", start)
        compile_run("dma-clear.cpp", {"// PRODUCTION_FILL": source[start:end]})

    def test_inline_write_drain(self):
        memory = "src/video_core/memory_manager.cpp"
        compile_run(
            "inline-invalidation.cpp",
            {
                "// PRODUCTION_WRITE": function_body(
                    memory, "void MemoryManager::WriteBlockCached"
                ),
                "// PRODUCTION_FLUSH": function_body(
                    memory, "void MemoryManager::FlushCaching"
                ),
                "// PRODUCTION_INVALIDATION": function_body(
                    "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp",
                    "void RasterizerD3D12::InnerInvalidation",
                ),
            },
        )


if __name__ == "__main__":
    unittest.main()
