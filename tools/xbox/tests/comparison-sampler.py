# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production Sampler and table builder with a recording D3D12 host shim.

No Windows SDK or GPU required. The shim records descriptors; it does not emulate sampling.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
RENDERER = ROOT / "src/video_core/renderer_d3d12"


def block(path, start, end):
    text = path.read_text()
    return text[text.index(start) : text.index(end, text.index(start))]


def main():
    sampler_class = block(
        RENDERER / "d3d12_texture_cache.h", "class Sampler {", "class Framebuffer {"
    )
    sampler_source = (RENDERER / "d3d12_sampler.cpp").read_text()
    sampler_source = sampler_source[sampler_source.index("namespace D3D12 {") :]
    table = block(
        RENDERER / "d3d12_pipeline_helper.h",
        "class SamplerTableBuilder {",
        "/// Push constants",
    )
    fixture = (Path(__file__).with_suffix(".cpp")).read_text()
    source = fixture.replace("// PRODUCTION_SAMPLER_CLASS", sampler_class)
    source = source.replace("// PRODUCTION_SAMPLER_SOURCE", sampler_source)
    source = source.replace("// PRODUCTION_SAMPLER_TABLE", table)
    with tempfile.TemporaryDirectory(prefix="jadyx-comparison-sampler-") as directory:
        cpp = Path(directory) / "gate.cpp"
        exe = Path(directory) / "gate"
        cpp.write_text(source)
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Wshadow",
                "-Wconversion",
                "-Werror",
                str(cpp),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    # The executable above tests the shared table logic. Check both real callers use it.
    for filename in ("d3d12_graphics_pipeline.cpp", "d3d12_compute_pipeline.cpp"):
        text = (RENDERER / filename).read_text()
        assert (
            "sampler_table.AddStage(info, texture_cache, table_samplers_it);" in text
        ), filename
        assert "for (const SamplerId id : samplers)" not in text, filename
    clear = (RENDERER / "d3d12_rasterizer_clear.cpp").read_text()
    assert "{sampler.Handle(false), sampler.Key(false)}" in clear
    print("PASS graphics/compute per-instruction tables and plain DrawTexture wiring")


if __name__ == "__main__":
    main()
