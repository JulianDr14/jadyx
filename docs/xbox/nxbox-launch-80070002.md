<!--
SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Series activation 0x80070002: 0.3.8.0 versus 0.3.8.5

## Finding and status

**The reported activation regression is not fixed or reproduced by this change.**
The missing-launch-file hypothesis was investigated against both source tags and
locally available release APPX files. No new missing dependency was found. The
packaging validation gap is fixed; identifying the Series failure still requires
activation evidence from the failing console. Treating a passing package check as
a launch repair would contradict the artifact comparison below.

Work starts at `main` commit `0fc5c01836` on
`nxbox/30-launch-80070002`. No network, GitHub, PC runtime, console, full build or
signing operation was used. Artifacts were only read from
`/Users/miguel/.local/share/nxbox/jadyx-test/{rel0,rel}`. Their names identify the
versions, but their relationship to the exact installation that failed on the
owner's Series has not been independently established. No binaries or private
payloads are added to the repository.

## Source and artifact evidence

`git diff v0.3.8.0 v0.3.8.5` contains 57 changed files. In particular:

- `dist/uwp/AppxManifest.xml:20`: only `Identity.Version` changes from 0.3.8.0
  to 0.3.8.5. Executable `jadyx.exe`, entry point `Jadyx.App`, capabilities,
  target OS and VCLibs dependency are unchanged (`:30–44`, `:55`).
- `tools/xbox/package-appx.ps1` and `src/jadyx/CMakeLists.txt` are unchanged
  between those tags. The latter's delay-load list (`:208–219`) does not gain
  a new hard DLL import. The packaging script copies the executable and sibling
  DLLs (`:122–129`), stages shader DLLs (`:132–148`), and generates the main PRI
  from Assets (`:237–256`). Before this change it never inspected PE imports.
- `src/jadyx/app/uwp_boot.cpp:2981–2984`: entry is still
  `winrt::init_apartment()` followed by `CoreApplication::Run`. The changes
  between the tags add pipeline-key configuration and enable async recording by
  default (`:314–323`, `:521–522`), applied inside `RunHeadlessBoot` after a
  library game is selected. They do not introduce a new package startup file.
  Installed-location handling at `:2855–2874` and its diagnostic catch remain.
- `src/video_core/renderer_d3d12/d3d12_shader_compiler.cpp:73–99` dynamically
  opens `spirv_to_dxil.dll` and `dxil.dll`. Failure is logged and returns from
  the compiler constructor. Both DLLs are present and byte-identical in the
  two packages. They are required for a release's shader path, rather than
  being newly introduced OS activation imports.
- `src/jadyx/ui/ui_fonts.cpp:30–55`: bundled Inter fonts also have a Segoe UI
  fallback. All four bundled fonts are present and identical in both packages.

The following measurements were made on the local ZIP/PE contents, without
executing them. Both APPX files contain **38 entries with the same names**.
Every entry except these six is byte-identical: `jadyx.exe`, `AppxManifest.xml`,
`AppxBlockMap.xml`, `[Content_Types].xml`, `AppxMetadata/CodeIntegrity.cat`,
`AppxSignature.p7x`. The manifest differs only in version; content types differ
only in element order. Changed signatures, block hashes and code-integrity data
are expected for a changed executable; their cryptographic validity was not
certified here.

| Item | 0.3.8.0 | 0.3.8.5 |
| --- | ---: | ---: |
| APPX bytes | 15,754,812 | 15,791,146 |
| EXE bytes | 33,903,616 | 33,930,752 |
| `resources.pri` bytes | 3,208 | 3,208 (identical) |
| `dxil.dll` bytes | 1,509,800 | 1,509,800 (identical) |
| `spirv_to_dxil.dll` bytes | 9,726,976 | 9,726,976 (identical) |
| `boot.cfg` bytes | 17 | 17 (identical: library/play) |

APPX SHA-256:

```text
0.3.8.0 f92b4b83f1074a48ae03eb8b33cc4d292afe76ec9fef9ffd8f135c7a082f623c
0.3.8.5 b063c9e54d53e602fed40147f11148782aa11f695ce17b579bd073e90770fdad
```

A direct PE table comparison found equal hard-import DLL/name-or-ordinal symbol
sets and equal delay-import DLL/symbol sets in the two executables. The DLLs'
imports are necessarily identical because the DLL bytes are identical. This
rules out a *new* hard dependency introduced in this executable pair. It does not
prove that every import is supported by the console OS, that activation state is
healthy, or that startup code cannot fail. The generic HRESULT alone does not
identify the missing path; no console activation trace was available in scope.

## Change

`tools/xbox/check-package-launch.py` reads a layout directory or an APPX with
Python's standard library. It checks:

- The manifest's executable, tile/splash assets (including Jadyx's scale-200
  candidates), and `resources.pri` exist.
