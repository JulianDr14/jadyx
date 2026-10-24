# Translation and dub language filter

Task: `jx-24-mod-language-filter`, branch `nxbox/24-mod-language-filter`, based on
local `nxbox/integration3` (`31c2a7709735`). No push, network access, Windows PC,
console access, UWP build or package was performed for this work.

## Finding and reference

The C07 evidence row and rank 9 acceptance gate in
`nxbox/0e-feature-diff:docs/xbox/jadyx-vs-nxbox-features.md` identify a discovery gap:
Jadyx already manages mods, variants, priorities and catalog sources, but cannot
narrow translations/dubs by language. The gate calls for local/mock metadata,
missing-language fallback and no new download system.

Read-only NXbox reference: `rc/next` (`1087264c852c`),
`src/eden_uwp/ui/mods.h:65,89` and `mods.cpp:768–827`. Its filter recognizes names
and categories and keeps translations without a recognized language visible.
The reference is GPL-3.0-or-later, Copyright 2026 NSPX; the new policy header
records that attribution. No reference repository files were changed.

## Change

In a game's **Mods → Explore → Y (Options)**, the last row is
**Translations / dubs**. Press **A** repeatedly to cycle these 11 choices:

1. All mods (default).
2. All languages (translations/dubs only).
3. Portuguese, English, Spanish, French, German, Italian, Japanese, Korean,
   then Chinese, followed by All mods again.

This is an in-memory, per-mod-session choice. Closing the game's mods manager
restores All mods. Changing tabs, source, category, sort or search retains the
choice during that session. There is no new environment switch because the
explicit filter control is itself opt-in, and does not affect emulation.

- `mod_language_filter.h` provides a platform-independent metadata predicate and
  stable projection. It examines only names/categories and CNX's existing
  translation flag. It does not inspect authors, URLs, archives or game data.
- Whole-word matching avoids NXbox substring false positives such as `dub` inside
  `Double jump` or `fran` inside `France costume`. Common language names/tags,
  ASCII case and Latin-1 UTF-8 uppercase names are handled. This is a heuristic,
  not a complete language detector; unsupported/unnamed languages remain visible
  when there is translation/dub evidence. A multilingual entry can match more
  than one language. No language or compatibility guarantee is made.
- `ModStore` retains the current catalog after the existing adult-content policy
  and reprojects it when the filter changes or a page arrives. Source IDs, order,
  generation checks, CNX detail mapping and existing file safety checks remain.
  Filter changes issue no request. Existing category/search/source operations
  retain their normal requests. No new endpoint, downloader or install path exists.
- Filtering applies to loaded catalog pages. Empty filtered pages offer **A: Load
  more** when another page exists. **Options → Load more** also works with visible
  results and from any cursor position, so a later page without matches cannot
  strand the cursor. These use the existing catalog-page request; filtering does
  not automatically crawl all pages.
- The canvas/painter options sheet scrolls at seven rows, including its maximum
  nine-action case (search, active downloads and another page available). The
  active filter summary uses `Palette` and `TypeRole`; 16 strings are supplied in
  English, Spanish and Brazilian Portuguese. Unknown-language fallback is explained.
- Installed order, enable flags, file browsing and the storage broker are
  untouched. Settings, library records and mod manifests gain no fields or schema
  versions, so old files require no migration. Only catalog card metadata is
  additionally retained in memory; no archive data is retained for filtering.

## Host evidence (macOS)

Before implementation, `mods-page` and `localization-catalogs` passed on the
requested integration base. The new host test failed to compile because
`mod_language_filter.h` did not exist (red gate).

After implementation: **7 C++ harnesses pass**, plus **1 Python locale-order
regression test**, with **0 failures**. The new harness executes **53 checks**:
language choices, mixed languages, CNX/missing metadata, default/reset order,
false positives, accented case, empty pages, catalog IDs and menu/paging commands.
The other harnesses cover existing mods navigation, catalog completeness and
placeholders/plurals, layout, inspection, priority and split archives.

Reproduce offline on macOS (no downloaded dependencies; temporary binaries are
removed by the runner):

```sh
python3 -B tools/xbox/tests/run-mod-language-filter.py
```

The new harness is compiled with C++20, `-Wall -Wextra -Wshadow -Wconversion
-Werror -Wno-missing-field-initializers`; no warnings occur. The Windows frontend
runner also includes it. New production aggregates use named fields. MSVC/UWP
compilation and Direct2D visual behavior remain unverified on this Mac.

The generic NXbox command was attempted before and after:

```sh
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

Both attempts report `ImportError: Start directory is not importable: 'tests/port'`;
that suite is absent from Jadyx. This is not the NXbox baseline of 3 failures and
5 errors. JSON-backed manifest/parser harnesses were not compiled: their existing
nlohmann dependency is not installed in this host checkout; no dependency was fetched.

## Exact console acceptance test (pending)

Use a separately built UWP package in Game mode with the owner's existing library,
settings, authorized mod folders and existing catalog access. Do not install or
download any mod/archive during this test.

1. Launch the existing library. On one title press **Menu**, choose **Mods**.
   Verify Installed order, counts and enable flags match the previous package.
   Visit **Files** and an already-authorized folder; cancel without installing.
2. Switch to **Explore** with **RT**. Let the existing catalog list finish. Open
   **Y**, move **Up** once to the final **Translations / dubs** row, and confirm
   its initial value is **All mods**. Press **A** once, then **B**. Verify the
   summary says **All languages**, the fallback hint is present and ordinary
   non-language mods are filtered out of the loaded pages.
3. Reopen **Y**, select the final row again and press **A** for Portuguese.
   Check metadata naming Portuguese/PT-BR remains; explicitly English-only entries
   disappear; translation/dub entries with no recognized language stay. Repeat
   the cycle through English, Spanish and the remaining languages. Do not infer
   failure if the live catalog has no example of a particular language; the
   synthetic host fixture covers every language deterministically.
4. With an empty filtered page and more pages available, press **A** once. Verify
   the loading state and then results or the actionable empty message. With some
   results already visible, choose **Y → Load more → A** from the top of the grid;
   verify another page is requested and any newly matching card opens the correct
   named detail with **A**. Press **B** before selecting a downloadable file.
   At the final page, Load more must disappear. Disconnect the network for a
   catalog request and verify the existing Offline/Retry path, then reconnect.
5. Keep a non-default filter and test search/category/sort, then switch source to
   CNX. Verify the same choice is retained and CNX translation entries without
   language text remain visible. Open a filtered CNX detail and verify it belongs
   to the selected entry, then cancel. Return to All mods and confirm loaded
   source order is restored, without toggling or moving any installed mod.
6. With a search and an already-active download (if present from normal use), open
   Options and traverse every row. Verify seven rows fit, scrolling reaches the
   last row, and Cancel downloads and Language remain distinct. Do not start a
   download merely to create this condition. Check the filter summary/empty hint
   and sheet in EN, ES and PT-BR, with both existing themes, for clipping/focus.
7. Leave the mods manager, reopen it and verify All mods is restored. Restart
   Jadyx and verify the old settings/library and Installed order/enable flags are
   unchanged. Record build, locale/theme, source, example metadata and any visual
   issue. These console steps have not been executed; no Xbox compatibility or
   visual pass is claimed.
