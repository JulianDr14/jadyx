<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Read-only on-console Files browser

24 October 2026. Branch `nxbox/20-files-browser`, based on local
`nxbox/integration3` (`31c2a77097`), itself descended from `main`. No remote
operations, Windows PC execution, Xbox access, UWP build or packaging were performed.

## Finding and implementation

The feature comparison on local `nxbox/0e-feature-diff`, row I13 and rank 1 of
“Top 10 remaining ports”, identifies a real gap: Jadyx's file manager configures
imports and sources but does not inspect arbitrary files. Acceptance requires
roots, paging, empty/error/permission states and USB disconnect handling.

Read-only reference: NXbox local `rc/next` (`1087264c852c68731a436aca0cb36c849bff734c`),
`src/eden_uwp/file_manager.{h,cpp}`, `files_policy.h`, and the architecture and
maintenance maps. The port adopts the rooted browsing/page/error concepts, with
an independent Jadyx implementation. It does not import NXbox's file-operation,
FTP, remote-source, download or transfer implementation.

Settings → File manager → **Browse files** opens a dedicated canvas/painter page.
It uses Jadyx `Palette`, `TypeRole`, theme tokens, hit targets and EN/ES/PT-BR
catalogs. It shows names, folder indicators and exact file sizes in bytes. Files
are metadata only; confirm enters folders and does nothing to files.

- Roots are the app's **LocalState**, the existing FutureAccessList game/content
  folders, and already-discovered session roots. USB access uses Jadyx's broker
  tokens, including sources without drive-letter access. A disconnected remembered
  source remains visible and can be retried after reconnection. The root page
  explains using the existing Add games folder flow to authorize USB access.
- Each directory query is shallow, globally sorted by broker
  `System.ItemNameDisplay`, with 64 entries plus one lookahead for Next. Eight
  rows are visible at once; Up/Down traverses the current page, Left/Right changes
  broker pages. Roots use the existing bounded source catalog, with row scrolling.
  Sorting is by name, not a new user-selectable sorting preference.
- One MTA `jthread` reads a snapshot of the requested location. The UI polls its
  future; no blocking WinRT `.get()` is added to the UI thread. Pending async
  operations share a five-second page deadline and are polled every 10 ms; timeout
  or cancellation cancels the operation and reports an unavailable folder.
- Busy, empty, unavailable and access-denied states have translated recovery text.
  Failed pages discard partial/stale entries. A USB catalog failure preserves the
  internal root and shows a warning. Back during loading closes Files and requests
  cancellation. A new request cannot replace an outstanding worker; late results
  are discarded when Files is closed.
- Relative child components reject traversal, slashes, ADS, backslashes and NUL.
  Each directory resolves from LocalState or its original authorized broker root.
  Reparse directories detected through available FromApp attributes are refused;
  the broker remains responsible for authorization. There is no arbitrary path
  editor, file preview or execution route.

Nothing enumerates storage until this page is explicitly opened. There is no new
persistent setting or migration, and no changes to library/settings file formats,
guest behavior, USB-discovery defaults, import policy or renderer. This safe,
read-only route needs no experimental environment switch. No write/create/delete,
rename/copy/move, game acquisition, protected payload, payment or network feature
is added. Package version is unchanged; increment it during a later authorized
console package preparation.

## Host evidence

The new `tools/xbox/tests/files-browser.cpp` was compiled before implementation:
it failed with `jadyx/storage/files_browser.h file not found`. After implementation,
**30 assertions pass**, covering internal-root fallback, folder/file semantics,
busy gating, invalid indices, Unicode names, nested Back navigation, rejected path
components, page boundaries/overflow and clearing stale entries on unavailable or
permission failures. These are portable policy tests, not WinRT integration tests.
The same 30 assertions also pass with AddressSanitizer/UndefinedBehaviorSanitizer
(`-fsanitize=address,undefined`). It is registered in
`tools/xbox/tests/run-frontend.ps1`.

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion \
  -Wno-sign-conversion -Werror -I src tools/xbox/tests/files-browser.cpp \
  -o /tmp/jadyx-files-browser
