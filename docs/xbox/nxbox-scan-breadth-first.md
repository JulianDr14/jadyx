# Breadth-first library scans (jx-42)

Historical report from commit `928018bd2e`. For the integration5 port, current
per-source budgets, preserved USB policy and verification results, see
[nxbox-jx-43-scan-bfs-on-int5.md](nxbox-jx-43-scan-bfs-on-int5.md).

## Finding

The internal scanner (`ScanGameLibrary`) used a recursive filesystem iterator.
The external/USB scanner in this fork is `ScanFolder` in
`src/jadyx/storage/uwp_rom_storage.cpp`, not `usb_discovery.h`. It recursively
entered each StorageFolder before finishing its parent. Either scanner could
spend all 10,000 visits on unrelated deep files before seeing shallow games.
The reported Series X symptom on 24 October 2026 was an empty library with the
existing five-level/10,000-entry scan-limit notice, including on 0.3.8.1.

The frontend already merges partial internal/external results, assigns the scan
without clearing its entries when `limited` is true, and displays the translated
`ScanLimit` notice alongside the library. No UI or translation changes are needed.

## Change

`library/library_scan.h` provides a shared FIFO traversal used by both scanners.
Finish enumerating one folder before visiting queued children; all folders at a
shallower level are enumerated before deeper folders within that source.

- Keep the existing 10,000-entry safety cap (files and directories both count).
- Keep five levels: root entries are depth 0, entries at depth 4 are included,
  and directories at depth 4 are not descended into.
- Keep cancellation, filesystem symlink exclusion, case-insensitive NSP/XCI/NRO
  matching, token-relative USB identities, creation dates and 64-item WinRT pages.
- Retain every game already found when the budget runs out. An actual entry beyond
  the cap sets `limited`; exactly 10,000 entries without more work is complete.
- Record errors per folder and preserve queued sibling work and partial results.

The queue holds at most one pending child per visited entry, hence at most 10,000
folders. It opens no game containers. No new dependency or environment switch is
needed for this scan-order correction. No Wii U directory-name heuristic was
added: names such as `code`, `content` and `meta` alone could hide valid sources.
The cap can still truncate a single very wide level, and external sources still
share their original budget in registration order; this is not an exhaustive
scan guarantee for arbitrary trees.

## Host verification (macOS)

Existing assert-based test style: `tools/xbox/tests/game-library.cpp`.
Before the change, the existing library and storage-path tests passed. Adding the
20,000-file regression before changing the scanner failed at
`limited.entries.size() == 2` (exit 134).

After the change the expanded library test passes, compiled with:

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror -Isrc \
  tools/xbox/tests/game-library.cpp -o /tmp/jadyx-game-library
/tmp/jadyx-game-library
```

The temporary fixture has `a.nsp`, `b/c.xci` and 20,000 empty text files under
`deep/x/y`. An adversarial layout first puts noise beneath the first directory
returned by the filesystem, so the regression does not depend on directory
ordering. Both layouts return exactly two games and `limited == true` through
both the native scanner and a portable 64-item-page adapter exercising the same
FIFO/depth/budget implementation used by USB. Tests also check cancellation,
symlinks, existing extension/path behavior, missing roots, exactly 10,000 visits,
and all five allowed levels. The paged adapter tests shared traversal policy;
it does **not** execute WinRT or certify USB authorization on a console.

The unchanged frontend test set from `run-frontend.ps1`, plus `storage-path`, was
compiled with Clang/C++20, `JADYX_NO_WINRT`, the four locale sources and
`localization.cpp` (`cheats-page` also links `core/memory/cheat_text.cpp`):

| Result | Before | After |
| --- | --- | --- |
| Passing | 10 | 10 |
| Runtime failures | 3 | 3 |
| Build errors | 0 | 0 |

Passing: frontend-input, controller-selection, player-slots, stick-navigation,
game-keyboard, mods-page, text-entry, localization-catalogs, play-history,
storage-path. Existing failures on this host: game-menu, system-settings,
cheats-page (abort without a diagnostic). No additional failures.

The requested NXbox command was also attempted before and after:

```sh
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

Both runs report `ImportError: Start directory is not importable: 'tests/port'`:
that suite is absent in Jadyx. `git diff --check` passes. No network, Windows/UWP
build, PC app run or console run was performed. MSVC compilation and real WinRT
enumeration remain to be checked on Windows/Series.

## Exact console test (pending)

1. On an external drive, create an isolated `jadyx-scan-test` source. Place two
   existing, legally owned dumps at `jadyx-scan-test/a.nsp` and
   `jadyx-scan-test/b/c.xci`. Do not change or delete the original files.
2. Create the unrelated files on that drive with PowerShell (replace `E:` with
   its current workstation drive letter; Jadyx does not store that letter):

   ```powershell
   $noise = 'E:\jadyx-scan-test\deep\x\y'
   New-Item -ItemType Directory -Force $noise | Out-Null
   0..19999 | ForEach-Object {
       [IO.File]::WriteAllText((Join-Path $noise "$_.txt"), '')
   }
   ```

3. Build the changed UWP target, package via the repository's usual process to
   `build-uwp/package/jadyx.appx`, and install on Series in Game mode.
4. In Settings → file manager, add the `jadyx-scan-test` folder as a game source.
   Return to the library and refresh. Both shallow games must appear and remain
   selectable **while** the existing scan-limit notice is visible. Their titles
   may replace filenames after normal metadata loading. A notice with an empty
   list is a failure.
5. Close/reopen Jadyx, refresh again, and require the same two entries and notice.
   Launch each game once to verify the preserved token-relative launch paths;
   return to the library normally. No gameplay/FPS claim is part of this gate.
6. Test the native path separately with the same layout beneath the internal
   `LocalState/games` folder using isolated test data and existing file-management
   tools. Require both entries plus the notice after refresh. Remove only this
   isolated test data afterward.
7. Check the log for scan/access errors; disconnect/reconnect the USB drive and
   refresh to confirm existing missing-source handling and recovery still work.
   Remove the test source via the file manager when finished.

The console gate is documented, not executed or certified by this change.
