# USB discovery and play in place

Task: jx-40-usb-play-in-place. Branch: `nxbox/40-usb-play-in-place`, based on
Jadyx `main` at `0fc5c0183694d4cc91b062f00a5641315de30003`.

## Finding and design

Jadyx already supports playing external games without copying them. Its missing
piece was automatic discovery: `ScanExternalGameFolders` read only the folders
previously selected through the picker and persisted in FutureAccessList. The
manifest had neither `removableStorage` nor game file associations, so it could
not enumerate removable drives through KnownFolders.

The existing path remains the right one:

1. Enumerate with `StorageFolder`, register the source in FutureAccessList and
   produce `jadyx-usb:/<token>/<relative file>` identities.
2. Metadata, launch-content resolution and guest boot all use
   `MakeUwpFilesystem`, not an ordinary Win32 path into `E:`.
3. `StorageGameFile` opens read-only through `CreateFile2FromAppW` (the FromApp
   family). If that fails, `StorageFile.OpenReadAsync` supplies the brokered
   random-access stream. A per-file mutex serializes seek/read; offsets and size
   are 64-bit. Reads use chunks of at most 1 MiB, including the stream scratch.
4. Preflight checks the beginning, offsets beyond 4 GiB where applicable, end
   and EOF before starting the guest. Errors use existing Jadyx I18n strings.

Reference inspected locally, without network access: NXbox's
`src/eden_uwp/usb_library.cpp` (`ListExternalGames`),
`src/common/fs/file.cpp` (FromApp handles) and
`dist/nxbox/AppxManifest.xml` (removable capability and associations), in the
read-only checkout `/Users/miguel/.local/share/nxbox/repo`. Both projects are GPL;
this change uses Jadyx's existing VFS rather than importing NXbox's FILE adapter,
folder naming, or unrelated features. Prior Jadyx research and storage gates are
in [xbox_rom_storage.md](xbox_rom_storage.md).

## Change and limits

Automatic discovery is opt-in: `NXBOX_USB_PLAY_IN_PLACE=1` in the process
environment, or the same line in the installed package's `boot.cfg`. The boot
file is read **before opening the library**, not only after a game is selected.
The default is off; `0` disables the boot-file opt-in, while an environment value
of exactly `1` still enables discovery.

On startup or library refresh, the MTA scan worker enumerates removable drives
through `KnownFolders::RemovableDevices`. Newly found roots are registered in
the existing catalog and scanned in place. No fixed drive letter, formatting,
ACL edits, copy, import or deletion is performed by discovery. File associations
expose only the already supported `.nsp`, `.xci` and `.nro` formats; compressed
containers are not added.

An existing selected folder on a drive takes precedence: discovery skips an
overlapping drive root instead of creating duplicate entries or expanding that
selection. Comparisons normalize ASCII case, separators and trailing slashes,
with component boundaries (`Games` does not overlap `Games-old`). Current
broker-resolved paths are preferred over saved path metadata, including after a
drive-letter change. Unavailable tokens remain saved for reconnection.

Sources discovered automatically remain normal persisted sources after discovery
is disabled. To remove one permanently, disable discovery, then remove the source
in Settings > File manager. With discovery enabled, a connected removed drive
will be found again. Existing picker, portal/copy/import, internal library and
updates/DLC paths remain available.

Existing bounds remain: 16 game sources, 64-item pages, 10000 visited items across
external game sources and five folder levels including the source root. Drive
enumeration also has a 10000-entry upper bound. Cancellation is checked between
broker calls and items; an outstanding WinRT call is not given a new timeout by
this change. Source-limit warnings must not truncate the scan of already saved
sources. A selected subfolder means games elsewhere on that drive require changing
that selection. Physical USB access and unexpected removal during gameplay still
need the console gate below.

## Host validation

Run offline on macOS (or a host with a C++20 compiler):

```sh
python3 -B tools/xbox/tests/usb-play-in-place.py
```

