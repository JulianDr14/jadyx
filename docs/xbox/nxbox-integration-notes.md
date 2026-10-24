# Compatibility badges on the integration branch

Task: `jx-17-badges-on-integration`, 24 October 2026.

## Inputs and scope

The clean task worktree was reset with `git reset --hard nxbox/integration`
to `3c62b4ffff`, then merged with `git merge --no-ff --no-commit
nxbox/15-compat-badges` (`fcc85de728`). The single final merge commit includes
the resolutions, regression check and these notes. Its first parent retains
FTP and its safety fixes, USB discovery/play in place, parallel shader signing,
resolution options, user guides and the launch dependency checker.

`nxbox/16-themes` exists but points to `0fc5c01836`, the same commit as `main`
at inspection. It is already an ancestor of integration and contains no theme
changes. Merging that tip is therefore already up to date; no theme feature or
second merge commit is claimed.

No renderer behavior, feature switches, package version or translations were
redesigned. Existing experimental switches remain default off, including
`NXBOX_COMPAT_REFRESH`, `NXBOX_USB_PLAY_IN_PLACE`,
`NXBOX_D3D12_RESOLUTION_SCALE` and the existing `parallel_shaders` boot option.
No network, GitHub, Windows PC runtime, Xbox, dependency installation or push was
used. The reference NXbox checkout was not changed.

## Conflict resolutions

| File | Conflict | Resolution |
| --- | --- | --- |
| `src/jadyx/i18n/locales/en.cpp` | FTP entries versus compatibility entries at the catalog start | Keep both English groups; reorder all entries to match the merged `Text` enum. |
| `src/jadyx/i18n/locales/es.cpp` | Same insertion conflict | Keep both Spanish groups in the same enum order. |
| `src/jadyx/i18n/locales/pt_br.cpp` | Same insertion conflict | Keep both Brazilian Portuguese groups in the same enum order. |
| `src/jadyx/library/uwp_library.cpp` | FTP close callback versus compatibility loader/table declarations before the event loop | Keep the complete `close_ftp` callback, followed by both compatibility declarations. |

The catalogs use keyed `MakeCatalog` entries, so their old source order was not
needed for lookup. Nevertheless, matching the enum exactly is an explicit merge
requirement. Each locale now has **677 entries**, with every translation from
both parents preserved verbatim. Section comments follow the enum groups.
The new `integration-locales.py` checks the entire source order, detecting missing,
duplicate and misplaced entries. It failed for all three locales before resolution
and passes afterward. Existing compile-time catalog completeness and runtime
placeholder checks also pass.

The FTP callback still stops and joins transfers, clears credentials, restores
the file-manager selection and refreshes metadata/scanning after writes.
The compatibility loader still publishes optional tables on the library thread,
starts only after drawing the first frame, and cancels/joins at scope exit.
The designated `LibraryView` initializer includes both the existing configuration
and compatibility table in declaration order. Automatic merges preserve the FTP
screen in the canvas, compatibility source registration in CMake, bundled metadata
copying, and the existing package launch validation.

## Host results on macOS

The before baseline is integration `3c62b4ffff`. Tests were repeated after the
resolution; the play-history baseline was also rerun from an isolated archive of
that commit. No full emulator build or UWP/MSVC compilation was attempted.

| Gate | Before | After |
| --- | --- | --- |
| FTP CTest (`ftp-protocol`, `ftp-transfer`, `ftp-safety`) | 3/3 pass | 3/3 pass |
| Resolution runner | 4 pass, 2 known failures | Same 4 pass, same 2 failures |
| USB runner | 5/5 pass | 5/5 pass |
| Package launch regressions | 17/17 pass | 17/17 pass |
| Parallel shaders, linked shader cache, pipeline residency | All pass | All pass |
| Text entry, game library, play history | All pass | All pass |
| Release, GC trace, address-wait trace, pipeline-build trace, draw-preparation report, material-probe parser | All pass | All pass |
| Compatibility parser, real bundled data and fallback policy | Not present in integration | Pass |
| New catalog source-order regression | Three locale failures in unresolved merge | Pass for all three locales |
| NXbox `tests/port` discovery command | Directory absent | Same directory absent |

The two existing resolution-runner failures are `game-menu` (obsolete fixed-height
layout expectation) and `system-settings` (macOS `wchar_t`/UTF-16 and stale
navigation expectations), already recorded in [resolution notes](nxbox-resolution-options.md).
There are **no new failing host gates**. `tests/port` reports
`ImportError: Start directory is not importable: 'tests/port'`; the generic NXbox
three-failure/five-error estimate does not describe Jadyx.

Compatibility passes with `-Wall -Wextra -Wconversion -Wshadow -Werror` using a
locally available nlohmann header, with no download or repository dependency added.
FTP uses its strict CMake warning options. Existing play-history code fails an
optional stricter `-Werror` probe on a pre-existing missing aggregate-field
initializer in `game_library.h`; its documented build command passes. No unrelated
warning cleanup is included. Ruff formatting/lint and `git diff --check` pass.

Reproduce from the repository root (set `JSON_INCLUDE` to an existing nlohmann
include directory):

