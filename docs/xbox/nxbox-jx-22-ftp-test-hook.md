# Installed-package FTP test hook (jx-22)

## Finding and change

Device Portal's virtual gamepad cannot operate the library. FTP previously required
opening Settings → Files manually, which blocked unattended console protocol tests.

The library now accepts `ftp_test_password=<password>` from **only**
`Package::Current().InstalledLocation()/boot.cfg`. A missing file/key, empty value
or value shorter than 12 bytes leaves the hook off. CRLF is accepted; control
characters (including embedded NUL) are rejected. No LocalState, user configuration,
environment or fallback path is searched. The portable parser never logs its input.
The boot diagnostics emit `FTP test hook on` once per enabled library entry; this
means the hook is configured, not proof that binding succeeded. Normal server logs
report start failure or listening on port 2121 without credentials.

After initial seeding/scanning completes, the hook starts the existing server with
the supplied password, user `jadyx`, control port 2121 and passive ports 50000–50009.
Manual and automatic start share roots and storage acquisition: stop the portal,
pause the updater, stop/join metadata readers, and defer storage jobs/new readers.
No FTP page is opened, no UI or translated strings are added. Pointer/navigation
mutations and `library_pick` are held while the hook owns storage; Back/Escape stops
it and refreshes changed files. Suspend cancels even a pending start, and resume
does not restart FTP. Library destruction still joins/stops the server.

All existing private-peer, authentication/backoff, timeout, path/reparse protection,
ALLO/exact-length publication, replacement and cancellation rules are unchanged.
Removing the installed-package line restores ordinary manual FTP behavior. This is
an explicit test-package option, never a release default; do not distribute a
package containing a test credential. No credential is committed in this change.

## Host evidence (macOS, 24 October 2026)

- Started with the requested `git reset --hard nxbox/integration` on
  `nxbox/22-ftp-test-hook` (base `3c62b4ffff`). No remote operations.
- Before: existing standalone FTP suite **3/3 passed**. Existing storage-path,
  text-entry and localization-catalogs checks **3/3 passed**.
- Red gate: added `ftp-test-hook.cpp` before implementation; build failed on the
  missing production parser header.
- After: FTP suite **4/4 passed**, including 11 parser assertions covering absent,
  empty, 11-byte rejected/12-byte accepted, LF/CRLF, embedded `=`, comments,
  leading whitespace, tab and embedded NUL. The same three other checks pass.
- FTP targets use `-Wall -Wextra -Werror -Wconversion -Wshadow` on macOS; their
  existing MSVC configuration treats C4456/C4457/C4459/C4244 as errors.
- `git diff --check` passes. No dependencies, network traffic, PC app, console or
  Windows build used. UWP lifecycle/binding and MSVC compilation remain pending.
  This Jadyx checkout has no `tests/port` suite; its own standalone tests were used.

Reproduce the FTP host checks:

```sh
cmake -S tools/xbox/tests/ftp -B /tmp/jadyx-jx22-ftp-tests
cmake --build /tmp/jadyx-jx22-ftp-tests
ctest --test-dir /tmp/jadyx-jx22-ftp-tests --output-on-failure
```

## Exact console test (pending; not executed here)

1. On the Windows build machine, close any app using the layout, increment the
   delivery version, build and stage the normal library package:

   ```powershell
   tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
   powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library
   ```

2. In a Windows SDK Developer PowerShell, write a throwaway password without
   echoing it, repack and sign with the existing Jadyx certificate. Do not pass the
   password via `-BootCfg`: that packaging option prints each line. These commands
   modify the **package layout**, not LocalState. No new certificate is created.

   ```powershell
   $secret = Read-Host 'Test FTP password (at least 12 characters)' -AsSecureString
   $credential = [System.Net.NetworkCredential]::new('', $secret)
   $password = $credential.Password
   if ($password.Length -lt 12 -or $password -match '[\x00-\x1f\x7f]') { throw 'Invalid test password' }
   $layout = (Resolve-Path 'build-uwp/package/layout').Path
   [IO.File]::WriteAllText((Join-Path $layout 'boot.cfg'), "library=1`nplay=1`nftp_test_password=$password`n", [Text.UTF8Encoding]::new($false))
   $password = $null; $credential = $null; $secret = $null
   MakeAppx.exe pack /o /d $layout /p build-uwp/package/jadyx.appx
   if ($LASTEXITCODE) { throw 'MakeAppx failed' }
   $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=JadyxDev' -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
   if (-not $cert) { throw 'Existing Jadyx certificate required' }
   SignTool.exe sign /fd SHA256 /sha1 $cert.Thumbprint build-uwp/package/jadyx.appx
   if ($LASTEXITCODE) { throw 'SignTool failed' }
   ```

3. Install `build-uwp/package/jadyx.appx` through Device Portal, in Game mode.
   Launch the library and leave it foreground without any gamepad input. After
   scanning, require one `FTP test hook on` diagnostic and the ordinary FTP
   listening log. There must be no FTP screen and no password in diagnostics.
4. From a trusted LAN client, run this synthetic round-trip (replace the address):

   ```python
   from ftplib import FTP
   from getpass import getpass
   from io import BytesIO

   payload = bytes(range(256)) * 4096  # 1 MiB, no game-derived data
   with FTP() as ftp:
       ftp.connect('192.168.1.42', 2121)
       ftp.login('jadyx', getpass('Test FTP password: '))
       roots = {name.rstrip('/').rsplit('/', 1)[-1] for name in ftp.nlst('/')}
       assert roots == {'games', 'keys', 'firmware', 'saves'}
       ftp.voidcmd(f'ALLO {len(payload)}')
       ftp.storbinary('STOR /games/jx22-test.bin', BytesIO(payload))
       assert ftp.size('/games/jx22-test.bin') == len(payload)
       received = bytearray()
       ftp.retrbinary('RETR /games/jx22-test.bin', received.extend)
       assert received == payload
       ftp.delete('/games/jx22-test.bin')
   ```

5. Run the existing FTP safety gate in `nxbox-ftp-server.md` with synthetic fixtures:
   short/missing ALLO, traversal, replacement protection, timeout, ABOR, second
   client and private-peer checks must remain unchanged. Back must stop FTP and
   refresh changed data; suspend/resume must leave it off until a fresh library
   entry. Closing the app must leave no listener.
6. Build/install fresh test versions with the line absent and with an 11-character
   password. Neither may autostart FTP or emit the hook diagnostic. A copy of a
   valid line in LocalState/boot.cfg must not enable it. Manual Settings FTP must
   still work with its generated password. Remove that LocalState test fixture.
7. Remove the credential from the layout, rebuild a normal package and install it
   with a fresh version. Delete private test APPX copies; do not publish them.
