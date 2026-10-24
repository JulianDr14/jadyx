# Jadyx FTP server

## Finding and change

The task reports Device Portal returning HTTP 500 for files over 2 GB on a Series X
(23 October 2026). That report was not reproduced here. Jadyx already has its own
pairing HTTP portal and import coordinator; FTP provides another local transport
without Device Portal's HTTP request path.

Ported the GPL-3.0-or-later NXbox `src/eden_uwp/ftp/` server and protocol tests into
`src/jadyx/storage/ftp/` and `tools/xbox/tests/ftp-protocol.cpp`. The reference checkout
was read only. The implementation uses Jadyx names, logging, library ownership,
existing CMake target and English, Spanish and Brazilian Portuguese catalogs. No
new dependencies, public services, download sources or bundled content were added.

In the library open **Settings → Files → FTP server**. It starts **off** on every
launch. The dedicated page displays `ftp://<private IPv4>:2121`, user `jadyx` and a
fresh six-digit password. The address comes from Jadyx's existing ordered adapter
discovery. Passwords and filenames are not logged; no autostart or fixed-password
file from NXbox is imported. The existing `privateNetworkClientServer` capability
is sufficient at the manifest level; actual Xbox firewall behavior remains untested.

Only these virtual roots are exposed:

| FTP directory | Jadyx destination |
| --- | --- |
| `/games` | Library root, normally `LocalState/games` |
| `/keys` | `SystemDestination(Keys)`, normally `LocalState/jadyx/keys` |
| `/firmware` | `SystemDestination(Firmware)`, normally `LocalState/jadyx/nand/system/Contents/registered` |
| `/saves` | `EdenPath::SaveDir/user/save`, normally `LocalState/jadyx/nand/user/save` |

These are raw file transfers, **not** the HTTP portal's validated system-import
workflow: upload extracted firmware NCAs, not ZIP archives. They replace individual
files, not a complete firmware installation atomically. Use the existing importer
when a version review or validated firmware replacement is needed. `/saves` exposes
account/device/cache save layouts under `user/save`; preserve the existing hierarchy
and user IDs. Temporary saves outside that directory are not exposed. Back up saves
before replacing them. Only use dumps from hardware and games you own.

The page exclusively owns storage: it stops the HTTP portal, pauses the updater,
stops/joins metadata readers and prevents new metadata/import jobs. **Back**, **Stop**,
suspend, game launch and library destruction stop FTP and join its active session.
After uploads, renames or deletions, leaving the FTP page automatically rescans the
library and reloads the key manager, system status and metadata. Rescanning is deliberately deferred
until FTP has stopped, so readers cannot inspect keys/firmware during a multi-file
upload. Reopening generates a new password; resume never restarts the server.

## Transport and limits

- Passive IPv4 FTP, control TCP **2121**, data TCP **50000–50009**, one control session
  and one transfer at a time. Private IPv4 peers only; data peer must match the
  control peer. All-interface listeners retain the source's Xbox binding workaround.
- No application file-size cap: 64-bit byte counts and a bounded **1 MiB** copy buffer
  shared by the upload, download and append algorithms. WinRT/CRT buffers are additional.
  Available disk space and filesystem limits still apply. Appending temporarily
  needs room for both the old file and its replacement.
- `STOR`, `APPE`, `RETR`, `LIST`, `NLST`, `MLSD`, `SIZE`, `MDTM`, directory creation,
  deletion and file rename are retained. Active mode and TLS are not implemented.
  `REST` supports zero only: retry with **overwrite**, not resume. Use binary mode.
- Sibling `.part` files publish only after flush/commit through the app-safe Windows
  move/replace APIs. ABOR, server stop and I/O failures remove the owned temporary
  file and retain the previous destination. An orderly data EOF is FTP completion:
  a client that simply closes data early without ABOR may produce a shorter upload.
  Verify important transfers by downloading and comparing hashes.
- `.part` names, traversal above `/`, Windows device names/streams and reparse-point
  descendants are rejected. Virtual roots cannot be deleted or renamed. A pre-existing
  `.part` is never overwritten; an app termination/power loss can leave one for manual
  removal through Device Portal while FTP is off.
- Plain FTP exposes credentials and data to the local network. Use a trusted LAN,
  not an internet port forward. Wrong login waits about two seconds and closes the
  control session after five failures.

## macOS checks (24 October 2026)

Before the port, Jadyx's standalone `storage-path`, `text-entry` and
`localization-catalogs` tests compiled and passed. The ported FTP protocol test was
added first and failed to compile because `jadyx/storage/ftp/ftp_protocol.h` did not
exist. After implementation, those same three existing tests still pass and the
new standalone CMake suite passes **2/2** with `-Wall -Wextra -Werror -Wconversion
-Wshadow` (MSVC configuration enables C4456/C4457/C4459/C4244 as errors).

