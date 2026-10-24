# Themes on integration (jx-18)

## Finding and change

On 24 October 2026, reset the clean task branch to `nxbox/integration2`
(`2a25d4862e`) and merged `nxbox/16-themes` (`4fc8c8eaff`) with
`--no-ff --no-commit`. One final merge commit contains the resolutions and tests.
This supersedes the earlier integration note that the themes branch contained no
changes: its tip has since advanced.

Seven files conflicted: the en/es/pt-BR catalogs, library canvas, system page,
system settings and painter header. The resolution keeps both parents' features:

- Keep all integration translations, then add the four theme entries in the merged
  `Text` enum order. Each locale has **681 entries**; the source-order regression,
  compile-time completeness and runtime placeholder checks pass. Both parents'
  existing translations are unchanged.
- Keep system rows through Updates unchanged, then **Resolution, Theme**. Preserve
  both settings fields, handlers, titles/hints/values and both persistence paths.
  Changing Theme must not change internal resolution or its per-title overrides.
- Keep the theme-aware painter and its shared `TypeRole`/palette definitions in
  `ui_theme.h`; remove the obsolete duplicate RGB/type declarations from the header.
- Keep the themed shelf headings and compatibility detail text, tile badges,
  hero report, pin placement and FTP connection screen. All automatic integration
  changes for USB, parallel shaders, resolution, compatibility and FTP remain.

The non-textual merge bug was the badge's direct `D2D1::ColorF(color, alpha)`:
semantic theme handles are outside the RGB range. `DrawCompatibility` now uses
`Ink(color, alpha)`, including the theme's opacity. Two new semantic slots extend
its palette from **25 to 27**. Playable stays `#73d69b`, Broken `#f08b8b`; Old NXbox
inherits these status colors. Issues uses the existing Warning token and Untested
uses TextSecondary, including Old NXbox's alpha. The dark disc uses Canvas. All
four shapes remain independent of color.

Jadyx is still the missing/invalid-preference default. Its original 25 colors,
typography, geometry, opacity and focus tokens remain unchanged; the two badge
slots preserve integration's RGB/alpha and every badge coordinate. The inherited
278 canvas-token checks retain original values and literal types. **Actual pixel
identity requires the pending console screenshot comparison**, not just token
checks. Old NXbox remains an explicit saved user choice. No new dependency,
experimental default, renderer behavior, version bump or content acquisition was
added. Existing opt-in switches and the installed-package FTP test hook stay as
on integration2. No network, push, GitHub, PC app or console was used; the NXbox
reference checkout was not modified.

## Host results on macOS

Baseline: integration2, before the merge. Repeated the same gates afterward.

| Gate | Before | After |
| --- | --- | --- |
| FTP CTest | 4/4 pass | 4/4 pass |
| USB runner (including storage paths/discovery) | 5/5 pass | 5/5 pass |
| Package launch | 17/17 pass | 17/17 pass |
| Resolution runner | 4 pass, 2 existing failures | Same |
| Existing 12 frontend tests | 9 pass, 3 existing failures | Same |
| Themes and theme settings | Absent | 2/2 pass |
| New theme integration C++ and drawing-bridge Python gates | Fail before fix | 2/2 pass |
| Catalog source order | Pass | Pass, all 681 ids in each locale |
| Parallel shaders, linked cache, pipeline residency, game library | Pass | Pass |
| Release, GC/address-wait/pipeline traces, draw report, material parser | Pass | Pass |
| Compatibility parser/bundled data/fallback | Pass | Pass |
| Generic NXbox `tests/port` command | Directory absent | Directory absent |

The three existing assertions are `game-menu`, `system-settings` and `cheats-page`;
the resolution runner repeats the first two. Their tests were not changed or
suppressed. The generic command reports `ImportError: Start directory is not
importable: 'tests/port'` on both runs; NXbox's estimated failures do not apply.

The new C++ test failed first on the missing theme header, then on the missing
compatibility palette slots after textual resolution. The drawing check rejected
the raw Direct2D color conversion before the fix. The C++ gate checks badge-token
resolution in both themes, default status colors, final row order and independent
theme/resolution navigation in every locale. It is registered in `run-frontend.ps1`.
The Python gate protects the Windows-only draw call's connection to `Ink`.

