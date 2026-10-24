# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline macOS/Linux gates: python3 -B tools/xbox/tests/usb-play-in-place.py."""

import os
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class UsbPlayInPlaceTests(unittest.TestCase):
    def compile_and_run(self, name, extra=()):
        with tempfile.TemporaryDirectory(prefix="jadyx-usb-host-") as directory:
            binary = Path(directory) / name
            subprocess.run(
                [
                    os.environ.get("CXX", "clang++"),
                    "-std=c++20",
                    "-DJADYX_NO_WINRT",
                    "-I",
                    "src",
                    f"tools/xbox/tests/{name}.cpp",
                    *extra,
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(binary)], cwd=ROOT, check=True)

    def test_storage_paths(self):
        self.compile_and_run("storage-path")

    def test_library_scan(self):
        self.compile_and_run("game-library")

    def test_localization(self):
        self.compile_and_run(
            "localization-catalogs",
            ["src/jadyx/i18n/localization.cpp"]
            + sorted(
                str(path.relative_to(ROOT))
                for path in (ROOT / "src/jadyx/i18n/locales").glob("*.cpp")
            ),
        )

    def test_discovery_policy(self):
        self.compile_and_run("usb-discovery")

    def test_discovery_vfs_integration(self):
        source = (ROOT / "src/jadyx/storage/uwp_rom_storage.cpp").read_text()
        scan = source.split("LibraryScan ScanExternalGameFolders", 1)[1].split(
            "void SetUsbPlayInPlace", 1
        )[0]
        self.assertNotIn("RememberGameFolder(", scan)
        self.assertNotIn("AddOrReplace(", scan)
        self.assertIn("SessionGameFolder(drive)", scan)
        self.assertIn("source.drive_root = true", scan)
        self.assertIn("SourceFolder(path.token)", source)
        # OpenFile must accept session tokens as well as FutureAccessList tokens.
        self.assertNotIn("ContainsItem(", source)

    def test_manifest_usb_access(self):
        manifest = ET.parse(ROOT / "dist/uwp/AppxManifest.xml").getroot()
        uap = "{http://schemas.microsoft.com/appx/manifest/uap/windows10}"
        self.assertIsNotNone(
            manifest.find(f'.//{uap}Capability[@Name="removableStorage"]')
        )
        association = manifest.find(f'.//{uap}FileTypeAssociation[@Name="jadyxgames"]')
        self.assertIsNotNone(association)
        self.assertEqual(
            {item.text for item in association.iter(uap + "FileType")},
            {".nsp", ".xci", ".nro"},
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
