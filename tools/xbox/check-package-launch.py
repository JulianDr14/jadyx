#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate a staged UWP layout or APPX without executing Windows binaries.

Only explicitly reviewed OS DLLs may be absent. CRT DLLs must be in the declared,
provided VCLibs framework; arbitrary API-set names are not automatically trusted.
This checks dependency presence, not Xbox API/export availability or PRI semantics.
"""

import argparse
import json
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path, PurePosixPath

POLICY = Path(__file__).with_name("launch-dependencies.json")
FOUNDATION = "{http://schemas.microsoft.com/appx/manifest/foundation/windows10}"


def pe_imports(data):
    """Return (DLL name, delay-loaded) entries from an x64 PE, checking bounds."""

    def unpack(fmt, offset):
        if offset < 0:
            raise ValueError("negative PE offset")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    (header,) = unpack("<I", 60)
    if data[header : header + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine, count = unpack("<HH", header + 4)
    (optional_size,) = unpack("<H", header + 20)
    optional = header + 24
    if machine != 0x8664 or unpack("<H", optional)[0] != 0x20B or optional_size < 240:
        raise ValueError("expected x64 PE32+")
    if unpack("<I", optional + 108)[0] < 14:
        raise ValueError("missing PE data directories")
    sections = [
        unpack("<8sIIII", optional + optional_size + index * 40)
        for index in range(count)
    ]

    def offset(rva, size):
        for _, _, address, raw_size, raw_offset in sections:
            if address <= rva and rva + size <= address + raw_size:
                result = raw_offset + rva - address
                if result + size <= len(data):
                    return result
        raise ValueError(f"unmapped RVA {rva:#x}")

    def dll_name(rva):
        start = offset(rva, 1)
        end = data.find(b"\0", start, start + 260)
        if end < 0:
            raise ValueError("unterminated DLL name")
        offset(rva, end - start + 1)
        name = data[start:end].decode("ascii").lower()
        if not name or "/" in name or "\\" in name or ":" in name:
            raise ValueError("invalid DLL name")
        return name

    result = []
    for index, width in [(1, 20), (13, 32)]:
        rva, size = unpack("<II", optional + 112 + index * 8)
        if not rva and not size:
            continue
        if not rva or size < width:
            raise ValueError("invalid import directory")
        for position in range(0, size - width + 1, width):
            start = offset(rva + position, width)
            descriptor = unpack("<" + "I" * (width // 4), start)
            if not any(descriptor):
                break
            if index == 13 and descriptor[0] != 1:
                raise ValueError("expected RVA-based delay imports")
            result.append((dll_name(descriptor[3 if index == 1 else 1]), index == 13))
        else:
            raise ValueError("unterminated import directory")
    return result


class Files:
    def __init__(self, path):
        self.archive = None
        if Path(path).is_dir():
            self.entries = {
                str(p.relative_to(path)).replace("\\", "/").lower(): p
                for p in Path(path).rglob("*")
                if p.is_file()
            }
        else:
            self.archive = zipfile.ZipFile(path)
            names = self.archive.namelist()
            self.entries = {
                name.replace("\\", "/").lower(): name
                for name in names
                if not name.endswith("/")
            }
            if len(self.entries) != len(
                [name for name in names if not name.endswith("/")]
            ):
                self.close()
                raise ValueError("duplicate package paths")

    def read(self, name):
        entry = self.entries[name.lower()]
        return self.archive.read(entry) if self.archive else entry.read_bytes()

    def close(self):
        if self.archive:
            self.archive.close()


def check(package, framework=None, release=False):
    policy = json.loads(POLICY.read_text(encoding="utf-8"))
    errors = []
    files = Files(package)
    runtime = Files(framework) if framework else None
    try:
        manifest = ET.fromstring(files.read("appxmanifest.xml"))
        declaration = next(
            (
                node
                for node in manifest.iter(FOUNDATION + "PackageDependency")
                if node.get("Name") == "Microsoft.VCLibs.140.00"
            ),
            None,
        )
        framework_declared = declaration is not None
        if runtime:
            identity = ET.fromstring(runtime.read("appxmanifest.xml")).find(
                FOUNDATION + "Identity"
            )

            def version(value):
                parts = tuple(int(part) for part in value.split("."))
                if len(parts) != 4:
                    raise ValueError("expected four-part framework version")
                return parts

            if (
                not framework_declared
                or identity is None
                or identity.get("Name") != declaration.get("Name")
                or identity.get("Publisher") != declaration.get("Publisher")
                or identity.get("ProcessorArchitecture") != "x64"
                or version(identity.get("Version", ""))
                < version(declaration.get("MinVersion", ""))
            ):
                errors.append(
                    "VCLibs framework identity/version does not satisfy the manifest"
                )
                framework_declared = False
        required = {"resources.pri"}
        if release:
            required.update(["boot.cfg", "dxil.dll", "spirv_to_dxil.dll"])
        executables = []
        for node in manifest.iter():
            if "Executable" in node.attrib:
                executable = node.attrib["Executable"].replace("\\", "/")
                required.add(executable)
                executables.append(executable.lower())
            for attribute in [
                "Square150x150Logo",
                "Square44x44Logo",
                "Wide310x150Logo",
                "Image",
            ]:
                if attribute in node.attrib:
                    required.add(node.attrib[attribute].replace("\\", "/"))
            if node.tag == FOUNDATION + "Logo" and node.text:
                required.add(node.text.replace("\\", "/"))
        if not executables:
            errors.append("manifest has no application executable")
        for name in sorted(required):
            normalized = name.lower()
            path = PurePosixPath(normalized)
            scaled = str(path.with_name(path.stem + ".scale-200" + path.suffix))
            if normalized not in files.entries and not (
                path.suffix == ".png" and scaled in files.entries
            ):
                errors.append(f"missing required file: {name}")
        # Inspect every bundled DLL too: a dynamically loaded shader translator can have
        # transitive dependencies that do not appear in the executable's import table.
        pending = [(files, name) for name in files.entries if name.endswith(".dll")]
        pending.extend((files, name) for name in executables if name in files.entries)
        visited = set()
        while pending:
            source, name = pending.pop()
            identity = (source is runtime, name)
            if identity in visited:
                continue
            visited.add(identity)
            try:
                imports = pe_imports(source.read(name))
            except (ValueError, struct.error, UnicodeError) as error:
                errors.append(f"{name}: invalid PE: {error}")
                continue
            for dependency, delayed in imports:
                edge = f"{name} -> {dependency}" + (" (delay)" if delayed else "")
                if dependency in files.entries:
                    pending.append((files, dependency))
                elif dependency in policy["framework_dlls"]:
                    if (
                        not framework_declared
                        or runtime is None
                        or dependency not in runtime.entries
                    ):
                        errors.append(f"{edge}: missing declared VCLibs framework DLL")
                    else:
                        pending.append((runtime, dependency))
                elif (
                    dependency in policy["system_dlls"]
                    or delayed
                    and dependency in policy["optional_delay_dlls"]
                ):
                    pass
                else:
                    errors.append(
                        f"{edge}: missing DLL; package it or review the Xbox OS contract"
                    )
    finally:
        files.close()
        if runtime:
            runtime.close()
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="staged layout directory or APPX")
    parser.add_argument(
        "--framework", type=Path, help="VCLibs x64 APPX or unpacked framework"
    )
    parser.add_argument(
        "--release",
        action="store_true",
        help="require the library shader DLLs and boot.cfg",
    )
    args = parser.parse_args()
    try:
        errors = check(args.package, args.framework, args.release)
    except (OSError, ValueError, KeyError, ET.ParseError, zipfile.BadZipFile) as error:
        errors = [str(error)]
    for error in errors:
        print(f"launch check: {error}", file=sys.stderr)
    if errors:
        return 1
    print("Launch dependency/file presence check: PASS (not an Xbox activation test)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
