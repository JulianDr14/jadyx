# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard the Direct2D badge bridge that portable token tests cannot execute."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class ThemeIntegrationTests(unittest.TestCase):
    def test_badges_resolve_theme_ink(self):
        source = (ROOT / "src/jadyx/library/uwp_library_canvas.cpp").read_text()
        badge = source.split("void DrawCompatibility(", 1)[1].split(
            "/// The focused game", 1
        )[0]
        self.assertIn("brush->SetColor(Ink(color, alpha));", badge)
        self.assertNotIn("D2D1::ColorF", badge)
        for token in (
            "CompatibilityPlayable",
            "CompatibilityBroken",
            "Warning",
            "TextSecondary",
        ):
            self.assertIn(f"Palette::{token}", badge)


if __name__ == "__main__":
    unittest.main(verbosity=2)
