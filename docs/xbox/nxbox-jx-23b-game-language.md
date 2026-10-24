# Per-game language override

## Finding and scope

The local feature comparison (`nxbox/0e-feature-diff`,
`docs/xbox/jadyx-vs-nxbox-features.md`, L10 and remaining-port rank 6) identifies
NXbox's per-title language choice as the missing part of Jadyx's language settings.
Jadyx already has global automatic/explicit language selection, eighteen selectable
languages, regional-variant/English/first-supported NACP fallback, and EN/ES/PT-BR
catalogs. Those policies remain the source of truth.

Reference inspected read-only: NXbox `rc/next`,
`src/eden_uwp/title_settings.h`, `ReadLanguage` and `SaveLanguage`. This port uses
Jadyx's LocalSettings broker and canvas rather than copying NXbox's settings files
or interface. No NXbox files were modified. The worktree starts at Jadyx `main`
`0fc5c0183694d4cc91b062f00a5641315de30003`.

## Change

- Library > game options > Game language offers inherit plus the existing eighteen
  languages. A/Right advances, Left goes back, X resets to inherit. Keyboard uses
  Enter/Left/Right and O (X is also accepted); pointer clicks the value or reset row.
  LB/RB switches sections and B exits as on the other management pages.
- The inherited value displays the currently resolved global language. Reset
  removes the override, so subsequent global changes propagate to that title.
- LocalSettings stores one validated int32 per nonzero program ID under
  `game_language_<16 uppercase hexadecimal digits>`. Missing, invalid or wrongly
  typed data inherits; storage errors show the existing localized failure notice
  without publishing an unsaved choice. Unknown identities/homebrew cannot save a
  shared title-zero preference. Global settings and library file formats are unchanged.
- Fresh boot resolves override > global, then uses Jadyx's existing NACP fallback.
  Missing NACP leaves the preferred language intact. Pulse resume does not apply a
  newly selected override; use a fresh start to change a game's language. Existing
  resume behavior is otherwise retained.
- Metadata uses the same preference/fallback and existing language-keyed cache.
  Changing the choice requests a metadata refresh. On cold startup, a cache record
  stored in an override language may need a full metadata read to recover its title
  identity; no cache format migration is required.
- Presentation uses the existing `LibraryCanvas`, `Palette`, `TypeRole`, hit targets
  and navigation. All new strings are in en/es/pt_br catalogs. No dependencies,
  background services, renderer changes or network features were added. No env
  switch is needed: only an explicit per-title selection changes guest behavior.

## Host validation

Before implementation, the new `game-language.cpp` failed to compile because the
new language model did not exist. After implementation it passes, including all
18 persistence round trips, 19-state forward cycling/backwards wrap, isolated reset,
invalid values, unknown identity, failed writes, current-global inheritance,
324 requested/single-supported-language fallback combinations, management navigation,
and the three UI catalogs' inherit formatting.

Run on macOS from the repository root:

```sh
c++ -std=c++20 -DJADYX_NO_WINRT -Isrc \
  tools/xbox/tests/game-language.cpp \
  src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/{en,es,pt_br,registry}.cpp \
  -o /tmp/jadyx-game-language-test
/tmp/jadyx-game-language-test
```

The new test translation unit also compiles with
`-Wall -Wextra -Wshadow -Wconversion -Werror`. Existing localization dependencies
have unrelated signedness warnings under that broader warning set, so that strict
check is applied to the new test/model, not claimed for the entire frontend.
`tools/xbox/tests/run-frontend.ps1` now includes `game-language` in its default gate.

The existing twelve default frontend gates were compiled with Apple clang/C++20
and `JADYX_NO_WINRT`, using the runner's source list (including `cheat_text.cpp`
for cheats-page). Before and after: **9 pass, 3 fail**, with no new failing gate.
The failures are `game-menu`, `system-settings` and `cheats-page`; all other gates,
including catalog completeness/placeholders, pass. Temporary diagnostic copies
confirmed identical first failing checks before/after: game-menu line 167,
system-settings line 72, cheats-page line 144. The inherited command
`python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`
was run before and after and fails identically because Jadyx has no `tests/port`.
The NXbox failure counts do not describe this repository.

No UWP/MSVC build, PC app run or Xbox test was performed. The host gate does not
certify WinRT persistence, the console layout or guest language behavior.

## Exact console acceptance test (pending)

1. Build the `jadyx` UWP target with the normal project toolchain and package into
   `build-uwp/package/jadyx.appx`. Install in Xbox Dev Mode, Game mode. Use existing
   owner-provided dumps only; this feature needs no new content or network access.
2. Set the global game language to English. Launch two existing titles without
   visiting their language pane; confirm their previous language behavior. Return
   to the library. Keep global region and interface language unchanged.
3. Open title A's options with Menu, move with LB/RB to Game language. Confirm
   “Use global setting (English (US/UK))”. Exercise A/Right and Left, choose a
   language the title supports, exit with B. Reopen and verify it persisted. Quit
   and relaunch Jadyx and verify again. The global setting and title B must remain
   unchanged. Fresh-start A (discard its kept Pulse session first, if present),
   confirm the guest language and inspect the existing `Headless boot: game
   languages ..., preferred ..., system language ...` log.
4. Choose a regional variant absent from A's NACP but with its sibling present
   (for example Latin American Spanish versus Spain's Spanish). Fresh-start and
   verify sibling fallback. Then choose a language absent from that title entirely:
   verify English, or the first declared supported language if it lacks English.
   Confirm the library title follows the same available language.
5. On A's pane press X to reset. Reopen/relaunch and confirm inherit persists.
   Change the global language: A and B must follow it. Set an override on A again
   and verify a later global change still only affects B. Recheck after USB
   disconnect/reconnect if the title is on an authorized external drive.
6. Repeat the pane, notices and reset in English, Spanish and Brazilian Portuguese.
   Check controller prompts, readable seven-section bar, keyboard Enter/arrows/O,
   pointer value/reset rows, B exit, and navigation to Mods/Cheats/Pulse/Remove.
   Opening/closing alone must not create an override. An original NRO with identity
   zero must not allow a saved shared language choice.
7. With a disposable kept Pulse session, change its override and resume: the new
   override must not be injected into that session. Discard it explicitly and
   fresh-start to apply the choice. Do not discard the owner's only wanted state.

Console acceptance remains pending; no performance or game-compatibility claim is
made from host tests.
