#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-only launch layout regressions; no Windows tools or runtime required."""

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "package_launch", TOOLS / "check-package-launch.py"
)
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


def pe(imports=(), delay=()):
    """Small PE32+ fixture with one section and RVA-based import descriptors."""
    data = bytearray(4096)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<HH", data, 132, 0x8664, 1)
    struct.pack_into("<H", data, 148, 240)
    struct.pack_into("<H", data, 152, 0x20B)
    struct.pack_into("<I", data, 152 + 108, 16)
    struct.pack_into("<8sIIII", data, 392, b".rdata", 3584, 4096, 3584, 512)
    cursor = 512
    for index, names, width in [(1, imports, 20), (13, delay, 32)]:
        if not names:
            continue
        struct.pack_into(
            "<II", data, 152 + 112 + index * 8, cursor + 3584, (len(names) + 1) * width
        )
        strings = cursor + (len(names) + 1) * width
        for name in names:
            encoded = name.encode() + b"\0"
            data[strings : strings + len(encoded)] = encoded
            if index == 13:
                struct.pack_into("<II", data, cursor, 1, strings + 3584)
            else:
                struct.pack_into("<I", data, cursor + 12, strings + 3584)
            cursor += width
            strings += len(encoded)
        cursor = strings + 64
    return bytes(data)


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write(
            "AppxManifest.xml",
            (TOOLS.parents[1] / "dist/uwp/AppxManifest.xml").read_bytes(),
        )
        self.write("jadyx.exe", pe())
        self.write("resources.pri", b"fixture")
        self.write("boot.cfg", b"library=1\nplay=1\n")
        self.write("dxil.dll", pe())
        self.write("spirv_to_dxil.dll", pe())
        for name in [
            "StoreLogo",
            "Square150x150Logo",
            "Square44x44Logo",
            "Wide310x150Logo",
            "SplashScreen",
        ]:
            self.write(f"Assets/{name}.scale-200.png", b"fixture")

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def check(self):
        return launch.check(self.root, release=True)

    def test_complete_layout(self):
        self.assertEqual(self.check(), [])

    def test_missing_executable(self):
        (self.root / "jadyx.exe").unlink()
        self.assertIn("jadyx.exe", "\n".join(self.check()))

    def test_missing_pri(self):
        (self.root / "resources.pri").unlink()
        self.assertIn("resources.pri", "\n".join(self.check()))

    def test_missing_dynamic_shader_dll(self):
        (self.root / "dxil.dll").unlink()
        self.assertIn("dxil.dll", "\n".join(self.check()))

    def test_missing_hard_import(self):
        self.write("jadyx.exe", pe(["forgotten.dll"]))
        self.assertIn("jadyx.exe -> forgotten.dll", "\n".join(self.check()))

    def test_transitive_import(self):
        self.write("spirv_to_dxil.dll", pe(["forgotten.dll"]))
        self.assertIn("spirv_to_dxil.dll -> forgotten.dll", "\n".join(self.check()))

    def test_missing_delay_import(self):
        self.write("jadyx.exe", pe(delay=["forgotten.dll"]))
        self.assertIn("delay", "\n".join(self.check()))

    def test_reviewed_desktop_delay_only(self):
        self.write("jadyx.exe", pe(delay=["SETUPAPI.dll"]))
        self.assertEqual(self.check(), [])
        self.write("jadyx.exe", pe(["SETUPAPI.dll"]))
        self.assertIn("setupapi.dll", "\n".join(self.check()))

    def test_unknown_api_set_is_not_implicitly_trusted(self):
        self.write("jadyx.exe", pe(["api-ms-win-new-unsupported-l1-1-0.dll"]))
        self.assertTrue(self.check())

    def test_packaged_dependency_and_cycle(self):
        self.write("jadyx.exe", pe(["helper.dll"]))
        self.write("helper.dll", pe(["jadyx.exe"]))
        self.assertEqual(self.check(), [])

    def test_framework_required(self):
        self.write("jadyx.exe", pe(["VCRUNTIME140_APP.dll"]))
        self.assertIn("framework", "\n".join(self.check()))

    def test_missing_manifest_asset(self):
        (self.root / "Assets/SplashScreen.scale-200.png").unlink()
        self.assertIn("SplashScreen.png", "\n".join(self.check()))

    def test_malformed_pe_fails_closed(self):
        self.write("jadyx.exe", b"MZ")
        self.assertIn("invalid PE", "\n".join(self.check()))

    def test_framework_identity_and_dependency(self):
        framework = self.root / "framework"
        framework.mkdir()
        text = (
            '<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">'
            '<Identity Name="Microsoft.VCLibs.140.00" Version="14.0.33519.0" '
            'ProcessorArchitecture="x64" Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, '
            'L=Redmond, S=Washington, C=US" /></Package>'
        )
        (framework / "AppxManifest.xml").write_text(text)
        (framework / "VCRUNTIME140_APP.dll").write_bytes(pe(["kernel32.dll"]))
        self.write("jadyx.exe", pe(["VCRUNTIME140_APP.dll"]))
        self.assertEqual(launch.check(self.root, framework, release=True), [])
        (framework / "AppxManifest.xml").write_text(
            text.replace("14.0.33519.0", "14.0.1.0")
        )
        self.assertIn(
            "identity/version",
            "\n".join(launch.check(self.root, framework, release=True)),
        )

    def test_archive_input(self):
        import zipfile

        archive = self.root / "fixture.appx"
        with zipfile.ZipFile(archive, "w") as output:
            for path in self.root.rglob("*"):
                if path.is_file() and path != archive:
                    output.write(path, path.relative_to(self.root))
        self.assertEqual(launch.check(archive, release=True), [])

    def test_packager_checks_before_packing(self):
        script = (TOOLS / "package-appx.ps1").read_text()
        check = script.index("& python @launchArgs")
        self.assertLess(check, script.index("& $makeappx @packArgs"))
        self.assertIn(
            'if ($LASTEXITCODE -ne 0) { throw "Launch dependency check failed',
            script[check:],
        )

    def test_import_tables(self):
        self.assertEqual(
            launch.pe_imports(pe(["a.dll"], ["b.dll"])),
            [("a.dll", False), ("b.dll", True)],
        )


if __name__ == "__main__":
    unittest.main()
