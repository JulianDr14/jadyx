# USB discovery: isolate sources and keep automatic roots in memory

Task: jx-41-usb-discovery-fix. Branch: `nxbox/41-usb-discovery-fix`, based on
`nxbox/40-usb-play-in-place` at `a12e3c3428`, not `main`.
This document supersedes the persistence and global scan-budget rules in
[nxbox-usb-play-in-place.md](nxbox-usb-play-in-place.md).

## Finding

Discovery called `RememberGameFolder(drive)`, permanently adding whole-drive
roots to FutureAccessList. The overlap check only prevented *new* discovery; it
did not suppress roots already saved there. The subsequent scan iterated saved
sources in list order, recursively scanned every directory up to five levels,
and shared one 10000-entry counter and `limited` flag. An earlier broad source
could therefore exhaust the budget before the selected games folder ran. Later
launches would not print a discovery log because the root was already saved.
There is no explicit clearing of games on `limited`: early termination leaves
later sources unvisited, and the library replaces its previous catalog with that
partial scan and shows the limit banner. These are verified code defects. The
reported console state itself is unavailable: with exactly one saved
`F:\NXbox\games` source and a matching `F:\` broker path, the original overlap
helper already suppresses that drive. A stale root, another broad source, or
different broker paths is needed to explain the observed empty result; the new
per-source logs distinguish those cases instead of claiming an unobserved state.

## Changes

- Snapshot and scan registered folders before automatic discovery. An explicit
  subfolder suppresses its drive root, including a root saved by the old build,
  regardless of list order. Disconnected selections retain their metadata and
  saved permissions. No migration deletes or rewrites FutureAccessList entries.
- Newly discovered roots have session-only GUID tokens backed by a synchronized
  in-memory `StorageFolder` registry. VFS resolution, metadata, directory access,
  preflight and read-only file opening use that registry before FutureAccessList.
  Tokens remain stable during refreshes in this process; they are not persistent
  favorites/history identities across app restarts. Register a folder explicitly
  for persistent identity. GUIDs avoid reusing an identity for a different drive.
- Every source gets its own 10000-entry budget. Preserve partial results from a
  capped source, append results from subsequent sources, and retain the warning.
  The existing maximum of 16 scanned sources and 64-item pages remains. Worst-case
  external enumeration is now 160000 entries, rather than 10000 globally.
- Automatic roots and saved drive-letter roots only descend into `games`,
  `switch`, `NXbox`, `Jadyx`, `NXbox/games`, and `Jadyx/games` (case insensitive).
  Only `.nsp`, `.xci`, `.nro` files are listed, at the root or inside those folders.
  No unrelated root directory or deeper subdirectory is entered. File depth is
  at most two directory components; `NXbox/games/title.nro` is included,
  `NXbox/games/title/boot.nro` is not. Explicitly selected non-root folders keep
  the existing five-level traversal. Selecting a deeper folder restores that
  use case without scanning an entire drive.
- Each scanned source logs path, token, visited entry count, game count and limit
  status. Suppressed saved roots log the registered-subfolder reason. Discovery
  logs `session-only` and never calls `RememberGameFolder` or writes permissions.

Discovery remains opt-in through `NXBOX_USB_PLAY_IN_PLACE=1` in `boot.cfg` or the
process environment, default off. Source isolation and legacy-root suppression
are safe fixes that also apply to previously registered sources with the flag off.
No renderer, UI, localization, manifest, dependencies or package-version changes.
The local NXbox checkout was inspected read-only for its known-folder discovery
approach; Jadyx keeps its own StorageFolder/VFS and naming implementation.

## Host validation (macOS, offline)

```sh
python3 -B tools/xbox/tests/usb-play-in-place.py
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror \
  -Wno-missing-field-initializers -Isrc tools/xbox/tests/usb-discovery.cpp \
  -o /tmp/jadyx-usb-discovery-gate
/tmp/jadyx-usb-discovery-gate
git diff --check
```

Before: existing suite **5/5 passed**, zero failures/errors. New policy tests
failed to compile before implementation (**1 error** in the same five-test suite).
After: **6/6 passed**, zero failures/errors. Coverage includes mixed-case path
boundaries, selected-folder suppression, legacy-root suppression in both list
orders, known names/depth exclusions, exactly 10000 accepted entries followed by
a cap, and retention of one partial result plus **17** games from the next source.
An additional source integration guard checks session registration, session VFS
resolution, root policy activation and absence of discovery persistence.
The strict Clang policy build and diff whitespace check pass. The missing-field
warning exclusion is for pre-existing aggregates in `game_library.h`; new
aggregates are initialized by field. Changed C++ lines use `git-clang-format`.

This checkout has no `tests/port` suite; it uses `tools/xbox/tests`. The UWP
translation unit requires Windows SDK/C++/WinRT, unavailable on this Mac. Its
full compilation and the Xbox gate remain unverified. No Windows PC, console,
network, package build or push was used.

## Exact console gate (not executed)

1. Use the same Series X in Game mode and the same registered
   `F:\NXbox\games` containing the 17 owned dumps from the report. Build this
   branch with the normal Windows UWP toolchain, package in library mode, and
   preserve application data when updating. Set `NXBOX_USB_PLAY_IN_PLACE=1` in
   the installed `boot.cfg`.
2. Open the library and refresh twice. Both scans must show all 17 games. Logs
   must include `scanned source=F:\NXbox\games ... games=17 limited=false`.
   There must be no `discovered USB source=F:\` or scan of that root. If a
   legacy root is saved, expect `skipped drive root=F:\ (registered subfolder)`.
   Preserve the logs including source entry counts if the numbers differ.
3. Launch one of those games, return to the library, and verify it remains
   listed. This checks the existing registered-source read path.
4. On a separate test USB with no registered folder, put your own homebrew in
   `games/a.nro`, `NXbox/games/b.nro`, `Jadyx/games/c.nro`, `switch/d.nro`, and
   `root.nro`. Put additional copies in `unrelated/ignored.nro` and
   `games/deeper/ignored.nro`. Refresh: the five allowed files must appear,
   the two excluded files must not, and logs must show `session-only` followed
   by that source's scan counts. Launch one allowed homebrew to exercise the
   session-token VFS. Refresh again; its session token must remain the same.
5. Restart with the flag off. The separate automatically discovered USB must
   disappear and must not appear as a saved source in Settings > File manager;
   the 17 explicitly registered games must remain. Restart with the flag on
   to verify automatic rediscovery. Do not remove the owner's saved folder.
6. Cap isolation: explicitly register a separate fixture folder containing
   over 10000 ordinary non-game entries, and another fixture folder with your
   own homebrew. Refresh with both source orders if practical. The capped
   source must log `entries=10000 ... limited=true`, the other source must
   still log and list its homebrew, and the original 17 games must remain.
   A scan-limit banner is expected in this fixture. Remove only test sources
   afterward. No cap case may empty results contributed by other sources.

Console acceptance is pending; host tests do not certify WinRT permissions or
physical USB behavior.
