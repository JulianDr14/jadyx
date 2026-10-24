# Resolution options from NXbox

Branch `nxbox/60-resolution-options`, based on Jadyx main `0fc5c01836`.
Local reference: NXbox `a7968a92f1bda9bbfdcef3d54a650a045b93e41d`, read only.
No upstream contact, network, Windows execution or console execution was used.

## Finding and scope

Jadyx already ships the FSR 1 work from `feature/d3d12-fsr1`: bilinear plus sharp,
balanced and soft FSR, changed live in the display menu. It is a presentation
filter, not internal render-target scaling. `Image::ScaleUp`/`ScaleDown` were
stubs, and graphics/compute supplied zero texture scaling flags.

| Option | NXbox reference | Jadyx after this change |
| --- | --- | --- |
| Internal scale | 0.5, 0.75, 1, 1.5, 2, 3; global and per title | Same six choices; experimental D3D12 implementation |
| Persistence | `resolution_setup` in global/title files | Jadyx LocalSettings, global default and application-ID override |
| Upscaler | Global bilinear / FSR 1 | Existing bilinear / three FSR sharpness presets retained |
| Independent output resolution selector | None found | None added; swapchain follows the window/display |

Reference files: NXbox `src/eden_uwp/title_settings.h`, `ui/library_screen.cpp`,
`game_session.cpp`, and `src/video_core/renderer_d3d12/d3d12_resolution.h`,
`d3d12_texture_cache.cpp`, `d3d12_rasterizer.cpp`, `renderer_d3d12.cpp`.
The arithmetic helper is adapted from NXbox and retains its GPL attribution.
The implementation uses Jadyx's resource allocator, scheduler, blitter and I18n;
it does not replace the renderer with NXbox's different resource abstraction.
Local design models: Jadyx `texture_cache/texture_cache.h`, Vulkan's
`pipeline_helper.h`, `vk_graphics_pipeline.cpp`, `vk_texture_cache.cpp`, and
[`xbox_fsr.md`](xbox_fsr.md). No new dependencies or shaders are required.

## Behavior and persistence

- Settings > System > Internal resolution sets the default for games without an
  override. First install is 1×.
- In a game, hold View + Menu, open Display, then Internal resolution. Left/right
  or A cycles the six scales and saves only that application's override. A failed
  save restores the saved choice and displays the existing localized save error.
- Restart the game to apply a scale. Changing it never mutates resources or shaders
  in a running session. FSR keeps its existing live behavior and global persistence.
- Missing or malformed title values inherit the global value; malformed globals
  fall back to 1×. Enum values are validated against the six choices, not a range.
- Keys are `system_resolution` and `game_resolution_<16 lowercase hex digits>`.
  Reinstalling the same app identity while preserving app data retains them.
- The renderer is gated by `NXBOX_D3D12_RESOLUTION_SCALE=1`, default off. For UWP
  tests the same exact line is accepted in the packaged `boot.cfg`. Without it,
  preferences still save but effective rendering stays at 1×. Diagnostics print
  requested/effective enum values (1, 2, 3, 5, 6, 7 respectively).
- Non-native experimental launches bypass Pulse save/resume. Existing native
  sessions are preserved. Test with Pulse off and cold launches; do not treat a
  restored older native session as a test of new settings.

## Renderer and limits

The shared cache chooses which targets can scale. Eligible images retain native
and scaled allocations. Resource, allocation ownership, resource state and placed
initialization state move together; the existing allocator retires both by fence.
Uploads and guest readbacks go through the native representation; readback restores
the scaled resource without overwriting its higher-resolution contents. Scaled GC
readbacks use the existing synchronous recovery path, not mismatched async footprints.

Viewports, scissors, clear rectangles and DrawTexture regions follow the target
scale. Mirrored blit regions preserve direction. Graphics and compute provide the
same descriptor-level scaling masks as Vulkan; fragment coordinates use the target's
down factor. Present uses scaled image dimensions and normalizes crop in guest
coordinates, avoiding a second scale of the crop. FSR then processes the actual source
extent. Shader environments are still retranslated on load; hot PSO lists are
separated by resolution so scale tests do not train one another's prewarm lists.

Supported targets are renderable, unconverted, uncompressed, noninteger, single-mip,
single-layer, single-sample 2D color or depth-only images, within the D3D12 extent
limit. Arrays, MSAA, stencil, converted/compressed and integer targets remain native
as a framebuffer group. The blitter cannot preserve stencil or integer values;
enabling them by copying depth alone would silently corrupt guest data. Native
fallback targets are logged, capped at 16 messages. This is **partial renderer
coverage**, not full NXbox scaling parity or console certification. A game may keep
its principal 3D target native. Allocation/blit exceptions use Jadyx's existing GPU
failure recovery; no new automatic retry policy or memory budget is introduced.

