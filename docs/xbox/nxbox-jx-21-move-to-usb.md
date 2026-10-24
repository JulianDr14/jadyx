<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Verified internal game move to USB (jx-21)

24 October 2026; branch `nxbox/21-move-to-usb`, base `nxbox/integration3`
(`0fc5c0183694d4cc91b062f00a5641315de30003`, also the local main at task start).
One local implementation commit; no push, network, Windows PC or console execution.

## Finding and reference

Read the rank-2 gate and I15 evidence row in local branch
`nxbox/0e-feature-diff:docs/xbox/jadyx-vs-nxbox-features.md` first. Jadyx can play
from authorized USB folders and relocate individual content, but has no whole-game
move. Its installed updates/DLC are generated object directories governed by a
transactional schema-1 catalog; a plain base-file move would strand embedded
references or leave installed content behind.

Read-only reference: NXbox local `rc/next` at
`1087264c852c68731a436aca0cb36c849bff734c`,
`src/eden_uwp/ui/library.cpp:955` and
`src/frontend_common/library_transfer.h`. The latter supplies the GPL transaction
idea (capacity/overflow checks, group verification before cleanup), attributed in
`verified_move.h`. Its checked copy tests size, not a whole-file cryptographic hash.
This implementation adds independent SHA-256 checks and Jadyx catalog publication.
No upstream contributions or unrelated NXbox features are included.

## Change and boundaries

- Default off: `NXBOX_MOVE_TO_USB=1` in the process environment, or the same exact
  line in the installed diagnostic package's `boot.cfg`. Without either, the six
  existing game options and their ordering are unchanged. No settings migration.
- Library → game Options → **Move to USB**. Press A twice, then select a USB folder
  through Jadyx's existing asynchronous folder picker. Cancelling the picker writes
  nothing. UI uses the existing canvas/painter, Palette, TypeRole and EN/ES/PT-BR
  catalogs, with preparation, MiB progress, cancellation and result messages.
- Moves the **selected internal base copy**, including its embedded content, and
  **every registered internal update/DLC revision for its application**, including
  disabled and older revisions. Separate base copies and existing external content
  references remain where they were. No heuristic filename/title-ID matching.
- Saves, mods, cheats and caches remain internal. A saved Pulse session blocks the
  action: it captures the original launch identity and cannot safely be relabelled.
  The user must explicitly discard it in the existing Pulse pane first. Unknown
  title IDs and external base copies are refused.
- The existing serial MTA storage worker joins the stopped metadata reader. Portal
  mutations are blocked before admitting the job; guest launch, other storage jobs
  and update installation cannot run concurrently with the move.
- Existing content catalog schema 1 is reused: generated internal object references
  become token-scoped external directory references. Embedded references to the
  moved base become references to its new container. IDs, enabled/verified states,
  versions, requirements, other titles and managed-application choices are retained.
  `source_size=0` represents external directories; `source_stamp=0` requires normal
  content verification on launch. No format/read-path change to old library/settings.

## Transaction and numbers

1. Validate internal paths, reject symlinks/reparse components, and hold source
   handles denying writes/rename/deletion. Hash the sources with SHA-256.
2. Preflight the entire manifest and broker-reported USB free space. Require total
   bytes **plus 64 MiB**; unknown capacity fails closed. At most **4096 files**;
   aggregate overflow, duplicate relative names (ASCII case-insensitive), traversal,
   ADS and invalid paths are refused. Never overwrite an existing destination.
3. Copy with **1 MiB chunks** through WinRT streams into a new `.jadyx-move*` staging
   folder under the authorized destination. Flush and close every output. Interrupted
   staging folders are excluded from library scanning. No whole-game RAM buffer.
4. Reopen **all destinations** through Jadyx's existing token/VFS storage broker;
   check size, exact EOF and SHA-256 against the locked source hashes.
5. Rename the whole folder to `Jadyx-<16-digit application ID>` using FailIfExists,
   then reopen and verify **all files again** under the published identity.
6. Publish the relocated catalog **twice**, through `ContentStore::Save`, so both
   current and fallback catalogs point to USB. Only then mark the locked source
   handles for deletion. Do not call RemoveGame (which also removes saves/content).

Cancellation is checked between files and 1 MiB hashing/copy chunks. An in-flight
WinRT call can delay it. Once the catalog is committed, cleanup finishes without
rolling the catalog back. A cleanup failure reports remaining internal duplicates.
Any earlier failure retains **all originals**. Best-effort staging cleanup does not
remove published copies: failed publication/registration or interrupted runs can
leave USB duplicates, which a retry refuses to overwrite. Inspect them manually;
there is no automatic resume or recovery deletion. Old empty internal object
folders can remain until the existing content manager cleanup.

Source hashing, copy reads, staging verification and published verification perform
roughly four payload-length reads plus one payload-length write. No transfer-speed,
space-reclamation or console-performance measurements are claimed. The transaction
handles application termination; it does not certify a USB device's power-loss cache.

## Host verification

Before implementing `verified_move.h`, the new `game-move.cpp` regression failed to
compile because the feature header was absent. After implementation, all three new
executables pass:

```sh
python3 -B tools/xbox/tests/game-move.py
```

This runner uses installed clang++ and OpenSSL/libcrypto through pkg-config; it
installs/downloads nothing. It enables shadow/conversion warnings as errors, with
sign-conversion alone disabled for the pre-existing `storage_path.h` range-for.
New aggregates use named fields. New C++ and edited C++ regions were clang-formatted;
Python was formatted with the already cached ruff binary.