/tmp/jadyx-files-browser
```

`-Wno-sign-conversion` excludes an existing signed-char loop in `storage_path.h`;
the remaining conversion/shadow warnings are errors. New aggregates use field
assignment. Windows MSVC C4456/C4457/C4459/C4244 checks remain a required build gate.

Existing macOS-compatible selection, before and after:

| Gate | Before | After |
| --- | --- | --- |
| 17 frontend C++ tests from `run-frontend.ps1`, directly with clang++ | 14 pass / 3 abort | Same |
| 11 Python fixture scripts | 11 pass | 11 pass |
| 3 Node page tests | 1 pass / 2 fail | Same |
| Requested `tests/port` unittest discovery | Directory absent | Same |

Total: **31 existing executable gates, 26 pass and five pre-existing failures**,
with no new failure. `game-menu`, `system-settings`, `cheats-page` abort (`-6`);
`network-portal-page` and `log-export-page` fail in their `renderUpdate` mock's
`classList` access. The `tail` pipeline's status is not a successful unittest run.
Catalog completeness/placeholder and integration-order checks were also rerun
successfully after the final translation additions. `git diff --check` passes.

The 17 original frontend names are `frontend-input`, `controller-selection`,
`controller-preference`, `stick-motion`, `player-slots`, `stick-navigation`,
`game-menu`, `game-keyboard`, `system-settings`, `cheats-page`, `mods-page`,
`text-entry`, `localization-catalogs`, `play-history`, `themes`, `theme-settings`,
`themes-integration`. Compile each with `-std=c++20 -O0 -DJADYX_NO_WINRT -I src`,
its `tools/xbox/tests/<name>.cpp`, `src/jadyx/i18n/localization.cpp` and
`src/jadyx/i18n/locales/*.cpp`; `cheats-page` also links
`src/core/memory/cheat_text.cpp`. Run each resulting executable independently.

Python scripts: `release`, `draw-preparation-report`, `pipeline-build-trace`,
`address-wait-trace`, `gc-trace`, `material-trace`, `material-probe-parser`,
`dxil-branches`, `dxil-probe`, `noperspective-interpolation`, `integration-locales`;
run `python3 -B tools/xbox/tests/<name>.py`. Node scripts: `updater-page`,
`network-portal-page`, `log-export-page`; run
`node tools/xbox/tests/<name>.cjs`.

## Exact future console test (not executed)

1. On a separately authorized Windows build host run
   `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`.
   Require no new MSVC shadow/narrowing errors. Bump the package version, prepare
   only `build-uwp/package/jadyx.appx` using
   `powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library`,
   and archive matching EXE/PDB in `build-uwp/symbols/<version>`.
2. Install over the existing package in Xbox Dev Mode, **Game** mode. Record Series
   X/S, revision/version, theme and language. Retain existing settings/library data.
   Before opening Files, navigate/launch/return as usual and confirm unchanged
   sources, settings and library loading. No new configuration flag is needed.
3. Prepare only synthetic files: `browser-test/empty/`, `browser-test/nested/child/`,
   a zero-byte file, a 123-byte text file, a Unicode filename, and 130 small text
   files named `entry-000.txt` through `entry-129.txt` inside `browser-test/paged/`.
   Put a copy in LocalState using Device Portal and another on USB. Keep hashes,
   sizes and file counts outside the console for the read-only comparison.
4. Open Settings → File manager → Browse files. With no USB authorization, internal
   storage must remain available and the USB authorization hint must be visible.
   Enter internal storage and inspect the synthetic tree. A opens folders only;
   B moves to parent, then roots, then back to settings. Empty directories show the
   empty-state text; zero and 123-byte sizes must match. Confirm on a file must
   never launch it, import it, preview it or alter it.
5. Enter `paged/`: traverse all three broker pages, 64/64/2 entries. Check ascending
   names, no duplicates/missing entries in a stable folder, first/last-page bounds,
   row scrolling, Unicode ellipsis and the breadcrumb. Refresh resets selection
   safely. Test controller, keyboard Enter/Esc/R/arrows and pointer buttons; tab,
   menu and launch input must not reach the library while Files is open.
6. Go back and authorize the USB test folder using Add games folder. Reopen Files;
   inspect that root without copying. Unplug while listing and while idle, then
   refresh/enter/change page. Require a responsive loading/error screen, no partial
   or stale selectable rows after a failed read, and usable Back. Reconnect and
   refresh; the same remembered root must work again. Close during loading and
   immediately try reopening; wait/retry must never publish an old location.
7. Exercise an inaccessible/revoked test source if the platform permits it: access
   denied must suggest reauthorization, with internal storage still usable. Refuse
   a synthetic reparse directory if available. Do not manipulate the owner's only
   files to provoke these cases. Verify a missing USB root cannot redirect into
   another root or ascend above its authorization.
8. Repeat in English, Spanish and Brazilian Portuguese, both existing themes,
   including empty/error/loading states. Compare the synthetic files' hashes,
   sizes/counts and existing library/settings after inspection and restart: all
   unchanged. Export normal diagnostics and record any exceptions. No console
   claim is made until the WinRT sorted-query, permissions, disconnect and visual
   gates above have passed on the Series.
