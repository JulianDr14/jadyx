# Integration4: feature union and recovery seams

## Scope and history

Local branch `nxbox/integration4` starts at `nxbox/integration3`. It contains normal
merge commits, in this order:

1. `nxbox/19-compat-fresh`
2. `nxbox/1a-game-info-menu`
3. `nxbox/1b-missing-covers`
4. `nxbox/20-files-browser`
5. `nxbox/22-warmup-skip`
6. `nxbox/24-mod-language-filter`
7. `nxbox/23b-game-language`

The old-main branches `nxbox/21-move-to-usb` and
`nxbox/39-applet-errors-optin` were each transplanted onto that union as one
squashed commit, without rewriting the source branches. A final
`fix integration seams` commit contains the integration corrections and this report.
All work is local to Jadyx; no network, push, PC application, console or UWP build
was used. Existing project dependencies were reused without installing anything.

## Findings and changes

- The union silently produced two `Compatibility::Report::date` members. The
  existing game-information harness then failed to compile. Keep the single
  optional field and one parser assignment. Both branches' validation survives;
  report dates retain the stricter 64-byte limit.
- Locale entries landed in a different order from the combined `Text` enum.
  `integration-locales.py` reproduced three failures, one each for en/es/pt-BR.
  Reorder complete entries, preserving their translations, to match the enum.
- Merge the complete options list: Information, Mods, Cheats, Updates, DLC,
  Language, Pulse, Remove, plus Move to USB only when enabled. Preserve all
  fields in the canvas view, FTP exclusion, storage-job serialization, warm-up
  state and both runners' test lists. The portable frontend runner now covers
  the same 21 frontend tests as the PowerShell runner.
- Combine missing-cover recovery with per-game language selection: read the ID
  before language-dependent presentation, use the preferred language in cache
  keys and package controls, keep base-cover donor indices separate per language,
  retain retry/short-read handling, and restore the global language on exit.
- Preserve appended README and notebook material. Correct obsolete compatibility
  refresh instructions: refresh is now enabled by default after first draw,
  reads `main/compat/compat.json`, and caches at
  `LocalState/jadyx/compat_cache.json`. `NXBOX_COMPAT_REFRESH` is obsolete.
- Remove unrelated formatting changes from the USB transplant in
  `uwp_library.cpp`; keep changes to existing behavior local.
- USB startup/rescans clean `.jadyx-move*` folders in remembered destinations
  when `NXBOX_MOVE_TO_USB=1` is enabled. Snapshot broker folder names before
  deleting, so deletions cannot shift paging offsets. Respect cancellation,
  skip reparse/unreadable entries, log storage failures, and keep staging hidden
  from library scans even with the option off. Published `Jadyx-<id>` folders
  and source files are never part of staging cleanup.
- A retry finding `Jadyx-<id>` now hashes every expected destination file through
  the same broker/VFS verifier used by a new move. Exact size and SHA-256 matches
  produce an **already published / verify registration and originals** notice;
  missing, different or unreadable files remain a conflict. Recognition neither
  rewrites catalogs nor deletes originals. It deliberately requires manual review
  before cleanup, including checking both catalog generations. No overwrite or
  automatic roll-forward is introduced.
- Applet error UI remains opt-in through `nxbox_error_overlay=1`; shared-buffer
  pixel stride remains opt-in through `shared_buffer_stride_fix=1`. Both default
  off. Weak ownership of the callback and the duplicate-completion guard remain
  unconditional in `applet_error.cpp`.

## Host verification

Baseline: `integration3`, before any merge. All top-level `.cpp`, `.py` and `.cjs`
files under `tools/xbox/tests` were inventoried. Portable tests were compiled/run;
Windows/GPU/dependency-dependent builds and artifact consumers were recorded
separately. FTP CMake/CTest and the nested core-fixes adapter runner were also run.
No network test server or native GPU fixture runner was started.

| Gate | Before | After |
| --- | --- | --- |
| Common standalone C++ inventory, with supplementary production sources where available | 62 pass; 7 existing runtime/assert failures; 60 unavailable builds | 74 pass; same 7 failures; 61 unavailable builds |
| Python inventory | 31 pass; 5 unsuccessful entrypoints; 9 excluded server/artifact/helper scripts | 36 pass; same 5 entrypoints plus frontend wrapper reporting known failures; same 9 exclusions |
| Node mock gates | 1 pass, 2 existing failures | Same |
| Frontend C++ tests shared with the final runner | 14 pass, 3 existing aborts | 18 pass, same 3 aborts |
| FTP CTest | 4/4 pass | 4/4 pass |
| Nested core-fixes adapters | 4/4 pass | 4/4 pass |
| Integration locale source order | Pass at baseline; 3 failures after union | Pass for all 3 locales |
| Game information / duplicate date | Absent at baseline; compile failure after union | Pass |
| USB move runner | Absent; options test aborts after transplant | 4 binaries pass, including recovery |
| Recovery policy regression | Fails to compile before helper exists | Pass with strict warnings |
| Warm-up control runner | Absent | 2 tests pass |
| Applet error and stride runners | Absent | 3 + 2 tests pass |
| Mod language runner | Absent | 7 harnesses and locale order pass |
| Compatibility curation CLI | Absent | 1 Bun test, 17 expectations pass |
| Requested NXbox `tests/port` discovery | Directory absent | Same directory absent |

The compatibility/freshness harnesses also pass with `-Wall -Wextra
-Wconversion -Wshadow -Werror`. New C++ files pass clang-format verification;
the changed Python runners pass the installed offline Ruff formatter/linter.
`git diff --check` passes.

