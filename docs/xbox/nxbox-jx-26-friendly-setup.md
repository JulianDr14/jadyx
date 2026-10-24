# Friendly first install and first run

Task: `jx-26-friendly-setup`. Branch: `nxbox/26-friendly-setup`.
Base: Jadyx `main`, `0fc5c0183694d4cc91b062f00a5641315de30003`.
This is Jadyx, not NXbox. No network, push, Windows PC or Xbox connection was used.
The reference NXbox checkout was read only. Its current checkout has no
`NXBOX_SETUP_CHECKLIST` implementation or `docs/nxbox/architecture.md`; no code was copied.

## Finding and change

The existing frontend already has folder/ZIP import, transactional system-file publication,
three language catalogs and resumable portal uploads. The general NSP validator returned
`base` for updates and DLC, offering no useful destination. Native write errors collapsed
into `disk`; HTTP upload errors dropped the byte requirements. System validation reported
counts without the current filename. README required installing the certificate.

- The library checks the real Xbox `AppMemoryUsageLimit`, independently of the PC Job limit.
  A known limit below **5120 MiB** displays a persistent Game-mode card. Unknown/zero limits
  do not diagnose App mode. The boot diagnostic records the limit and whether it is below
  the Game budget. No memory allocation policy changes.
- The checklist appears by default only when usable keys or firmware files are missing.
  `NXBOX_SETUP_CHECKLIST=1` explicitly requests it even when configured. It shows keys,
  firmware and games in that order, with found/missing/needs-attention states, USB/portal/
  manual-copy guidance and a one-sentence explanation of `title.keys`. Readiness probing
  runs in the existing stopped-guest storage worker; status reaches the UI after future
  synchronization. It does not replace the normal library for configured users.
- Firmware versions compare numerically as five fields (major/minor/micro/revisions).
  Key generations are compared separately. An older-keys diagnosis requires both a failed
  NCA decrypt and evidence that available key generations precede the required generation.
  Working generated/console keys remain accepted. The review names a known firmware
  version; when decryption prevents reading SystemVersion, the error says “selected
  firmware” and explicitly says the version is unreadable. A wrong key is not automatically
  described as an old key.
- System import progress names the ZIP/NCA currently being copied, extracted or inspected.
  After **60 seconds** without progress, local USB import and the web portal display that
  name. ZIP extraction updates progress on each **1 MiB** block, so a large active entry
  does not look idle. A warning clears when progress resumes. This is diagnostic only;
  no watchdog cancels the import or changes its confirmation/recovery protocol.
- Win32 `ERROR_DISK_FULL` (112), `ERROR_HANDLE_DISK_FULL` (39), their HRESULTs
  (`0x80070070`, `0x80070027`) and filesystem ENOSPC are recognized in the changed
  upload/system-import write paths. Portal 507 responses carry `free_bytes` and
  `required_bytes`; snapshots carry the same information for failures after validation.
  All three languages show decimal GB free/needed. Upload preflight retains the existing
  **64 MiB** reserve and the second copy needed for content installation. ZIP estimates
  cover the current entry; they cannot predict all following uncompressed entries.
  Other I/O errors retain their original generic diagnosis. This does not retrofit every
  unrelated save/cache/log writer in the emulator.
- A general upload whose decrypted CNMT identifies an update or DLC returns `update_route`
  or `dlc_route`, with **Library > your game > Updates/DLC** instructions. This deliberately
  uses the task's “tell exactly where” option: no automatic installation or relaxed
  validation, and no filename heuristics. Mixed/unsupported packages remain rejected.
  The pending general upload must be discarded before resending in the indicated page.
- The portal names receiving/validating/adding phases and shows received/total bytes next
  to the phase. System validation also shows processed file counts and current filename.
  Received bytes measure upload progress, not bytes decrypted by the validator.
- English README and matching Brazilian Portuguese/Spanish beginner guides separate Xbox
  Device Portal installation from Jadyx Portal imports. `Jadyx.cer` is optional based on
  the owner's measured Series X deployment; the certificate asset is retained.

All app text uses the existing en/es/pt_br catalogs; portal text uses its existing parallel
three-language dictionaries. No dependencies, renderer behavior, content ownership rules,
keys, firmware, game data or payment/download features were added. No package was produced.

## Host evidence

The new C++ test was created and run before implementing its helper: it failed to compile
because `friendly_setup.h` did not exist. The implemented tests now pass. Host checks use
synthetic numeric IDs and filenames; no real game, key or firmware material is fixtures.

| Check | Before | After |
| --- | --- | --- |
| Existing localization catalogs/negotiation/plurals/placeholders | PASS | PASS |
| Existing storage path validation | PASS | PASS |
| Existing GC trace fixtures | PASS | PASS |
| Existing release script fixtures | PASS | PASS |
| Existing full portal-page mock | FAIL: missing mock `document.body.classList` in `renderUpdate` | Same failure; no new failure observed |
| Requested `tests/port` discovery | Directory absent / ImportError | Same; this is not the NXbox test tree |
| New friendly setup C++ test | Missing helper compile failure | PASS |
| New friendly portal test | New test | PASS: en/es/pt-BR, 507 counts, routing text, current file, 59/60-second boundary and resumed progress |
| Existing guided updater-page test | Not separately run in baseline | PASS |