For a 1280×720 eligible target, the six resulting extents are 640×360, 960×540,
1280×720, 1920×1080, 2560×1440 and 3840×2160. A 1920×1080 target at 3× is
5760×3240, not 4K. Pixel counts of the scaled copy are 0.25, 0.5625, 1, 2.25, 4 and
9 times native. Both representations can remain allocated, so lowering scale does
**not** establish a memory saving. There are no measured FPS or console RAM gains.

## Host validation

Run on macOS, without downloads:

```sh
python3 -B tools/xbox/tests/run-resolution-options.py
```

The new harness was introduced before the implementation: it failed to compile
because `resolution_options.h` did not exist. After implementation it passes six
choice round trips and wraparound, global fallback, title isolation, malformed
values, store rejection and failure, both settings UI entry points, save failure
feedback, all three catalogs, fractional/odd/minimum extents, flipped rectangles,
and both scaling-mask word boundaries/overflow (130 texture and 66 image pushes).

Existing `upscaling` and `localization-catalogs` passed before and after. `fsr-plan`
passes 240 EASU constant sets, 201 RCAS levels and five crop cases. Final runner:
**4 passed, 2 failed**. The two failures existed in main:

- `game-menu`: obsolete `SystemPageLayout` fixed-height assertion. The production
  settings renderer uses its scrollable settings rows. Fullscreen row expectations
  were updated here for the added resolution row; no additional failing checks.
- `system-settings`: UTF-16 surrogate expectation on macOS's 32-bit `wchar_t`, plus
  stale navigation expectations in the same test. A temporary reporting-only copy
  ran every check against main and this branch: the same seven failing checks in
  each, including the navigation-dependent name editor checks.

No UWP/MSVC compile, D3D12 GPU readback test, visual UI check, packaging or console
run was performed. Those are required before enabling the switch for production.

## Exact console test: 3D World and BotW

Use only the tester's own existing dumps and saves. Do not bundle them in the
package. On the Windows build machine, with no app using the package layout:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -BootCfg @('NXBOX_D3D12_RESOLUTION_SCALE=1')
```

Install `build-uwp/package/jadyx.appx` through Device Portal, retaining app data,
and select **Game** mode. Keep the 5120 MiB console limit and existing budgets.

1. In Settings > System turn Pulse off. Discard any kept sessions for these two
   games using their Pulse panes; retain normal game saves. Set the global internal
   scale to **0.75×**. Launch **Super Mario 3D World + Bowser's Fury** cold. Open
   View + Menu > Display: the choice must be 0.75× before a title override exists.
2. In 3D World set **0.5×**, return home without saving a session, and launch cold.
   Verify `resolution: requested=1 effective=1` and `D3D12: resolution image ...`
   lines with half-size extents. Play the same level manually, including water,
   particles, shadows and UI. Check the paused image crop, corners, depth occlusion,
   and FSR bilinear/sharp/balanced/soft comparisons. The display's output size and
   overlays must remain unchanged. Stop when the tester chooses, without a timeout.
3. Launch **The Legend of Zelda: Breath of the Wild** cold. Its initial choice must
   still inherit **0.75×**, not 3D World's override. Set **1×**, leave and relaunch.
   Verify `requested=3 effective=3`, correct native rendering, and no scaled-image
   creation. Test the same outdoor save and a shrine, including shadows, menus,
   map, water and Magnesis/picking. Stop manually, then set **0.75×** and cold-launch
   again: expect `requested=2 effective=2` and scaled extents where eligible.
4. Fully quit and reopen Jadyx. Confirm 3D World's 0.5× and BotW's 0.75× survive,
   independently of a later global change to **1×**. Test a third title without an
   override to confirm it inherits 1×. On an installation with previous overrides,
   record those first; inheritance is only expected for an unconfigured title.
5. In 3D World repeat cold launches at **1.5×**, **2×**, and **3×**, then return to
   **1×**. Log requested/effective values 5/6/7/3 and actual resource extents. Do not
   claim every surface scales: inspect native-target messages. If the principal
   scene stays native, record that as missing coverage, not a successful scale test.
   Watch memory and recovery at higher scales; abort the case on allocation/device
   errors rather than repeatedly loading it. Repeat 1×/0.75× in BotW after this.
6. Repeat the UI check with English, Spanish and Brazilian Portuguese selected;
   verify the resolution row and save error/hint fit and controller navigation reaches
   it. Capture settings evidence without committing game imagery.
7. Repackage with `-Library` and **without** the extra BootCfg line, reinstall while
   preserving data, and launch both titles. Requested values remain saved but
   effective must be **3 (1×)**; the native renderer and Pulse behavior must return.

Export Jadyx log and diagnostics after each cold-launch case. Record app version,
console model, guest/update version, scale/filter, actual scaled/native target
extents, memory peak and any Render Error/Critical/device removal. For performance,
compare the same saves, route and warmed shader history, with manual play; do not
infer FPS from the selector or certify stability from a short boot. Console status:
**not run**.
