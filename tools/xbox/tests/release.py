#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later

import importlib.util
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[1] / "release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def rejects(version, manifest, releases):
    try:
        release.validate(version, manifest, releases)
    except ValueError:
        return True
    return False


manifest_text = '﻿<?xml version="1.0"?>\n<Package>\n  <Identity Name="Jadyx"\n' \
    '            Publisher="CN=JadyxDev"\n            Version="0.2.99.4"\n' \
    '            ProcessorArchitecture="x64" />\n</Package>\n'
with tempfile.TemporaryDirectory() as directory:
    manifest = Path(directory) / "AppxManifest.xml"
    manifest.write_bytes(manifest_text.encode("utf-8"))

    # The first release only has to beat the manifest.
    assert release.validate("0.3.0", manifest, [[]]) == "0.3.0.0"
    assert release.validate("v0.3.0", manifest, [[]]) == "0.3.0.0"
    assert rejects("0.2.99", manifest, [[]])  # 0.2.99.0 <= 0.2.99.4: consoles would not update
    assert rejects("0.3", manifest, [[]])
    assert rejects("0.3.0.1", manifest, [[]])
    assert rejects("0.3.0-beta", manifest, [[]])
    assert rejects("0.3.70000", manifest, [[]])

    published = [[{"tag_name": "v0.3.0", "draft": False}, {"tag_name": "v0.4.0", "draft": True},
                  {"tag_name": "nightly", "draft": False}]]
    assert rejects("0.3.0", manifest, published)  # already published
    assert release.validate("0.3.1", manifest, published) == "0.3.1.0"
    assert release.validate("0.4.1", manifest, published) == "0.4.1.0"
    assert rejects("0.4.0", manifest, published)  # the draft holds the tag

    # bump rewrites only the version and keeps the BOM.
    release.bump("0.3.0.0", manifest)
    assert manifest.read_bytes() == manifest_text.replace("0.2.99.4", "0.3.0.0").encode("utf-8")
    assert release.manifest_version(manifest) == (0, 3, 0, 0)
    assert rejects("0.3.0", manifest, [[]])  # now equal to the manifest

log = "".join(f"{subject}\x1f{body}\x1e" for subject, body in [
    ("chore(uwp): bump version to 0.3.0.1", ""),
    ("fix(input): name a pad by its maker", "Longer explanation.\n"),
    ("feat(library): folders", ""),
    ("perf: faster draws", ""),
    ("docs: notes", ""),
    ("Merge remote-tracking branch 'eden/master'", ""),
    ("Merge pull request #4 from someone/branch", "fix(audio): no crackle\n\nDetails\n"),
    ("Tidy the menu", ""),
])
text = release.notes(log, "v0.3.1", "v0.3.0", "owner/jadyx")
assert text == """## New

- **library:** folders

## Fixes

- **input:** name a pad by its maker
- **audio:** no crackle

## Performance

- faster draws

## Other changes

- Tidy the menu

**Full changelog:** https://github.com/owner/jadyx/compare/v0.3.0...v0.3.1
""", text
assert release.notes("docs: x\x1f\x1e", "v0.3.1", "v0.3.0", "").startswith("No player-facing")
print("release: ok")
