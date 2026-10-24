# Breadth-first scans on integration5 (jx-43)

## Finding and base

The recursive internal and brokered scans can consume 10,000 visits in a deep
noise subtree before enumerating shallow games. The regression from Jadyx commit
`928018bd2e` reproduces this on macOS: `limited.entries.size() == 2` fails
(exit 134) against integration5's native scanner.

This port starts at the locally available `mine/nxbox/integration5`,
`2262038a46cce146e4f70811edacad09269c1974`, on
`nxbox/43-scan-bfs-on-int5`. No fetch was performed because the task prohibits
network access. The original [jx-42 report](nxbox-scan-breadth-first.md) is retained
as historical evidence; its shared-external-budget description does not apply to
integration5.

## Change and preserved behavior

Port the FIFO `ScanLibraryFolders` helper from `928018bd2e` and use it in both
`ScanGameLibrary` and the WinRT `ScanFolder` adapter. Enumerate a folder completely
before its children, and complete shallower levels before deeper levels within
each source. Keep games found before the budget expires, alongside `limited`.

- Internal entries retain sorted relative paths, NSP/XCI/NRO extension matching,
  cancellation, symlink exclusion and five levels (root entry depth 0 through 4).
- Each external source still has its own `StorageScanBudget`. Pass its `visited`
  and `limited` fields to the helper and propagate the latter to the partial scan.
  A failed or capped source still cannot starve later sources.
- Drive-root sources still descend only when `ShouldDescendUsbFolder(child)`
  accepts the relative name. The helper's depth cap cannot broaden that allowlist.
  Explicitly selected folders retain the depth < 4 descent rule.
- Keep `VerifiedMove::IsMoveStaging` exclusion, relative-path validation, 64-item
  WinRT pages, token-scoped launch identities, source names and creation dates.
- Catch WinRT enumeration errors inside each queued folder, preserving queued
  sibling work. Keep source resolution errors, cleanup and partial-result merging.
- Leave source discovery, registered-source overlap checks, registration order,
  the 16-source cap and opt-in `NXBOX_USB_PLAY_IN_PLACE` behavior unchanged.
- Preserve integration5's `LibraryEntry::metadata_retry` and title-reuse policy.

The queue is bounded by the existing 10,000 visited entries per source. Exactly
10,000 actual entries is complete; discovering an additional entry marks the scan
limited. A wide shallow level can still exhaust the budget. This is a safe order
correction, enabled without a new switch; it does not expand scan scope or open
containers. No new dependency, UI text or package version change is needed.

## Host verification (macOS)

Before: existing `game-library`, `usb-discovery` and `storage-path` tests pass.
After: all three pass, with the expanded `game-library` covering both native and
portable 64-item-page scans. The 20,000-noise-file fixture fails before the fix
and returns exactly two shallow games plus `limited == true` afterward.

The imported tests cover adversarial filesystem order, the exact reported
`a.nsp`, `b/c.xci`, `deep/x/y/*` layout, cancellation, links, extensions, missing
roots, the exact 10,000 cap and all five levels. Added integration checks cover
whole-drive versus selected-folder descent (3 versus 5 entries), interrupted
move staging (no extra entries), independent sources (2 capped + 3 later = 5
retained entries), and continuation past a failed folder adapter.

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion \
  -Wno-sign-conversion -Werror -Isrc tools/xbox/tests/game-library.cpp \
  -o /tmp/jx43-game-library
/tmp/jx43-game-library
for test in usb-discovery storage-path; do
  clang++ -std=c++20 -Isrc "tools/xbox/tests/$test.cpp" -o "/tmp/jx43-$test"
  "/tmp/jx43-$test"
done
```

`-Wno-sign-conversion` exempts an existing signed-char conversion in
`storage_path.h:47`, pulled in by the existing move-staging helper. No production
warning suppression was added. New entries use zero initialization and field
assignment; queue aggregates use designated fields. New locals were reviewed
for MSVC shadowing/narrowing errors. MSVC/UWP compilation remains pending.

All 21 frontend tests listed by `run-frontend.ps1` were also attempted before
and after, with `clang++ -std=c++20 -DJADYX_NO_WINRT -Isrc`, linking
`localization.cpp`, the four locale sources and, for `cheats-page`,
`core/memory/cheat_text.cpp`. All 21 are blocked in both runs by pre-existing
consteval catalog errors in `en.cpp`, `es.cpp` and `pt_br.cpp` (duplicate text IDs).
There are no new build failures in this set. The three standalone tests above
are run without the unrelated locale sources.

The requested `python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`
was attempted before and after: both report that `tests/port` is not importable;
that NXbox suite is absent in Jadyx. `git diff --check` passes. No network,
Windows PC app run, UWP build or console run was performed.

## Exact console test (pending)

1. Build the UWP target and package through the normal repository workflow into
   `build-uwp/package/jadyx.appx`. Install on Series in Game mode. Use only owned
   dumps or homebrew; keep the originals intact.
2. Prepare an isolated USB source `jadyx-scan-test` with `a.nsp`, `b/c.xci`, and
   20,000 empty text files in `deep/x/y`. On the workstation, substitute the
   actual drive letter in this PowerShell command:

   ```powershell
   $noise = 'E:\jadyx-scan-test\deep\x\y'
   New-Item -ItemType Directory -Force $noise | Out-Null
   0..19999 | ForEach-Object {
       [IO.File]::WriteAllText((Join-Path $noise "$_.txt"), '')
   }
   ```

3. Explicitly register `jadyx-scan-test` in Settings > file manager. Refresh the
   library: both games must appear while the scan-limit notice is visible.
   Reopen and refresh; both must remain selectable and launch through their
   token-relative paths. Also register a separate small source: its games must
   appear even when the first source hits the cap. Disconnect the first drive:
   the second source must still contribute; reconnect and verify recovery.
4. Separately test a whole-drive source, with USB discovery enabled through the
   existing opt-in setting or `NXBOX_USB_PLAY_IN_PLACE=1`. Put test files at the
   root, `games`, `NXbox/games`, `other/sub` and `games/title`. Only root,
   `games` and `NXbox/games` files should appear. Explicitly select a deeper
   folder and verify its games appear through entry depth 4, never depth 5.
5. With both a remembered whole-drive source and a selected subfolder on that
   drive, verify the existing overlap rule skips the root and produces no
   duplicate entries. Use an isolated `.jadyx-move-*` file and folder to verify
   staging never appears as a game; existing cleanup may remove that staging.
6. Repeat the noisy layout beneath internal `LocalState/games` using isolated
   data. Require both shallow games plus the limit notice after refresh. Check
   scan diagnostics, per-source visited counts <= 10,000 and later-source logs.
   Remove only isolated test data and registrations afterward.

The portable adapter exercises shared traversal and existing policies, not
WinRT authorization or hardware behavior. This console gate is not certified.
