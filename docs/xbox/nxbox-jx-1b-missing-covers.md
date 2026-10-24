<!-- SPDX-FileCopyrightText: Copyright 2026 Jadyx Project -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Library titles and covers: jx-1b-missing-covers

## Finding

Task branch: `nxbox/1b-missing-covers`, based on local `nxbox/integration3`
(`31c2a7709735e3a6bf086f56856880fbc7e533b4`). The supplied worktree initially
pointed at main; the patch was moved onto the task's explicit integration base.

`ReadLibraryMetadata` stored a record even when `ReadTitle` or `ReadIcon` failed.
Version 5 accepted a zero-length title/icon, including names-only cache hits.
External brokered paths have stamp 0, so an unchanged catalog/language could keep
that negative result indefinitely. `KeepKnownTitles` also preserved failed reads
across an in-session rescan. The catalog's per-application stamp itself is correct:
there is no need to invalidate every game's cache or increment its format version.

`AppLoader_NSP` looks for an Application/Control pair. A standalone update can
have a Control NCA under its update identity, and a DLC can have neither a Program
nor a Control NCA. Loader failure alone therefore does not establish that the
package has no usable presentation metadata. The library had no base-cover lookup.

The background fill ran, but discarded its icon before publication; the UI merged
icons only from visible-cover requests. A visible placeholder could consequently
remain until another page/scan even after background work completed.

`StorageGameFile` already reports failed/short brokered and direct reads through
`ConsumeStorageReadFailure`. No change to its I/O backend is required. In
particular, a partially returned JPEG must not become a successful persistent hit.

## Change

- Reject empty titles/icons on cache load and store, including old version-5
  negative records and names-only loads. Keep complete version-5 records valid.
- Track incomplete attempts in memory and exclude them from `KeepKnownTitles`.
  They retry after a rescan, without a user cache clear. Missing keys are also an
  incomplete attempt, rather than a permanent title result.
- For missing package presentation, use Jadyx's existing container content provider
  and `PatchManager::ParseControlNCA` to read NACP/icon data from Control NCAs.
  Recover accessory identity from parsed container metadata, never filenames. Do
  not choose a donor for an unidentified package spanning multiple applications.
- If an accessory still lacks a title or icon, borrow name, developer and cover
  from a complete, current, same-language cached base application. Match with
  `FileSys::GetBaseTitleID` (`id & 0xFFFFFFFFFFFFE000`). Preserve the accessory's
  program ID, build ID and display version; presentation inheritance does not make
  an update or DLC bootable. A missing base remains a logged placeholder.
- After the initial fill, make one bounded retry pass. This handles DLC sorted
  before its base on a cold scan. Persistent failures wait for the next scan;
  there is no endless retry loop. Cancellation resumes unread entries.
- Publish background icons to tiles still visible and invalidate decoded canvas
  covers when their bytes change. Names-only hits retain visible artwork, and
  off-screen icon bytes are released with the batch.
- On exceptions or reported storage read failure, discard the icon and skip the
  persistent record. Log one warning per incomplete attempt, carrying path,
  program ID and reason (missing header key, loader unavailable, numeric
  title/icon statuses, storage short read, HRESULT or exception). A failed entry
  can log again during the bounded retry and the next scan.

These are metadata correctness fixes, enabled normally. No gameplay, content
installation, key discovery, downloading or memory-budget policy changes. All
new reads run in the existing single metadata worker. The base index is built
lazily from text-only cache records; only the selected donor's icon is loaded.
Each background fill retains at most six icons, each capped at 1 MiB, until merged.
A library with deliberately iconless homebrew will retry those entries too.

Reference inspected read-only: NXbox's
`src/frontend_common/library_content.h` (GPL-3.0-or-later), which classifies
accessories from metadata and masks title IDs. This implementation uses Jadyx's
existing platform helper, providers, language selection and cache. No assets or
external code dependencies were imported.

## Host validation

Before the fix, the existing `library-cache` and `game-library` harnesses passed.
The added negative-cache regression then aborted with exit 134 against the old
implementation: it accepted storing an iconless record. The new policy harness
initially could not compile because the new policy interface did not exist.

