<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Jadyx beginner guide (jx-12-user-guide)

## Finding and scope

The README had a short console checklist but no beginner walkthrough, and still
required a certificate on Xbox. Its portal route also used older labels. This
work adds English, Brazilian Portuguese and Spanish guides with the same three
parts: install, import your own files, play. README retains its short checklist
and links to all three guides near the top and in the console section. Discord
gets three English pin messages with links to the full translations.

Baseline: Jadyx `main` / HEAD `0fc5c0183694d4cc91b062f00a5641315de30003`.
The existing branch `nxbox/12-user-guide` started at that exact main commit.
Only documentation changes; no runtime behaviour, dependencies or environment switches.

Model: read-only NXbox `origin/main`, commit
`7275e402e17485beca00902074725dd5911c9c5d`, `docs/guide.md` (GPL-3.0).
Adapted the three-part progression, visible success checks and beginner Device
Portal explanation, rather than copying NXbox features. No helper, FTP, automatic
USB layout discovery, NSZ conversion or second-launch firmware installation is
promised for Jadyx. The simpler-install work is mentioned only as coming soon.

## Console facts supplied by the maintainer

These are task-supplied observations from Series X on 23–24 October 2026,
not tests performed during this documentation task:

- Xbox Developer Mode does not require the `.cer`; APPX + Microsoft.VCLibs installs.
  This supersedes the older certificate requirement in `xbox_deploy.md` and the
  generated release instructions. The README now calls the certificate optional.
- Keys in the keys folder are seeded/imported by the app. Code confirms the
  internal destination is `LocalState/jadyx/keys`, not an arbitrary USB root folder.
- Developer Mode internal file storage may have only a few GB available; a full
  disk can make Device Portal uploads fail. Use USB for big games. Storage capacity
  and the Game-mode RAM allowance are separate quantities.

## Step-by-step cross-check

The translations and Discord messages describe the same operations. Line numbers
below refer to the unchanged baseline sources unless otherwise indicated.
External Xbox UI labels come from the local NXbox reference and existing Jadyx
deployment docs; they are not invented Jadyx controls. No external site was opened.

| Guide operation | Evidence |
| --- | --- |
| Prerequisites, own-console files only | `CONTRIBUTING.md:17`, `README.md:34` (baseline) |
| Install 1: APPX and x64 VCLibs filenames | `.github/workflows/build.yml:270` |
| Install 2–3: Dev Home, Remote Access, address, credentials, certificate warning | NXbox `docs/guide.md:157` at the read-only commit above; `docs/xbox/xbox_deploy.md:74` |
| Install 4: Add, Next, dependency, Start; no required .cer | NXbox `docs/guide.md:158`; `docs/xbox/xbox_deploy.md:75`; maintainer facts above explicitly supersede the certificate advice |
| Install 5: View details, App type Game, about 1 GB versus 5120 MiB | NXbox `docs/guide.md:159`; `docs/xbox/xbox_deploy.md:78`; `README.md:116` (baseline) |
| Update over existing app; uninstall deletes internal data | `README.md:628` (baseline) |
| Import 1: bumpers, Settings, File manager, portal starts on selection | `src/jadyx/library/uwp_library.cpp:1088`, `src/jadyx/library/uwp_library.cpp:1870`, `src/jadyx/library/uwp_library_canvas.cpp:1175`; labels in all three `src/jadyx/i18n/locales/{en,es,pt_br}.cpp:236` and `:252` |
| Import 2: browser pairing, local network, expired code regeneration | `src/jadyx/storage/network_portal_page.h:160`, `src/jadyx/i18n/locales/en.cpp:260`, `src/jadyx/library/uwp_library.cpp:1138` |
| Import 3–4: System, Keys/Firmware, file/ZIP selection, review, confirmation, completion | `src/jadyx/storage/network_portal_page.h:155`, `:205`, `:210`, `:214`, `:220`, `:608`, `:616`, `:624`; `src/jadyx/storage/system_import.cpp:253` |
| Import 5: Keys ready and firmware file count, not full compatibility certification | `src/jadyx/storage/uwp_file_manager.cpp:90`, `src/jadyx/i18n/locales/en.cpp:248` (same keys/lines in es and pt_br) |
| Manual keys folder and reopen | `src/common/fs/path_util.cpp:128`, `src/jadyx/app/uwp_boot.cpp:2297`, `src/core/crypto/key_manager.cpp:577`, `src/jadyx/storage/system_import.cpp:150` |
| USB folder picker, external games stay on drive, subfolders, formats and permissions | `docs/xbox/xbox_frontend.md:570`, `docs/xbox/xbox_rom_storage.md:13`, `src/jadyx/storage/uwp_rom_storage.cpp:334`, `src/jadyx/library/game_library.h:73`; media/format warning: NXbox `docs/guide.md:152` |
| Browser base NSP upload only, internal destination | `src/jadyx/storage/network_portal_page.h:179`, `:183`, `:187` |
| Manual Device Portal games folder | `docs/xbox/xbox_frontend.md:64` and `:83`; `src/jadyx/app/uwp_boot.cpp:2307` |
| Updates/DLC: Menu, category, storage before package review, confirmation, one active update | `docs/xbox/xbox_content.md:131`, `src/jadyx/library/uwp_library.cpp:1507`, `src/jadyx/i18n/locales/en.cpp:363` and `:384`; corresponding es/pt_br entries use the same Text keys |
| Play 1: library, X rescan, A launch, keep USB available | `src/jadyx/library/uwp_library.cpp:1075` and `:2117`, `src/jadyx/i18n/locales/en.cpp:307` |
| Play 2: beta, compilation stutters, per-game compatibility | `README.md:34`, `README.md:48` (baseline) |
| Play 3: hold View + Menu, Resume/Back to home, wait for return | `README.md:455` and Pulse section before console checklist (baseline); `src/jadyx/i18n/locales/en.cpp:200` and `:204` (same keys/lines in es and pt_br) |
| Series S caveat and model in reports | `README.md:62` (baseline community rows); `CONTRIBUTING.md:30` |
| Existing-keys error; ticketless rights-protected update needs matching title key | `src/jadyx/i18n/locales/en.cpp:371`, `src/core/file_sys/content_archive.cpp:54`, `src/core/file_sys/submission_package.cpp:190`, `src/core/crypto/key_manager.cpp:578` |
| Other recovery messages: incomplete firmware, unavailable USB, lack of space | `src/jadyx/i18n/locales/en.cpp:305`, `:307`, `:320`, `:323`, `:376`; full Device Portal disk: maintainer facts above |
| Logs: LocalAppData package, LocalState/jadyx/log, both filenames and report fields | `CONTRIBUTING.md:27`, `src/jadyx/app/uwp_boot.cpp:1481`, `src/common/fs/fs_paths.h:43` |

