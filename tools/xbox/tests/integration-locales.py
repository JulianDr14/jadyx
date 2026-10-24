# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that merged locale entries follow the complete Text enum in source order."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class IntegrationLocaleTests(unittest.TestCase):
    def test_catalog_order(self):
        header = (ROOT / "src/jadyx/i18n/localization.h").read_text()
        enum = header.split("enum class Text : std::uint16_t {", 1)[1].split("};", 1)[0]
        expected = re.findall(r"^\s*(\w+),\s*$", enum, re.MULTILINE)
        self.assertTrue(expected)
        for path in sorted((ROOT / "src/jadyx/i18n/locales").glob("*.cpp")):
            source = path.read_text()
            if "MakeCatalog({" not in source:
                continue
            with self.subTest(locale=path.stem):
                actual = re.findall(r"\{Text::(\w+),", source)
                self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