The theme-only and compatibility tests pass with
`-Wall -Wextra -Wconversion -Wshadow -Werror`. The integration test passes with
`-Wall -Wextra -Wshadow -Werror`. Adding `-Wconversion` to that larger test exposes
existing conversions in `text_entry_panel.h` and `storage/utf8.h`; no unrelated
warning cleanup is included. FTP uses its strict CMake options. No UWP/MSVC build
was possible here; C4456/C4457/C4459/C4244 remain a Windows gate. New files were
formatted with clang-format and offline Ruff; Ruff and `git diff --check` pass.

Reproduce the theme/integration gates from the repository root:

```sh
python3 -B tools/xbox/tests/integration-locales.py
python3 -B tools/xbox/tests/themes-integration.py
mkdir -p /tmp/jadyx-jx18-tests
for test in themes theme-settings themes-integration; do
  clang++ -std=c++20 -DJADYX_NO_WINRT -Isrc "tools/xbox/tests/$test.cpp" \
    src/jadyx/i18n/localization.cpp src/jadyx/i18n/locales/*.cpp \
    -o "/tmp/jadyx-jx18-tests/$test" && "/tmp/jadyx-jx18-tests/$test"
done
cmake -S tools/xbox/tests/ftp -B /tmp/jadyx-jx18-tests/ftp
cmake --build /tmp/jadyx-jx18-tests/ftp
ctest --test-dir /tmp/jadyx-jx18-tests/ftp --output-on-failure
python3 -B tools/xbox/tests/run-resolution-options.py
python3 -B tools/xbox/tests/usb-play-in-place.py
python3 -B tools/xbox/tests/package-launch.py
python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4
```

The previous [integration notes](nxbox-integration-notes.md) contain commands for
compatibility, shader/cache, game-library/play-history and diagnostic-parser gates.
The [theme notes](nxbox-themes.md#host-validation) list the twelve frontend gates.

## Exact console test (pending, not executed)

1. Capture settled baseline screenshots from integration2 before installing: Home
   with its compatibility report, Library with focused/pinned tiles and all four
   status shapes, Settings/System, FTP and a canceled dialog. Use synthetic
   compatibility metadata from the [badge gate](nxbox-compat-badges.md), backed up
   first; do not add game-derived fixtures. Fix resolution, artwork, clock and
   animation state for the comparison.
2. On the Windows build machine, close any app using the package layout, choose a
   delivery version higher than the installed one, then build and package normally:

   ```powershell
   tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
   & .\tools\xbox\tests\run-frontend.ps1 -Tests @('themes', 'theme-settings', 'themes-integration', 'localization-catalogs')
   powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library
   ```

   Install `build-uwp/package/jadyx.appx` through Device Portal in Game mode,
   preserving LocalState. Keep all experimental switches off for the visual gate.
3. Leave Theme at Jadyx. Repeat the exact baseline views and compare matching
   frames pixel for pixel. Check badge position/shape/colors, focused cover/pin,
   report text, fonts, corners, spacing and focus halo. Missing/invalid saved theme
   must also select Jadyx. Record any mismatch rather than claiming equivalence.
4. Open Settings > System. Verify the end is Updates, Resolution, Theme in en, es
   and pt-BR. Change Theme with left/right/confirm to Old NXbox; verify immediate
   repaint and all four badge statuses, neutral opacity, report text, focus ribbon
   and unchanged resolution value. Move to Resolution, change it, and verify the
   theme stays selected. Keep rendering switches off unless running their separate
   opt-in resolution gate.
5. In both themes, open/close FTP from File manager with only synthetic transfers
   per the existing FTP gate. Verify credentials, Back/Stop, refresh and subsequent
   library badges. Check Home, every library filter, a pinned tile, game options,
   a canceled name editor and Pulse/in-game menu using original homebrew as needed.
6. Close normally and relaunch in Old NXbox; verify persistence and no stale
   brushes/fonts/cover mask on the first frame. Toggle back to Jadyx, repeat the
   screenshots, close/relaunch and confirm persistence. Repeat across all three
   languages. Restore backed-up synthetic compatibility metadata afterward.
7. Run the existing USB, parallel-shader, resolution and compatibility-refresh
   integration gates separately with their explicit switches; none is enabled by
   this merge. The installed-package FTP hook remains test-package-only.

Record version, console model, language, switches, screenshots/pixel differences,
launch/return and fresh diagnostics. Rendering, persistence, MSVC compilation and
console lifecycle/performance remain unverified on this host.
