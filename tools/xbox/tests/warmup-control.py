# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable startup lifetime gates. Run with python3 -B tools/xbox/tests/warmup-control.py."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WarmupTests(unittest.TestCase):
    def test_control_and_join(self):
        self.build_and_run("warmup-control")

    def test_shader_worker_drain_and_reuse(self):
        self.build_and_run("warmup-shader-workers")

    def build_and_run(self, name):
        with tempfile.TemporaryDirectory(prefix="jadyx-warmup-") as directory:
            binary = str(Path(directory) / name)
            subprocess.run(
                [
                    os.environ.get("CXX", "clang++"),
                    "-std=c++20",
                    "-pthread",
                    "-Isrc",
                    f"tools/xbox/tests/{name}.cpp",
                    "-o",
                    binary,
                ],
                cwd=ROOT,
                check=True,
                timeout=60,
            )
            subprocess.run([binary], check=True, timeout=10)

    def test_shader_callbacks_drain_before_scope_exit(self):
        source = (
            ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp"
        ).read_text()
        load = source.split("void PipelineCache::LoadDiskResources(", 1)[1].split(
            "GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath", 1
        )[0]
        self.assertNotIn("workers.WaitForRequests(stop_loading)", load)
        self.assertRegex(load, r"SCOPE_EXIT\s*\{\s*workers\.WaitForRequests\(\);\s*\}")
        jobs = load.split("VideoCommon::LoadPipelines", 1)[0]
        self.assertEqual(jobs.count("if (stop_loading.stop_requested())"), 2)
        self.assertIn("workers.WaitForRequests();", load)

    def test_boot_lifetime_and_opt_in(self):
        source = (ROOT / "src/jadyx/app/uwp_boot.cpp").read_text()
        load = source.split("warmup_screen.emplace(system, warmup_control)", 1)[1]
        end = load.index("void(system.Run())")
        startup = load[:end]
        self.assertIn("warmup_control.Token()", startup)
        self.assertLess(
            startup.index("ConfigureApplicationPrewarm"),
            startup.index("warmup_screen.reset()"),
        )
        self.assertLess(
            startup.index("warmup_screen.reset()"),
            startup.index("return RETURN_TO_LIBRARY"),
        )
        self.assertIn(
            'WarmupControlEnabled(std::getenv("NXBOX_WARMUP_CONTROL"))', source
        )
        self.assertIn('line == "NXBOX_WARMUP_CONTROL=1"', source)
        self.assertIn("bool warmup_control{}", source)
        self.assertIn("!resumed && config.play", source)

    def test_jit_checks_stop_between_blocks_and_restores_observer(self):
        source = (ROOT / "src/core/arm/dynarmic/jit_prewarm_owner.cpp").read_text()
        warm = source.split("void Owner::Warm(", 1)[1].split("void Owner::Save()", 1)[0]
        self.assertLess(
            warm.index("stop.stop_requested()"), warm.index("target->PrecompileBlock")
        )
        self.assertIn("stop.stop_requested() ? processed : plan.size()", warm)
        self.assertGreater(warm.index("AttachObserver()"), warm.index("catch ("))


if __name__ == "__main__":
    unittest.main(verbosity=2)