- `game-move`: **18 transaction cases**, including mid-copy/verification errors,
  publication/registry failure, cancellation, unknown/insufficient capacity, overflow,
  duplicate/case-colliding names and unsafe paths. No source removal before commit.
- `game-move-data`: multi-chunk SHA-256, equal-size corruption, short reads, extra EOF
  bytes, cancellation, empty-vector hash and catalog selection/reference preservation.
  Two real-file transactions use original synthetic bytes: corrupted last DLC keeps
  every original; the valid group publishes and removes originals after registration.
- `game-move-options`: default-off list, opt-in list, second confirmation and navigation.

Existing gates, run before and after:

| Selection | Before | After |
| --- | --- | --- |
| 12 frontend C++ sources from `run-frontend.ps1`, built with clang++/JADYX_NO_WINRT | 9 pass, 3 abort | Same |
| Existing `storage-path.cpp`, same host build | Pass | Pass |
| 10 Python fixture scripts listed below | 10 pass | Same |
| `updater-page.cjs` | Pass | Pass |
| `network-portal-page.cjs`, `log-export-page.cjs` | Both fail in existing browser mocks | Same |
| Requested `tests/port` unittest discovery | ImportError: directory absent | Same |

That is **26 existing executable gates: 21 pass, 5 pre-existing failures**, unchanged;
plus **3 new passing gates**. The three existing C++ aborts are `game-menu`,
`system-settings`, `cheats-page`. All 13 selected existing C++ sources compile.
The Python fixtures are `release`, `draw-preparation-report`, `pipeline-build-trace`,
`address-wait-trace`, `gc-trace`, `material-trace`, `material-probe-parser`,
`dxil-branches`, `dxil-probe`, `noperspective-interpolation` under `tools/xbox/tests`.
C++ command: `clang++ -std=c++20 -O0 -DJADYX_NO_WINRT -I src <test.cpp>
src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp -o <temporary-binary>`;
`cheats-page` additionally links `src/core/memory/cheat_text.cpp`. Run each binary.
Node commands: `node tools/xbox/tests/<name>.cjs`. Python commands:
`python3 -B tools/xbox/tests/<name>.py`.

The requested discovery command was executed both times; `tail` success is not suite
success:

```sh
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

No full native build/dependency setup, MSVC/UWP compile or console gate was possible
on this Mac. Passing host policy tests do not validate WinRT availability, picker
permissions, USB write/flush/rename semantics or game launching on Series.

## Exact future console gate (not executed)

1. On a later authorized Windows build host, record revision/version, Series X/S,
   Game mode and USB filesystem. Use disposable copies of the owner's already local
   base/update/DLC, never their only copy. Include an enabled update, older disabled
   update and disabled DLC; record IDs, versions, requirements and enabled states.
   Keep a save sentinel and an unrelated title. Do not add any payload to the repo
   or package. Discard only the disposable title's Pulse snapshot explicitly.
2. Build with `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`.
   Set a fresh package version and archive matching EXE/PDB. Package the opt-in test:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -BootCfg NXBOX_MOVE_TO_USB=1
   ```

   Use only `build-uwp/package/jadyx.appx`. Do not use `-Release` for the opt-in test;
   release packaging deliberately rejects diagnostic BootCfg options. First also
   review a plain `-Library` package: no Move option and existing menus/files load.
3. Install in place in Game mode. Visit the new pane in EN, ES and PT-BR. First A
   only displays confirmation; Back/section change cancels it. Second A opens the
   picker. Cancel it: no files/catalog changes. External games and saved Pulse
   sessions must be refused. Repeat with normal folder authorization and after restart.
4. Pick an empty USB folder. Before success, repeatedly inspect source presence;
   originals must remain through copy and BOTH hash passes. After success verify the
   `Game move: verified and registered` log precedes every `removed verified source`.
   Compare exact sizes and SHA-256 of the entire USB bundle against the disposable
   reference copies. Launch from USB, verify the selected update/DLC and existing save
   sentinel, toggle the disabled DLC, and restart the app. Selections must persist;
   the unrelated title must remain unchanged. Both catalog generations must resolve.
5. Restore disposable input for each fault case. Cancel during preparation, early
   copy and near end; disconnect USB during copy and during verification; use a
   read-only USB; fill a stick until free bytes are below total+64 MiB; precreate
   `Jadyx-<application ID>` and put an unrelated sentinel inside. Require failure or
   cancellation, every original retained, no overwritten sentinel and no incomplete
   game exposed from `.jadyx-move*`. Reconnect and restart before inspecting/retrying.
6. Force-close the disposable test during staging, after folder publication and
   during cleanup. On restart, staging remains hidden; a published copy is never
   overwritten/deleted automatically. Before catalog commit all originals must
   survive; after commit USB must launch even if internal cleanup was interrupted.
   Simulate unavailable source/delete permissions with disposable data: no deletion
   before verification; cleanup failure must name retained duplicates rather than
   claim full reclamation. Test an existing shared external content reference too:
   it remains a reference and may require its original USB.
7. Test an owner-created valid local package larger than 4 GiB on an appropriate
   USB filesystem; verify hashes, offsets, MiB progress and launch after restart.
   Do not certify FAT32 large-file support or infer it from total free space.
8. Export `jadyx_diag.txt`, `jadyx_log.txt`, both content catalog generations, file
   sizes/hashes and exact fault timing privately. Require no new Critical/Render
   failures, responsive cancellation/return and no changed saves. Gameplay has
   `play=1`, no programmed timeout/input; the owner closes with Q.