```sh
cmake -S tools/xbox/tests/ftp -B /tmp/jadyx-jx17-ftp
cmake --build /tmp/jadyx-jx17-ftp
ctest --test-dir /tmp/jadyx-jx17-ftp --output-on-failure
python3 -B tools/xbox/tests/run-resolution-options.py
python3 -B tools/xbox/tests/usb-play-in-place.py
python3 -B tools/xbox/tests/integration-locales.py
for suite in package-launch release gc-trace address-wait-trace pipeline-build-trace draw-preparation-report material-probe-parser; do
  python3 -B "tools/xbox/tests/$suite.py"
done
for suite in parallel-shaders linked-shader-cache pipeline-residency text-entry; do
  clang++ -std=c++20 -Wall -Wextra -Werror -pthread -Isrc \
    "tools/xbox/tests/$suite.cpp" -o "/tmp/jadyx-jx17-$suite"
  "/tmp/jadyx-jx17-$suite"
done
clang++ -std=c++20 -Isrc tools/xbox/tests/game-library.cpp -o /tmp/jadyx-jx17-game-library
/tmp/jadyx-jx17-game-library
clang++ -std=c++20 -Isrc tools/xbox/tests/play-history.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp -o /tmp/jadyx-jx17-play-history
/tmp/jadyx-jx17-play-history
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror -Isrc \
  -isystem "$JSON_INCLUDE" tools/xbox/tests/compatibility.cpp \
  src/jadyx/library/compatibility.cpp -o /tmp/jadyx-jx17-compatibility
/tmp/jadyx-jx17-compatibility
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

## Exact console integration gate (pending; not executed)

On the Windows build machine, with no running instance using the package layout:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library
```

Use a delivery version higher than the installed version under the normal release
procedure. Verify the launch checker succeeds and the standard
`build-uwp/package/jadyx.appx` contains `compat/compat.json`. Install through Device
Portal, preserve app data and select Game mode.

1. With compatibility refresh disabled and the console offline, open Home and
   Library. Verify all four badge shapes/statuses using the bundled metadata and
   synthetic edits to a backed-up `LocalState/compat/compat.json`, as described in
   [the badge gate](nxbox-compat-badges.md#exact-console-test-pending-not-executed).
   Verify pin placement, focus text, navigation, launch/return and settings in en,
   es and pt-BR. Missing/corrupt cache must fall back silently.
2. On a trusted LAN, start Settings > File manager > FTP. Use only synthetic files:
   declare their exact size with ALLO, upload into `/games`, then press Back.
   Confirm the FTP screen closes, credentials clear and the library rescans while
   badge rendering remains responsive. Reopen FTP; repeat short EOF, cancellation,
   suspend and protected rename checks from the
   [FTP regression gate](nxbox-ftp-server.md#review-regression-gate-jx-21).
   No transfer may survive library exit or publish an incomplete file.
3. In a separate diagnostic package, stage `NXBOX_USB_PLAY_IN_PLACE=1` using
   `-BootCfg`. Discover an original homebrew on USB, refresh twice, launch it in
   place and return. Confirm no duplicate entry or internal copy, working badges
   and persistent source identity. Repeat without the switch to verify default
   discovery remains off. Follow the larger-file/removal gate in
   [USB notes](nxbox-usb-play-in-place.md#exact-console-gate-not-executed) separately.
4. Verify global and per-title internal resolution still save and inherit, with
   rendering native by default. Run the existing
   [resolution console gate](nxbox-resolution-options.md#exact-console-test-3d-world-and-botw)
   only in its opt-in diagnostic package. Compare the existing
   [parallel shader gate](nxbox-parallel-shaders.md#exact-console-test-pending)
   off/on in separate runs; this merge makes no performance claim.
5. Only if explicitly testing remote badge refresh, stage
   `NXBOX_COMPAT_REFRESH=1` in its own diagnostic package. Verify cache publication,
   offline fallback and exit during refresh per the badge gate. Return to a normal
   package afterward; no experimental switch is enabled by this merge.

Record package version, switches, languages, screenshots, fresh diagnostics,
transfer hashes and launch/return results. Host checks cannot certify MSVC warning
behavior, WinRT worker lifetime, rendering, physical USB, FTP sockets, console
activation or performance. Those gates remain open.

## Themes resolution on integration2 (jx-18, 24 October 2026)

The themes branch subsequently advanced to `4fc8c8eaff`; its earlier no-op status
above is historical. Task branch `nxbox/18-themes-on-integration` was reset to
`nxbox/integration2` (`2a25d4862e`) and merged with `nxbox/16-themes`, keeping one
merge commit. Seven conflicts were resolved by retaining both features: all
three catalogs (681 entries in exact enum order), shelf compatibility text with
themed headings, Resolution followed by Theme, both settings includes/fields,
and the theme-aware painter definitions. FTP, USB, resolution, shader signing,
compatibility metadata and test-hook code remain integrated.

The badge draw call now resolves semantic colors through `Ink`; otherwise theme
handles would be interpreted as raw RGB. Two palette slots retain integration's
Playable/Broken colors in both themes; default Jadyx geometry/type/colors remain
covered by frozen-token tests. Console pixel equality is still pending.

Before/after host gates have no new failures: FTP 4/4, USB 5/5, launch 17/17,
locales, compatibility, shader/cache and diagnostic-parser checks pass. Existing
frontend failures remain game-menu, system-settings and cheats-page (the resolution
runner repeats two). The two inherited theme gates and two new integration gates
pass after reproducing the missing tokens/raw-color bug. Full resolutions,
commands, limits and the exact console procedure are in
[nxbox-jx-18-themes-on-integration.md](nxbox-jx-18-themes-on-integration.md).
No network, push, PC runtime or console was used.
