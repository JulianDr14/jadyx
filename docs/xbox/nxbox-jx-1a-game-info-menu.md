# Game information in the Menu options

## Finding and scope

Work starts at local `nxbox/integration3` (`31c2a77097`), on
`nxbox/1a-game-info-menu`. This is Jadyx, not NXbox. The existing game options
use horizontal categories, activated directly, rather than a separate vertical
context menu. Information is the first category in that existing shell.

The library already owns the title, icon, effective display version, program ID
and optional play history. The integrated compatibility catalog already selects
Jadyx reports and excludes reports about NXbox. Reuse those sources instead of
inferring compatibility from a title or successful launch. No reference-repo
files were modified, and no additional reference implementation was copied.

## Changes

- Menu opens with **More information** selected. The existing canvas draws the
  library cover/placeholder at 300 × 300 layout units on the left, with metadata
  on the right, in the existing 1280 × 720 management shell.
- All new labels use the English, Spanish and Brazilian Portuguese catalogs.
  Existing theme tokens, type roles, cover cache and compatibility badge are reused.
- Metadata includes a 16-digit program ID, effective version, uppercase file
  format, container file size, storage source/path, registered update versions
  and distinct DLC titles. Disabled registered installations still count.
  Merely opening information never adopts embedded content or enables it.
- File size and content catalog are read once on an MTA worker after the pane
  activates. Nothing reads game payloads or enumerates directories while painting.
  An unavailable size/catalog is **Unknown**, not zero. Path plus request generation
  prevent a completed request from replacing another game's or a refreshed pane's
  data. Returning from Updates/DLC starts a new snapshot.
- Size means the selected container's file length, excluding saves, caches and
  separately installed updates/DLC; it is not filesystem allocation size. IEC
  units use integer arithmetic and one truncated decimal, including 64-bit sizes.
- Internal storage shows the launch path. USB/authorized external folders show
  their source name plus relative path, without leaking the FutureAccessList token
  or pretending it is a drive letter. The label explicitly includes external
  folders because the same mechanism also authorizes non-USB folders.
- Compatibility shows the existing selected report's status, FPS note, Series
  model, Jadyx version and date. The optional schema-1 `date` string is bounded to
  64 bytes; old catalogs stay valid. Existing bundled reports omit date and often
  version, so those fields honestly show **Unknown**. No catalog facts were added.
  Games without a selected Jadyx report show **Untested**. History appears only
  when the library already tracks a nonzero value; the absolute date uses UTC.
- The information page's **Remove game** action selects the existing Remove
  category. It does not confirm removal: the same explanation, two presses,
  duplicate-copy rules and storage worker perform deletion. The action is absent
  for external dumps and unknown program IDs. B closes the options as before.
- View/Select mappings and gameplay input files are unchanged. This is additive
  read-only UI; it does not need a behavior experiment switch. The compatibility
  loader's existing refresh opt-in and networking behavior are unchanged.

## Host evidence (macOS)

No Windows/PC runtime, Xbox, network, package build or push was used.

Before implementation, the existing frontend gates compiled with host Clang,
`-std=c++20 -DJADYX_NO_WINRT -Isrc`, each test source plus `localization.cpp` and
`locales/*.cpp`; `cheats-page` also linked `core/memory/cheat_text.cpp`:
**9 passed, 3 failed**. The pre-existing aborts were `game-menu`, `system-settings`
and `cheats-page` (exit -6). The new `game-information.cpp` gate was run before
implementation and failed to compile because `game_information.h` did not exist.

After implementation:

```sh
python3 -B tools/xbox/tests/run-frontend-host.py all
```

**10 passed, the same 3 failed**; no additional failing gate. The old cheats test's
category-walking assumption now accounts for the inserted first category; its
other existing assertions/failure were left alone. The host runner mirrors the
existing portable frontend suite and adds `game-information`; binaries use a
temporary directory. The MSVC frontend runner also includes the new test.