The raw inventories contain 177 entries before and 196 after; wrappers overlap
individual binaries, so these are entrypoints, not independent assertions.
Standalone build availability is not a gate success: notably, the common compile
command lacks libcrypto for `game-move-data.cpp`; its proper existing
`game-move.py` runner links the installed library and passes. Full-capture and its
parser pass with freshly generated synthetic fixtures and production capture code.
The compatibility tests use an existing local nlohmann include directory. No
passing baseline test became failing after supplying the same required sources.

The three known frontend aborts are `game-menu`, `system-settings`, `cheats-page`.
Additional baseline failures found by the broader inventory remain unchanged:
`dma-clear`, `inline-invalidation`, `assert-macros` (stale expected log wording),
and `thread-cpu-time` (macOS CPU-time API unavailable). The existing
`comparison-sampler.py` adapter does not compile; `lighting-ir.py` cannot load its
native `fmaf`; `project-captured-mesh.py` fails its synthetic fixture. The
`geometry-passthrough.py` entrypoint requires generated SPIR-V artifacts and an
argument and was not certified by a no-argument invocation. The resolution wrapper
reports the existing menu/settings aborts. Both web mock failures are the existing
missing-element `classList` error in `renderUpdate`. The new frontend wrapper
returns failure solely for the same three frontend aborts, not a new regression.
Windows-only, code-generation templates, missing dependency builds and GPU tests
remain unvalidated; host results do not certify MSVC, WinRT or Series behavior.

Useful reproduction commands from the repository root:

```sh
python3 -B tools/xbox/tests/run-frontend-host.py
python3 -B tools/xbox/tests/run-resolution-options.py
python3 -B tools/xbox/tests/run-mod-language-filter.py
python3 -B tools/xbox/tests/integration-locales.py
python3 -B tools/xbox/tests/game-move.py
python3 -B tools/xbox/tests/warmup-control.py
python3 -B tools/xbox/tests/applet-errors.py
python3 -B tools/xbox/tests/applet-stride.py
python3 -B tools/xbox/tests/core-fixes/run.py
bun test tools/xbox/tests/compat-add.test.ts
cmake -S tools/xbox/tests/ftp -B /tmp/jadyx-integration4-ftp
cmake --build /tmp/jadyx-integration4-ftp
ctest --test-dir /tmp/jadyx-integration4-ftp --output-on-failure
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

The last command prints `ImportError: Start directory is not importable:
 'tests/port'` both times; the pipeline's `tail` exit code is not suite success.
Local run logs and raw inventories are under `/tmp/jx50-before` and
`/tmp/jx50-after`; the temporary baseline snapshot excludes project CLAUDE files.

## Exact console integration test (pending, not executed)

Use disposable synthetic homebrew/owned local content, never the only copy of
anything. Preserve saves and existing LocalState. On a later authorized Windows
build host, with no app using the package layout:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library
```

Use a fresh delivery version and archive matching EXE/PDB through the normal
release process. Install `build-uwp/package/jadyx.appx` in place through Device
Portal, in Game mode. Do not uninstall or erase app data.

1. In en, es and pt-BR, inspect Information and Language alongside all existing
   game tabs. Confirm dates, compatibility, storage, playtime, removal confirmation
   and navigation. Change one title's language, restart and verify its NACP title
   and cover, global inheritance, reset, and another title's unchanged language.
   Revisit an update/DLC missing a cover; check recovery, cancellation and unplugged
   USB retry without persisting partial images or borrowing the wrong language.
2. Open Files beside FTP/portal; navigate authorized roots, return during listing,
   and verify exclusive ownership during import/move. Open a normal library with
   no compatibility switch: local metadata must appear promptly and a bounded
   background refresh must preserve newer cache metadata. Test offline fallback.
3. Start homebrew with cached preparation; Skip must drain active workers before
   running, Cancel must return to the library, and a second launch must still work.
4. Package the USB experiment separately:
   `powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -BootCfg NXBOX_MOVE_TO_USB=1`.
   Move a disposable internal title with enabled/disabled updates and DLC into an
   authorized USB destination. Verify exact sizes/hashes, catalog selections,
   launch after restart and unchanged save sentinel. Repeat in all three locales.
5. Interrupt during copy, leaving `.jadyx-move` and a generated unique-name sibling.
   Restart with the USB option enabled: staging must be deleted before scanning,
   originals must remain, and unrelated folders and a published bundle must remain.
   Repeat with a disconnected/read-only USB, cancellation and a reparse point;
   failures must preserve data and leave the library usable. The ordinary package
   keeps the option off and never exposes staging as games.
6. Interrupt immediately after the staging rename but before catalog commit; keep
   disposable originals. Restart and retry into the same destination. An intact
   full matching bundle must show the verify-registration/originals notice, with
   no overwrite, catalog rewrite or original deletion. Change one destination byte,
   truncate a file, remove an update or cancel verification: no verified-success
   claim, no deletion, and a conflict/cancel notice. Check both catalog generations
   before any manual recovery. Test a cut after catalog commit separately: USB
   references must remain valid while retained duplicates await review.
7. A normal package must retain the original error frontend and shared-buffer
   stride. In separate diagnostic packages, use `play=1`,
   `nxbox_error_overlay=1` and then independently `shared_buffer_stride_fix=1` in
   boot.cfg via the existing packaging procedure. Exercise an original homebrew
   guest error, dismiss once, return/relaunch and exit with an error outstanding.
   Verify one completion, no stale callback and modal input isolation; separately
   inspect applet buffer rendering. These opt-ins are not certified by host tests.
8. Record package version, Series model, switches, USB filesystem, fault timing,
   hashes, catalog generations and fresh logs privately. Require responsive return,
   unchanged saves and no new Critical/Render failures. Any gameplay validation is
   manual with `play=1`, no timeout/input script; the owner closes with Q.