```sh
cmake -S tools/xbox/tests/ftp -B /tmp/jadyx-ftp-tests
cmake --build /tmp/jadyx-ftp-tests
ctest --test-dir /tmp/jadyx-ftp-tests --output-on-failure

clang++ -std=c++20 -Wall -Wextra -Werror -Isrc \
  tools/xbox/tests/storage-path.cpp -o /tmp/jadyx-storage-path
/tmp/jadyx-storage-path
clang++ -std=c++20 -Wall -Wextra -Werror -Isrc \
  tools/xbox/tests/text-entry.cpp -o /tmp/jadyx-text-entry
/tmp/jadyx-text-entry
clang++ -std=c++20 -Wall -Wextra -Werror -Isrc \
  tools/xbox/tests/localization-catalogs.cpp src/jadyx/i18n/localization.cpp \
  src/jadyx/i18n/locales/*.cpp -o /tmp/jadyx-localization
/tmp/jadyx-localization
```

Protocol coverage includes command parsing, spaces, UTF-8 option, traversal,
Windows aliases, PASV/EPSV, timestamps and 12 GiB listing sizes. Transfer/path
coverage runs the **production** copy loop with simulated **12 GiB + 37 bytes**
in known-size and EOF modes (**12,289 / 12,290** reads), verifies root mapping,
cancellation before/during I/O, short writes, early EOF and invalid read counts.
The transfer test also passes AddressSanitizer and UBSan, and the public server
header passes an independent host syntax check. It does not allocate or write a
12 GiB fixture. This is evidence for bounded memory
and 64-bit accounting, not a measurement of disk or network throughput.

The new files are registered in `src/jadyx/CMakeLists.txt`. The portable protocol,
path mapper and transfer code compile on macOS. The WinRT server, UI and full UWP
link require Windows SDK/MSVC; they were not built here. No network, PC app or
console was used. Console gates below remain **pending**.

## Exact console gate

1. On the Windows build machine, build the existing UWP preset, then prepare the
   normal library package using the repository's current packaging workflow:

   ```powershell
   tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
   powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library
   ```

   Install `build-uwp/package/jadyx.appx` through Device Portal on a Series X|S in
   Dev Mode, with the app in **Game** mode. Start in the library, on the same LAN
   as the FTP client. Do not replace a running package's layout.
2. Open **Settings → Files → FTP server**. Confirm the page shows a reachable IPv4,
   port **2121**, username **jadyx** and six-digit password. Repeat in English,
   Spanish and pt-BR; inspect long text and controller/pointer focus. Confirm the
   server is not listening before opening this page.
3. In FileZilla Site Manager use **FTP**, **Only use plain FTP**, **Normal** login,
   the shown IPv4/2121 and credentials. Set **Passive**, **maximum simultaneous
   connections = 1**, transfer type **Binary**. Confirm exactly `/games`, `/keys`,
   `/firmware`, `/saves` at `/`. Exercise both PASV and EPSV with a supporting client.
4. Upload a synthetic file larger than **2 GiB** to `/games/ftp-large.bin`, then
   another larger than **10 GiB** if disk space permits. Require `226`, matching
   `SIZE`, download each and compare SHA-256 with its local source. Delete both
   test files afterwards. Record source sizes, hashes, peak app memory and time;
   memory must not grow proportionally to file size. These are not game files.
5. Upload an owned/homebrew dump to `/games`, keys to `/keys`, extracted firmware
   NCAs to `/firmware` and a backed-up save into its existing `/saves` hierarchy.
   Wait for `226` and an empty queue. Press **Back** or **Stop** on the TV. Confirm
   automatic library rescan, keys/firmware status refresh and the imported homebrew
   entry. Reopen FTP and verify files and a new password. Do not publish test content.
6. Create/remove a directory under `/games`; rename/delete a test file and check
   the library after closing FTP. Try `CWD /../../`, `STOR /games/../..`,
   `STOR /keys/a:stream`, `STOR /games/NUL.nsp`, and deleting/renaming `/games`:
   require `550`. Confirm LocalState logs/configuration are inaccessible.
7. Upload over a small existing test file, explicitly send **ABOR** during transfer,
   then reconnect: the original hash must remain and the partial must be absent.
   Repeat using **Stop** on the TV and by disconnecting the network. Retry using
   **overwrite**. Compare hashes, not just client queue state.
8. While the first client remains connected, connect another: require `421`.
   Try an incorrect password five times on one control connection: require `530`
   delays and closure. A data connection from a different client IP must be rejected.
9. Suspend/resume during an upload: require cancellation and FTP off after resume.
   Close/reopen the library and launch/exit homebrew: require no surviving FTP
   listener. Check the HTTP portal still starts and imports normally after FTP stops.
10. Export normal diagnostics. Confirm no password, user filenames or file contents
    appear in FTP logs and no unhandled exceptions occur. If data connections fail,
    verify inbound TCP **2121 and 50000–50009** in the Dev Mode firewall and disable
    LAN client isolation. Do not expose these ports on the internet.