- Release layouts also contain `boot.cfg` and both dynamically opened shader DLLs.
- Actual x64 PE hard and delay-import tables of the executable and every bundled
  DLL, recursively including dependencies supplied by the VCLibs framework.
- The supplied framework's identity, publisher, architecture and minimum version,
  plus the presence of the actual CRT DLLs the PE imports.
- Unknown DLL/API-set names fail closed. The explicit OS baseline is in
  `tools/xbox/launch-dependencies.json`, derived from the working-version
  artifact and the supplied VCLibs contracts. Reviewed desktop delay imports
  are exempt **only while delay-loaded**; future additions require review.

`package-appx.ps1` now stages VCLibs and runs the check after MakePri and local
staging hooks, before MakeAppx and signing. A nonzero check result throws. This
also covers the existing CI packaging call without a separate CI-only path. The
CI script job runs the new host regressions on pull requests.
Python 3 is already used by the project's tooling; no third-party module is
needed. Runtime behavior, C++ code, manifest version and renderer defaults are
not changed, so no experimental behavior switch is introduced.

This is a presence gate, not a PE export resolver, PRI decoder, signature
verifier or Xbox loader emulator. Existing `verify-appx.ps1` remains responsible
for publication integrity. Arbitrary `LoadLibrary` strings in future code need
explicit additions to the release requirements; import tables cannot reveal
those. The OS baseline records existing dependencies and does not certify API
availability on every Xbox OS revision.

## Host validation (macOS)

Before: six existing standalone suites passed: `release.py`, `gc-trace.py`,
`address-wait-trace.py`, `pipeline-build-trace.py`, `draw-preparation-report.py`,
`material-probe-parser.py`, all under `tools/xbox/tests/`. The newly written
regression suite initially failed because the checker did not exist. The old
packager also has no check invocation. These reproduce the missing validation,
**not the reported console activation failure**.

After: the same six suites pass; all **17** new `package-launch.py` tests pass.
They exercise missing executable/PRI/assets/dynamic DLL, direct/transitive/delay
imports, optional delay versus hard imports, unknown API sets, cycles, framework
presence/version, malformed PE, archive input and pre-pack invocation. Both real
APPX files pass with the supplied VCLibs package. A temporary copy of the real
0.3.8.5 APPX with `dxil.dll` removed fails with exit **1** and
`missing required file: dxil.dll`. PowerShell parser validation, Ruff formatting
and lint, and `git diff --check` pass.

The requested NXbox unittest command was run before and after:

```sh
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

It reports `ImportError: Start directory is not importable: 'tests/port'` both
times: that directory belongs to NXbox and does not exist in this Jadyx tree.
The NXbox estimate of three failures/five errors does not apply here.

Re-run the new tests and validate either an existing layout or APPX:

```sh
python3 -B tools/xbox/tests/package-launch.py
python3 -B tools/xbox/check-package-launch.py build-uwp/package/layout \
  --framework build-uwp/package/Microsoft.VCLibs.x64.14.00.appx --release
```

## Exact console test still required (not executed)

1. On the Windows build host, build and stage a library-only diagnostic delivery
   at a version higher than the installed version. For an installed 0.3.8.5:

   ```bat
   tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Release -Version 0.3.8.6
   python tools\xbox\check-package-launch.py build-uwp\package\jadyx.appx --framework build-uwp\package\Microsoft.VCLibs.x64.14.00.appx --release
   ```

   Version is overridden only in the staged manifest. Use an even higher version
   if necessary; do not uninstall or erase user data to force a downgrade.
2. Record the APPX SHA-256, exact Xbox OS version and installed VCLibs version.
   Install `build-uwp/package/jadyx.appx` through Device Portal with the matching
   framework. Set the app to **Game** mode. Record the deployment result.
3. Start from Dev Home; record the exact activation HRESULT and whether the
   library appears. Repeat using Device Portal's launch action and record its
   result and whether a process is created. Check whether the timestamps of
   `LocalState/jadyx/log/jadyx_diag.txt` and `jadyx_log.txt` advance; preserve
   their contents and the console's available activation/crash diagnostics.
   No game, keys or firmware are needed to test library activation.
4. If 0x80070002 persists, obtain the failing activation event/loader path before
   changing another dependency. Compare it with the actual installed package
   file list and these artifact hashes. If a process starts, distinguish an
   application startup exception from a loader refusal using the fresh diag.
5. Acceptance for a future runtime repair: the affected Series X reaches the
   library on repeated cold launches and after an in-place upgrade, with fresh
   startup diagnostics. Passing this new presence gate alone is insufficient.

**Remaining acceptance gap:** the failing file/path and root cause are unknown;
no launch repair is claimed in this commit. The new packaging guard is complete
and exercised on the current tree and both locally available release artifacts.