The information test covers byte boundaries, zero, a 5 GiB file, UINT64_MAX
(`15.9 EiB`), unavailable versus zero counts, distinct DLC counting, uppercase
format, internal/external paths, untested/reported/foreign-product behavior,
report fields, optional history, all three locales, first-category navigation,
and the unchanged two-step Pulse/Remove state machine. It also passes with:

```sh
clang++ -std=c++20 -Werror=shadow -Werror=conversion -Wno-sign-conversion \
  -DJADYX_NO_WINRT -Isrc tools/xbox/tests/game-information.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp \
  -o /tmp/jadyx-game-information
/tmp/jadyx-game-information
```

Sign-conversion warnings are excluded because existing `storage_path.h` and
`utf8.h` trigger them; no new conversion or shadowing diagnostic remains.
This host check does not certify MSVC or WinRT compilation.

`localization-catalogs` passes completeness and placeholder checks. The new
Python runner passes Ruff. Changed/new C++ was run through clang-format, with
unrelated formatting edits discarded. `git diff --check` passes.

The existing `compatibility.cpp` gate was extended for a missing date, a supplied
date, wrong type and oversized date. Attempting to compile it and the production
parser on this Mac fails on the existing missing `nlohmann/json.hpp` dependency;
no dependency was downloaded. That parser gate and the UWP build remain pending.

## Exact console gate (pending)

On the Windows build host, run the existing incremental build and frontend gate:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\tests\run-frontend.ps1 -Tests game-information
powershell -ExecutionPolicy Bypass -File tools\xbox\tests\run-frontend.ps1 -Tests localization-catalogs
```

Use the project's standard manifest/version, packaging and symbol archival
workflow; deliver only `build-uwp/package/jadyx.appx`. No package was prepared here.
Install that package through Device Portal and set the app to Game mode.
Use only existing owner-provided lawful dumps; these steps require no downloads.

1. With compatibility refresh disabled, select an internal library entry and
   press Menu. Verify **More information** is first and selected, the left cover
   matches its tile, and the right title/ID/version/NSP-XCI-NRO format are correct.
   Compare size to the selected file's byte length, allowing IEC truncation.
2. Repeat with a missing icon, a long title/path, an NRO without program ID, and
   a file larger than 4 GiB. Check placeholder reuse, wrapping/ellipsis, no title
   overlap with the ID, and no overlap with the footer at both 720p and 1080p.
3. Open an authorized external/USB entry. Verify its source and relative path,
   and the absence of the information page's removal action. Disconnect the
   drive before opening once; metadata failures must stay unknown and B must work.
4. Use LB/RB to visit Updates/DLC, then return to information after an existing
   content change. Verify retained update-version counts and unique DLC-title
   counts refresh, including disabled entries; other games do not contribute.
5. Open a title with a bundled Jadyx report: compare status, FPS note and Series
   model to that report. Version/date absent in the catalog must read Unknown.
   Open an unlisted title or one with only an NXbox report: it must read Untested.
   For a dated schema-1 cache fixture at `LocalState/compat/compat.json`, restart
   the app and confirm the provided date is displayed;
   do not substitute the cache retrieval date for the report date.
6. Check a never-played entry and one with existing play history. No invented
   zero-time/history row appears for the former; the latter uses existing values.
7. On an expendable internal fixture, press A on **Remove game**. Verify it only
   opens the existing Remove pane. Press B and confirm the game still exists.
   Repeat, press A once for its warning, B to cancel, then separately test the
   existing second-confirmation deletion and duplicate-copy preservation flow.
8. Repeat the page and removal-cancellation steps in en, es and pt-BR. Check all
   seven category labels fit, controls remain readable, and B returns to the library.
9. Press View/Select in the library and compare its controller-settings behavior
   with the base build. Open gameplay and verify its existing View/Select and
   View+Menu behavior. No input mapping change is expected.

Record visual results and logs before certifying console operation. These are
pending checks, not claims of a successful UWP build or console run.
