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
  file and retain the previous destination. `STOR` and `APPE` now require
  `ALLO <bytes>` before each upload; the announced count is the number of incoming
  bytes (for `APPE`, excluding the existing prefix). Publication requires an exact
  match after data EOF. Missing ALLO returns `503`; short/excess data returns `426`
  and deletes the partial. An empty upload requires `ALLO 0`. `SIZE <path>` only
  queries a stored file: it cannot prove how many bytes the client meant to send.
  There is no standard client acknowledgement of the server's final `226`, so
  unknown-length uploads are refused rather than guessed complete. Clients that
  do not send ALLO, including default FileZilla/ftplib upload flows, need explicit
  ALLO support; the Python example below supplies it. Download/hash verification
  is still recommended for important transfers.
- `.part` names, traversal above `/`, Windows device names/streams and reparse-point
  descendants are rejected. Virtual roots cannot be deleted or renamed. A pre-existing
  `.part` is never overwritten; an app termination/power loss can leave one for manual
  removal through Device Portal while FTP is off.
- `RNTO` replaces an existing destination only under `/games`. Under `/keys`,
  `/saves` and `/firmware`, it returns `550` until the client explicitly deletes
  the destination. The final move itself is non-replacing outside `/games`, so a
  destination appearing after the check is also protected.
- Control sessions close after **30 seconds without completing login**, or after
  **300 seconds idle after login**. Unauthenticated commands/partial lines do not
  extend the login deadline; complete authenticated commands and actual data
  progress extend the idle deadline. A stalled transfer is cancelled on expiry.
  Pending reads are checked every **250 ms**; blocked filesystem/WinRT work may
  extend observed shutdown latency. Timeouts close the connection and free the
  single session; no final control reply is required.
- Plain FTP exposes credentials and data to the local network. Use a trusted LAN,
  not an internet port forward. Failed logins delay **1, 2, 4, 8, 16, 30, 30…
  seconds per peer IP**, shared across reconnects and FTP stop/start for the same
  server instance. The 30-second login deadline takes precedence over a longer
  remaining delay; at most five failures are allowed per session. Stop cancels
  waits. State is in memory, reset on server destruction/app exit; 1024 peer
  entries are retained without eviction, and additional unknown peers receive
  the maximum delay.

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

## Unattended library testing (jx-22)

For TEST-ONLY packages, an installed-package `boot.cfg` line
`ftp_test_password=<at least 12 characters>` starts FTP after the library's initial
scan without console input. It uses user `jadyx`, port 2121, the same roots and
safety rules. No FTP page opens. The diagnostic `FTP test hook on` reports that the
hook is enabled and never includes the password. Missing/short values leave it
off; LocalState and other user-file paths are never read for this setting.
Back/Escape stops the server; suspend cancels pending/active startup and resume
leaves it off. Input mutations and background readers are held while FTP owns
storage. Test credentials belong only in a private installed test package.

See [jx-22 test hook](nxbox-jx-22-ftp-test-hook.md) for host before/after results,
credential-safe packaging commands and an exact console round-trip/negative gate.
Do not supply the credential to `package-appx.ps1 -BootCfg`, which prints its
arguments. No console/Windows validation has been performed for this hook.

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
   another larger than **10 GiB** if disk space permits. Send `ALLO <exact incoming
   bytes>` before each STOR/APPE (including retries). Require `226`, matching
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
   **overwrite** with a fresh ALLO. Compare hashes, not just client queue state.
8. While the first client remains connected, connect another: require `421`.
   Without logging in, leave it silent for 31 seconds and verify a new client can
   connect. Repeat after login with 301 seconds idle. Partial command bytes and
   unauthenticated NOOPs must not extend the 30-second deadline. Send authenticated
   NOOP before 300 seconds and confirm the idle window restarts. A progressing
   transfer lasting over five minutes must remain active; a stalled one must close.
   Try wrong passwords on successive fresh connections from the same IP: measure
   growing 1/2/4/8/16-second reply delays; at the 30-second cap expect closure at the
   login deadline if no time remains for `530`. A different IP starts at one second.
   Stop during a penalty and verify prompt shutdown. A data connection from a
   different client IP must be rejected.
9. Suspend/resume during an upload: require cancellation and FTP off after resume.
   Close/reopen the library and launch/exit homebrew: require no surviving FTP
   listener. Check the HTTP portal still starts and imports normally after FTP stops.
10. Export normal diagnostics. Confirm no password, user filenames or file contents
    appear in FTP logs and no unhandled exceptions occur. If data connections fail,
    verify inbound TCP **2121 and 50000–50009** in the Dev Mode firewall and disable
    LAN client isolation. Do not expose these ports on the internet.


## Review regression gate (jx-21)

The host-only follow-up is documented in [nxbox-jx-21-ftp-fixes.md](nxbox-jx-21-ftp-fixes.md).
In addition to the checks above, use synthetic files only for these destructive tests:

1. Create `existing.bin` containing a known marker under each exposed root and
   record its SHA-256. Send `ALLO 1048576`, then STOR that path, send only 4096 bytes
   and close the data socket cleanly without ABOR. Require `426`, the original
   SHA-256 and no owned `.part`. Repeat by dropping the control connection during
   the transfer, resetting the data connection, stopping FTP and suspending.
2. Without ALLO require `503` and no partial. With `ALLO 4096`, send 4097 bytes:
   require `426` and preservation. Send exactly 4096: require `226` and the new
   hash. Test `ALLO 0` with an empty STOR; a subsequent upload without a new ALLO
   must again return `503`. Test APPE with ALLO counting only appended bytes and
   verify the original prefix; repeat with a short append and require no change.
3. Create `source.bin` and `target.bin` in `/keys`, `/saves` and `/firmware`.
   RNFR source then RNTO target must return `550` with both hashes unchanged.
   DELE target, then repeat RNFR/RNTO: require `250`. In `/games`, replacement
   remains supported. Remove all synthetic fixtures afterward.

For a client that explicitly supplies the required byte count (run on the LAN
client, with the address/password displayed on the TV):

```python
from ftplib import FTP
from pathlib import Path

source = Path("ftp-test.bin")  # Synthetic local file; use >2 GiB for the large-file gate.
with FTP() as ftp:
    ftp.connect("192.168.1.42", 2121)  # Replace with the displayed private IPv4.
    ftp.login("jadyx", input("FTP password: "))
    ftp.voidcmd(f"ALLO {source.stat().st_size}")
    with source.open("rb") as stream:
        ftp.storbinary("STOR /games/ftp-test.bin", stream)
    assert ftp.size("/games/ftp-test.bin") == source.stat().st_size
```

Console execution, Windows/MSVC compilation and real WinRT timeout/cancellation
behavior remain pending. No device/network tests were performed for this follow-up.