## Host checks before and after

The NXbox `tests/port` suite does not exist in this Jadyx worktree. Located Jadyx's
own tests in `tools/xbox/tests`. Windows/MSVC/AppContainer runners were not run
on macOS; the following existing offline Python/Node checks were run unchanged:

| Command | Before | After |
| --- | --- | --- |
| `python3 -B tools/xbox/tests/release.py` | PASS | PASS |
| `node tools/xbox/tests/network-portal-page.cjs` | FAIL | same FAIL |
| `node tools/xbox/tests/log-export-page.cjs` | FAIL | same FAIL |
| `node tools/xbox/tests/updater-page.cjs` | PASS | PASS |

Both existing failures are `TypeError: Cannot read properties of undefined
(reading 'classList')` in `renderUpdate` at evaluated script line 570. The
network-portal test first passes its syntax/SHA-256/fingerprint checks. No test
or product code was changed to hide the failures. Result: 2 passing scripts,
2 pre-existing failing scripts, no new failures in the selected checks.

A documentation existence assertion failed before writing: 4 missing guide files.
After writing, local checks passed: 3 guides, 3 ordered parts and 13 numbered
steps each, required filenames/facts, all relative links, and both README entry
points. The Discord blocks have **1274 / 1720 / 1543 Unicode characters**, each
also below 2000 UTF-16 code units. `git diff --check` passes. No formatter or new
dependencies were installed for this prose-only change.

## Exact console acceptance test (not executed)

Use only the tester's own console dumps and the current release. Follow the
English guide, then repeat the labelled navigation with UI language PT-BR and ES:

1. With Xbox already in Developer Mode, enable Remote Access/Device Portal,
   open the displayed address from a same-network computer, and sign in.
2. Add the Jadyx APPX and x64 VCLibs dependency, without a CER. Wait for success;
   in Dev Home > Jadyx > View details set App type to Game. Record model/version.
3. Start Jadyx > Settings > File manager > Jadyx Portal. Pair a browser using
   its address/code. Generate a new code once and verify expired codes fail.
4. Import own prod.keys and needed title.keys using System > Keys > Upload and
   review > Confirm installation. Import own firmware ZIP/NCA through Firmware
   in the same order; wait for Installation complete. Check Keys ready and the
   firmware file count. Do not accept warnings without checking their cause.
5. Add a USB folder with an owned complete NSP/XCI (and optionally own homebrew
   NRO); verify library discovery without an internal game copy. In the portal,
   upload a small owned base NSP and verify it appears after validation.
6. For an owned update/DLC, open Menu > Updates/DLC > Add content, choose USB
   storage first, select the complete package, review and confirm. Check active
   selection. If testing a rights-protected ticketless update, verify the missing
   title-key case is resolved by importing the matching own title.keys with
   prod.keys. Never send any key material in the report.
7. X rescans; A launches. Play manually, without a scripted timeout. Hold View +
   Menu, Resume, reopen menu and Back to home. Wait for the return to complete.
   Record issues separately for Series X and S; one does not certify the other.
8. Close/reopen Jadyx; verify manual keys at LocalState/jadyx/keys and manual games
   at LocalState/games are read, with no assumption of USB-root key discovery.
9. Download both LocalState/jadyx/log/jadyx_diag.txt and jadyx_log.txt via Device
   Portal. Report version/model/Game type and exact failure steps. Confirm free
   internal space if any upload fails; do not deliberately fill the disk or erase
   saves for this test.
10. On an existing installation, update over it and verify own keys, firmware,
    saves and game library remain. Do not uninstall to test preservation.

No network, push, GitHub CLI, PC runtime or console access was used for this task.
