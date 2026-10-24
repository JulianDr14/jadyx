# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the native constructor uses the portable gates before allocation."""

from pathlib import Path
import unittest

SOURCE = (
    Path(__file__).resolve().parents[3]
    / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp"
).read_text()
CONSTRUCTOR = SOURCE.split("Image::Image(TextureCacheRuntime&", 1)[1].split(
    "Image::Image(const VideoCommon::NullImageParams", 1
)[0]


class MsaaWiring(unittest.TestCase):
    def test_shape_gate_precedes_sample_grid(self):
        self.assertIn("ValidateMsaaShape(", CONSTRUCTOR)
        self.assertLess(
            CONSTRUCTOR.index("ValidateMsaaShape("),
            CONSTRUCTOR.index("VideoCommon::SamplesLog2("),
        )

    def test_exact_typed_capability_precedes_allocator(self):
        self.assertIn("ValidateMsaaAllocation(", CONSTRUCTOR)
        self.assertLess(
            CONSTRUCTOR.index("ValidateMsaaAllocation("),
            CONSTRUCTOR.index("texture_allocator.Create("),
        )
        self.assertIn(".Format = is_color ? format.view : format.dsv", CONSTRUCTOR)
        self.assertIn(".SampleCount = requested_samples", CONSTRUCTOR)
        self.assertIn("D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS", CONSTRUCTOR)
        self.assertIn(".Count = info_.num_samples, .Quality = 0", CONSTRUCTOR)


if __name__ == "__main__":
    unittest.main()
