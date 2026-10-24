# Jadyx and Old NXbox interface themes

Settings > System now includes **Theme** (English) / **Tema** (Spanish and Brazilian Portuguese).
Left, right or confirm switches between **Jadyx** and **Old NXbox**. Jadyx remains the default;
there is no migration of existing installations to the alternative appearance.

## Finding and change

The library is a Direct2D canvas, not XAML. `library/uwp_library_canvas.cpp` draws Home, Library,
Settings, the file manager, controller panels, management pages and dialogs. `ui/ui_painter.*`
provides their shared colors, text layouts, shapes and focus effects; game-menu and Pulse surfaces
also use that painter. The old palette lived in `ui_painter.h`, typography in `ui_painter.cpp`,
and additional colors and geometry were embedded in the library canvas.

`ui/ui_theme.h` now owns the theme tokens: 25 semantic color handles with RGB **and alpha**,
font size/weight/line/tracking tokens, cover and settings radii, focus-ring parameters, surface
opacities and a shared canvas-unit scale. The scale retains the original literal types: integer
arithmetic, floating-point rounding and pointer hit rectangles keep the same coordinates. Vector
icon paths, image-decoding limits, animation timing and nonvisual indices remain implementation
details. Seven extra text roles distinguish settings titles/details, cover titles, the clock,
statistics, controller hints and pill labels; their Jadyx definitions alias the original roles exactly.

The preference uses the existing `SystemSettings`, input effects and WinRT `LocalSettings`
save/apply path. Key: `system_theme`, signed 32-bit integer, `0 = Jadyx`, `1 = Old NXbox`.
Missing, unknown or incorrectly typed values fall back to Jadyx. The host-tested reader/writer
callbacks are also used by the WinRT adapter. Changing the option redraws immediately. Each
painter owns its theme snapshot and text/brush caches; only the selected enum crosses threads
atomically. Changing themes clears text formats/layouts, the focus brush, cover mask and colored
backdrop scrims, so the old theme cannot leak through a cached resource. No dependency was added.

## Before and after

**Jadyx** retains the original `#08090b` canvas, `#111315` surfaces, `#f5f5f7` focal white,
Inter with Segoe UI fallback, the original ten font roles, tracking, line spacing, focus halo,
cover mask and all canvas spacing. Token regression checks cover the previous numeric values;
pixel equality still needs the console screenshot comparison below.

**Old NXbox** uses the local, read-only reference at
`/Users/miguel/.local/share/nxbox/repo`, **origin/main at
`7275e402e17485beca00902074725dd5911c9c5d`**, read with `git show`, without fetching:

- `src/eden_uwp/ui/theme.h`: true black, `#f5f5f7` text, secondary/tertiary alpha 0.64/0.40,
  white surfaces at 0.055/0.09, tab glass `#141418` at 0.50, sheet `#141416` at 0.94,
  secondary pills at 0.12, and the original controller-prompt colors.
- `src/eden_uwp/ui/renderer.cpp`: Segoe UI and the original weights/sizes. NXbox's 1920×1080
  units become Jadyx's 1280×720 units by dividing by 1.5: hero 72→48, row title 25→16⅔,
  row detail 19→12⅔ and cover caption 20→13⅓. Native DirectWrite line metrics replace
  Jadyx's custom tracking/uniform line spacing in this theme. Clock figures use the same
  Consolas → Cascadia Mono → Courier New → Segoe UI fallback order as NXbox.
- `src/eden_uwp/ui/widgets.{h,cpp}`: focus ribbon `#2fd07a`, `#8fbf6a`, `#e05a45`, `#e8343e`
  at 0/38/64/100%, 100-degree direction, outer gap 6→4 and stroke 3→2. The alternative
  skips Jadyx's diffuse focus shadows. The brush is cached and falls back to a solid focus
  outline if Direct2D cannot create a gradient.
- `src/eden_uwp/ui/library_screen.cpp`: cover radius 22 at 1080p, capsule-shaped selected
  settings rows, and plain idle rows. The alternative omits the enclosing settings-row card.

The shared canvas intentionally retains Jadyx's seven-column grid, 144-unit tiles, 24-unit
horizontal gutters, hero/rail placement, filter model, file-manager hierarchy, data summary,
controller glyphs and dialog contents. NXbox uses 196-unit tiles and 30-unit gutters at 1080p,
a different rail/hero arrangement, additional screens and a dedicated capsule around its hints.
Those layouts, artwork-derived background ramps, additional title variants and tile tilt
are not recreated here. Some shared labels still use Jadyx's broader semantic roles; this is a
palette/typography/focus adaptation to Jadyx's canvas, not a second frontend. Selected settings
rows and tabs adopt NXbox's shape/material where existing hit rectangles permit it.