The new C++ test translation unit also compiles with `-Wconversion -Wshadow -Werror`.
The linked host test uses `-Wall -Wextra -Wshadow -Werror`; enabling conversion errors for
all existing localization sources encounters pre-existing signedness conversions in `utf8.h`.
New C++ files were formatted with clang-format; changed C++ additions were also passed
through the formatter without reformatting whole existing files. The new JavaScript test
uses the locally installed Prettier. `git diff --check` passes.

Reproduce from the repository root, offline, on macOS:

```sh
mkdir -p /tmp/jadyx-friendly-tests
c++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror -Isrc \
  -c tools/xbox/tests/friendly-setup.cpp -o /tmp/jadyx-friendly-tests/friendly-setup.o
c++ -std=c++20 -Wall -Wextra -Wshadow -Werror -Isrc \
  tools/xbox/tests/friendly-setup.cpp src/jadyx/i18n/localization.cpp \
  src/jadyx/i18n/locales/*.cpp -o /tmp/jadyx-friendly-tests/friendly-setup
/tmp/jadyx-friendly-tests/friendly-setup
c++ -std=c++20 -Isrc tools/xbox/tests/localization-catalogs.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp \
  -o /tmp/jadyx-friendly-tests/localization
/tmp/jadyx-friendly-tests/localization
c++ -std=c++20 -Isrc tools/xbox/tests/storage-path.cpp \
  -o /tmp/jadyx-friendly-tests/storage-path
/tmp/jadyx-friendly-tests/storage-path
python3 -B tools/xbox/tests/gc-trace.py
python3 -B tools/xbox/tests/release.py
node tools/xbox/tests/friendly-setup-page.cjs
node tools/xbox/tests/updater-page.cjs
node tools/xbox/tests/network-portal-page.cjs
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

## Exact console test — pending

First build the UWP target with the normal MSVC warnings-as-errors settings and run the
existing Windows native portal/import gates. Neither was possible on this Mac. Use the
normal `build-uwp/package/jadyx.appx` delivery layout; this branch supplies no new package.

Use a spare/test installation with only the tester's own dumps. Preserve the existing
installation and data; do not uninstall it to manufacture a clean setup.

1. Install the package and its VCLibs dependency through Xbox Device Portal without
   installing `Jadyx.cer`; record the console model and successful package launch.
2. Set Jadyx to App type **App**, start it and photograph the persistent warning. Save
   `jadyx_diag.txt`; verify `setup: platform_limit_mib=... below_game_budget=yes`.
3. Close Jadyx, follow **Dev Home > Jadyx > View > View details > App type > Game**, reopen
   it and verify the warning disappears and the diagnostic reports the Game budget.
   Repeat changing the type through **Device Portal > Apps > Jadyx > App type** to verify
   those UI labels on the installed Xbox OS; they have not been visually checked here.
4. In a test data set with keys and firmware absent, verify the keys → firmware → games
   checklist in English, Spanish and Brazilian Portuguese. Verify the controller can
   still reach Settings and that the text is not clipped at the console's display scale.
5. Import the tester's keys from a USB folder, then firmware from a separate USB folder
   containing a ZIP. Repeat with extracted NCAs and through Jadyx Portal. Verify found
   statuses, review/confirmation, preserved old data on cancellation, and the normal
   library after setup. Check `NXBOX_SETUP_CHECKLIST=1` in a test launch if available.
6. With the tester's genuinely older key dump, attempt their newer firmware. Verify the
   older-keys guidance appears only when generations establish it. Confirm that an
   unreadable version is not invented. Repeat a key downgrade review against readable
   installed firmware and verify the actual version appears in the warning.
7. During a firmware copy/validation, pause the test USB source for at least 65 seconds
   using a controllable source/test device. Verify the actual current ZIP/NCA filename
   appears after 60 seconds; restore the source and verify the warning clears. Repeat
   with web validation and check `/api/v1/status` fields `system.current_file` and
   `system.no_progress_seconds`. Do not claim this gate from waiting on a review screen.
8. With a bounded disposable storage allocation, leave less free than a known test
   upload requires. Verify the preflight returns HTTP 507 and reports decimal GB free
   and needed. Repeat a race where free space is consumed after preflight, exercising
   a write/flush disk-full result. Verify confirmed offsets remain resumable and an
   existing installed firmware is preserved. Remove the disposable allocation afterward.
9. Upload a base NSP through Games. Observe receiving, validating and adding with byte
   counts. Upload the tester's update NSP and DLC NSP through Games; verify the matching
   Updates/DLC guidance, no installation/publication as a base game, and no overwrite.
   Discard each pending general upload, resend under Library > matching game > Updates
   or DLC, and verify the existing content manager installs it correctly.
10. Restart with an already configured data set and no checklist switch. Verify the normal
    library and unchanged game selection/import controls. No gameplay performance claim
    follows from these setup tests.

Not verified here: UWP/MSVC compilation, native WinRT/file I/O and actual disk-full codes,
USB failure/recovery timing, real package CNMT/decryption, console layout/controller flow,
Game-mode OS menu labels, certificate deployment and successful real content installation.