After moving onto the specified integration base, its unmodified cache, scan and
storage-path harnesses were also rebuilt from `git show HEAD:<path>` snapshots:
3/3 passed. After the fix, those three plus `library-metadata` pass: 4/4, no new
failures. The metadata harness checks all 8192 low-bit values in both the matching
and adjacent application ranges, three update/DLC inheritance cases, unrelated
and unknown identities, incomplete donors, preserved version/build identity,
rescan retry, background tile refresh, names-only preservation, off-screen icon
retention and cancelled results. The cache harness also checks legacy negative
records and a successful replacement on a later scan. No game-derived fixtures.

Commands from the worktree root (macOS, Apple clang++, C++20):

```sh
mkdir -p /tmp/jadyx-missing-covers-tests
clang++ -std=c++20 -DJADYX_NO_WINRT -Isrc tools/xbox/tests/library-cache.cpp src/jadyx/library/library_cache.cpp -o /tmp/jadyx-missing-covers-tests/library-cache
/tmp/jadyx-missing-covers-tests/library-cache
clang++ -std=c++20 -Werror=shadow -Werror=conversion -Isrc tools/xbox/tests/library-metadata.cpp -o /tmp/jadyx-missing-covers-tests/library-metadata
/tmp/jadyx-missing-covers-tests/library-metadata
clang++ -std=c++20 -Isrc tools/xbox/tests/game-library.cpp -o /tmp/jadyx-missing-covers-tests/game-library
/tmp/jadyx-missing-covers-tests/game-library
clang++ -std=c++20 -Isrc tools/xbox/tests/storage-path.cpp -o /tmp/jadyx-missing-covers-tests/storage-path
/tmp/jadyx-missing-covers-tests/storage-path
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
git diff --check
```

The requested NXbox unittest command fails identically before/after:
`ImportError: Start directory is not importable: 'tests/port'`. That directory is
absent in Jadyx; its reported NXbox failure counts do not apply here. Existing
Windows-only harnesses, MSVC/UWP compilation and console behavior are untested.
No network, PC AppContainer or console was used. No scan-speed claim is made.

## Exact console test (pending, to be performed by the developer)

1. Build/package this branch with the documented UWP toolchain. Install the normal
   library package on a Series X in Dev Mode, Game mode. Preserve existing user
   data and use only the tester's own legally dumped files and keys.
2. Authorize a USB test folder containing one base NSP/XCI, its separate update,
   its separate DLC, a second unrelated base, and an NRO. Arrange filenames so an
   accessory sorts before its base. Clear the library metadata cache once from
   Settings to exercise the cold path, then open Library.
3. Stay on a page containing the accessory and base through the initial fill and
   bounded retry. The base should acquire its real NACP title/icon. An accessory
   with no presentation of its own should acquire the matching base's title/icon
   without leaving the page. The unrelated base must never donate its cover.
   Navigate away/back during work; unread cancelled entries must resume.
4. Restart without clearing cache. Complete entries should use cached titles and
   covers. Change game language and verify titles are reread in that language.
   Import/select an owned update and verify the changed application's content
   stamp refreshes its metadata without forcing other games to be reread.
5. To exercise failure recovery, clear metadata once and unplug the test USB
   while its metadata is being read. Reconnect, restore folder authorization if
   needed, and rescan without clearing metadata. Previously failed entries must
   retry and recover their covers/titles. Repeat after making the tester's own
   required keys available through the normal import flow if the first attempt
   had no usable keys. No file should be cached as permanently iconless.
6. Test an accessory without its base present and an intentionally unreadable test
   file. They should remain usable library placeholders, with a bounded warning
   per attempt explaining the reason; they must not continually reread at idle.
   Add the owned base and rescan: its accessory should recover after background
   fill. A standalone accessory must retain its existing nonbootable behavior.
7. Save `jadyx_log.txt` and `jadyx_diag.txt`. Check each remaining missing cover for
   a `Library metadata: no complete cover/title ... reason=...` line, check for
   storage read failures during unplug, and confirm no new frontend exception or
   crash. Record time to visible covers, counts of recovered/missing entries and
   whether recovery happened without navigation. These numbers are not available
   from host policy tests.