## Host validation

Both new tests failed before implementation (missing theme header, then missing settings field
and row). They now pass. `themes.cpp` checks lookup, unknown values, saved-value round trips via
an in-memory LocalSettings stand-in, preservation of unrelated settings, live preference lookup,
the original 25 RGB values, ten original font definitions and seven aliases, 278 canvas-unit
constants, default surface/radius flags and NXbox reference values. `theme-settings.cpp` checks
the default, both directions/confirm, shared library geometry/hit positions, and all three
complete I18n catalogs. A deliberate 64→65 layout-token mutation was rejected by the regression
test. The token checks also preserve each literal's type, including integer versus float arithmetic.

The repository's `run-frontend.ps1` includes both tests. On this macOS host, equivalent C++20
commands build the twelve existing frontend tests plus the two new tests:

```sh
mkdir -p /tmp/jadyx-theme-tests
for test in frontend-input controller-selection player-slots stick-navigation game-menu \
  game-keyboard system-settings cheats-page mods-page text-entry localization-catalogs \
  play-history themes theme-settings; do
  extra=""
  if [ "$test" = cheats-page ]; then extra=src/core/memory/cheat_text.cpp; fi
  clang++ -std=c++20 -DJADYX_NO_WINRT -Isrc "tools/xbox/tests/$test.cpp" $extra \
    src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp \
    -o "/tmp/jadyx-theme-tests/$test" && "/tmp/jadyx-theme-tests/$test"
done
```

Before: **9 pass, 3 existing assertion failures** (`game-menu`, `system-settings`, `cheats-page`).
After: **11 pass, the same 3 failures** including both new tests. A temporary check-counter build
of `main` and the modified headers confirms the same first failing check: respectively 699,
40 and 45. No existing failure was suppressed or changed. The requested Python `tests/port`
discovery command fails before and after because this Jadyx repository has no `tests/port`.

No Windows/MSVC build, PC application run, console access or network operation was performed.
Direct2D rendering, native-font availability and real WinRT restart persistence remain console
validation items; a portable host test is not evidence of pixel equality.

## Exact console test (pending)

Use an authorized normal UWP build/package; keep the existing settings and library. Build with
`tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`, then follow the existing
packaging procedure and install `build-uwp/package/jadyx.appx` through Device Portal in Game mode.
Do not erase LocalState. Use only synthetic fixtures/homebrew or content already legally supplied
by the owner; this feature requires no new game assets, keys or firmware.

1. Before installing, capture Home, Library with a focused tile, Settings, the file manager and
   a non-destructive dialog at a fixed resolution. After installing, leave Theme at **Jadyx**,
   select the same entries and compare settled frames with the same art/clock state. Check
   text size, line breaks, tile corners, spacing, colors and focus halo. Animated art and the
   clock must be held consistent for a pixel comparison.
2. Open **Settings → System**, scroll to **Theme/Tema** (after Updates), and press right once.
   It must show **Old NXbox** and repaint immediately without restarting. Verify black canvas,
   Segoe UI, translucent surfaces and green-to-red focus. Move away and back; no white halo or
   previous-theme scrim should remain. Check clipped text, long translated names and scrolling.
3. Visit **Home**, **Library** and every filter. Move focus to each grid edge and the import tile.
   Open game options without launching content; return. Navigation and pointer targets must
   still agree with the drawn controls.
4. Open **Settings → Files/File manager**. Browse an existing folder/source and return without
   importing/deleting anything. Open the user-name editor, enter a temporary character, then
   cancel. Inspect dialog background, keyboard, labels, focused actions and dark scrim.
5. Close Jadyx normally and relaunch. **Old NXbox** must still be selected, with no first-frame
   flash of Jadyx styling. Visit library, settings, file manager and the same canceled dialog.
6. Change interface language to **en**, **es**, then **pt-BR**. Check **Theme/Tema**, its hint and
   the two product names; switch themes with left, right and confirm in each language.
7. Switch back to **Jadyx**, repeat the screenshots from step 1, close and relaunch, and confirm
   that Jadyx persists. Repeat the theme round trip several times while changing focused games
   to catch cached text, cover-mask or backdrop colors from the previous theme.

Record package version, console model, resolution, screenshots and any discrepancies. No console
visual result is claimed by this change.