Before: the existing `storage-path`, `game-library` and `localization-catalogs`
tests all passed (3/3). New red checks failed before implementation: the discovery
policy test could not compile without its header, and the manifest assertion
failed because `removableStorage` was absent.

After: 5/5 tests pass: those same three existing tests, discovery opt-in/path
overlap policy, and manifest capability/format filtering. Policy includes absent,
invalid and exact enabled values, E/F separation, mixed case/separators, boundary
names, empty paths and Unicode folder names. The policy C++ test also passes
`-Wall -Wextra -Wconversion -Wshadow -Werror` with Apple Clang. New C++ and changed
C++ regions were formatted with clang-format; Python with offline Ruff.

These are host policy/catalog tests, not an MSVC/UWP build or physical USB test.
No network, PC runtime, console access or package build was used for this work.
No assertion about measured storage savings, gameplay, FPS or Xbox success follows
from these tests. The expected storage saving is the avoided dump-sized copy;
metadata, saves, shader caches and optional Pulse sessions still consume internal
storage.

## Exact console gate (not executed)

Build on the normal Windows toolchain, then prepare the single standard package:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -BootCfg NXBOX_USB_PLAY_IN_PLACE=1
```

Use a distinct package version for the trial according to the normal release
procedure; archive its EXE/PDB. Do not use `-Release` for this opt-in diagnostic
package and do not pass `-Game`: that option intentionally seeds an internal copy.
The output is `build-uwp\package\jadyx.appx`.

1. Prepare an Xbox-readable external NTFS or exFAT drive with a legally owned
   dump `.nsp` or `.xci` larger than 4 GiB, a small original `.nro`, and a nested
   subfolder. Do not use FAT32 for the large file. Record file sizes/hashes and
   internal LocalState size/free space before launch. Keep the USB connected.
2. Install the test package over the existing app through Device Portal, keeping
   user data, and set Game mode. Launch with no picker selection for that drive.
   Confirm its games appear automatically with titles/covers as available.
   The log must contain `ROM storage: discovered USB source=... token=...`.
   Refresh twice and confirm one entry per source/file, not duplicates.
3. Select the large game. Confirm `ROM storage: opened backend=FromApp` or
   `backend=WinRT`, a token-scoped path, the correct size and
   `ROM storage: preflight passed`. This exercises reads beyond 4 GiB.
   Play manually without a timer, then exit through the normal in-game menu.
   Check no internal dump-sized copy appeared and the external hash is unchanged.
   Do not confuse caches or a Pulse snapshot with copying the ROM.
4. Close and relaunch the app. Confirm remembered sources reopen, metadata and
   boot still work, and refresh adds no duplicate source. Repeat with a second
   external drive containing the same filename: both games must retain distinct
   token identities. Test a previously selected subfolder separately: no duplicate
   drive-root source should be registered for it.
5. Close the app, unplug a remembered drive and relaunch. Confirm a localized
   storage warning, an accessible internal library and no fallback copy. Reconnect
   and refresh; confirm the external game returns and boots. Where possible,
   repeat after the console assigns a different letter. Reauthorizing the source
   through the picker remains the recovery path if the OS cannot restore its token.
6. Separately remove USB during a read, collect the read-failure/preflight log and
   verify controlled return/error reporting. Do not certify removal recovery just
   from step 5; it exercises a different path.
7. Package without the switch (or with `NXBOX_USB_PLAY_IN_PLACE=0`), keeping the
   normal `-Library` option. Confirm an unregistered drive is not discovered,
   existing remembered folders still work, removing a source stays removed, and
   the existing internal copy/import route continues working. Repeat displayed
   error paths with English, Spanish and Brazilian Portuguese UI languages.

Record package version, filesystem, drive letter, both file sizes/hashes,
LocalState before/after, selected backend, preflight and exit results. Retain the
normal `jadyx_diag.txt` and `jadyx_log.txt`. Report partial failures separately;
only real-console results can close this gate.
