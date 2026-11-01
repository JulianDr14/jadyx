# Renderer regression suite

`tools/xbox/tests/renderer-suite/` checks that a change to the D3D12 renderer breaks nothing else,
in behaviour or in speed. It runs on the development PC. The console is still where a change is
finally confirmed.

```
python tools/xbox/tests/renderer-suite/suite.py                 # everything (~3 h; gates are most of it)
python tools/xbox/tests/renderer-suite/suite.py --layers scenes,perf
python tools/xbox/tests/renderer-suite/suite.py --only blend_factors unit-bc45-decode
python tools/xbox/tests/renderer-suite/suite.py --update-baseline   # accept the current results
```

Exit code 0 means no regressions. Each run writes `build-uwp/renderer-suite/<time>/` with
`summary.txt`, one log per gate, the emulator logs of the scene runs, and the read-back pictures.

## Before running

- `build-uwp` built (`cmake --build build-uwp --target jadyx`, from PowerShell with
  `tools\xbox\build-env.bat`).
- The guest payload built:
  `powershell -ExecutionPolicy Bypass -File tools\xbox\tests\renderer-suite\build-payload.ps1`
  (devkitPro with switch-dev and deko3d).
- The scenes and perf layers repackage `build-uwp\package\layout` and register it on the PC
  (`local-run.ps1`), under the version already installed there (a lower one is refused, and the
  suite never uninstalls). Do not package for the console during a run, and package again
  afterwards.
- Do not rebuild jadyx while the gates run: every build relinks `common.lib`, which several gates
  link against.

## The three layers

### gates

Every component gate in `tools/xbox/tests` that touches the renderer (`run-<name>.ps1`), plus the
renderer's standalone unit tests that have no script of their own (`unit-<name>`, built by
`run-unit.ps1`). Each runs as its own process, with a 20 minute limit. The result is pass, fail or
timeout, compared with `baselines/gates.json`.

Gates that need a capture or a game dump are left out. To add a gate, add its name to `GATES` (or
`UNITS`, with its build line in `run-unit.ps1`) in `suite.py`.

### scenes

A homebrew program (`payload/`, deko3d) runs about 200 small scenes through the whole emulator:
guest command processing, the texture and buffer caches, shader translation and the D3D12 backend.
None of the gates covers that path end to end.

Each scene draws into a 32x32 target or runs a compute shader. It then reads the result back and
compares it with what the documented behaviour says it must be. It never compares with what the
emulator does today. The scenes cover:

| File | What |
|---|---|
| `scenes_clears.cpp` | clears and draws into every colour format, masked and scissored clears, two targets, odd and large sizes |
| `scenes_blend.cpp` | blend factors, separate alpha, dual source, sRGB and float targets, logic op, write masks, MRT, alpha test |
| `scenes_depth.cpp` | depth compare and formats, sampled depth, shadow samplers, clamp, bias, bounds, viewport depth range, stencil ops, compares and masks |
| `scenes_raster.cpp` | culling, front face, rasterizer discard, polygon modes, scissors, viewports and swizzles, clip distances, points, lines, stipple |
| `scenes_geometry.cpp` | topologies (quads, fans, polygons, adjacency), index sizes, primitive restart, instancing, indirect draws, vertex formats, CPU overwrites of vertex and index buffers, tessellation, geometry shaders |
| `scenes_textures.cpp` | BC, ASTC and ETC2 formats, texture types, swizzles and views, wrap modes, filtering, mips and LOD, fetch, offsets, gather, multisampling and resolves, render to texture |
| `scenes_shaders.cpp` | compute (indirect, atomics, shared memory, images), integer and float maths, control flow, uniform and storage buffers, discard, derivatives, interpolation, provoking vertex |
| `scenes_transfers.cpp` | blits, image and buffer copies, uploads that replace a texture, push data, query reports, and the GL conventions (lower-left origin, -1..1 depth) |

The scene runs use `async_pipelines=off`. With asynchronous pipelines, a draw whose pipeline is still
compiling is skipped, so the read-back would depend on compile timing.

The program writes `START`, `PROBE` and `END` lines to `sdmc:/renderer_suite/results.txt` and to the
emulator log. If a scene hangs or takes the emulator down, the host adds it to `skip.txt`, counts it
as `crash` or `hang`, and runs the program again without it. If deko3d itself rejects a scene (for
example, rendering to a format the hardware cannot render to), the scene is counted as `invalid`: a
bug in the scene, not in the emulator. Each scene is compared on three things:

- **status**: pass, fail, crash or hang, compared with `baselines/scenes.json`. It is the same on every
  machine, because it comes from what each scene checks.
- **picture**: the last target the scene read back, pixel by pixel, with a small tolerance for
  normalized and float formats and none for integer formats. Stored in
  `baselines/pictures-<computer>.zip`. Another GPU or driver may round differently, so each machine
  keeps its own. When a picture changes, `<scene>.expected.png` and `<scene>.actual.png` are written
  beside the run.
- **debug layer**: the run has the D3D12 debug layer on. Each message id, and each renderer error in
  the log, is attributed to the scene that was running. A scene that gains one is a regression
  (`baselines/debug.json`). The GPU thread runs behind the guest, so a message can land on the scene
  after the one that caused it. `--no-debug-layer` turns this off.

`--only <scene> ...` runs just those scenes (the program reads `only.txt`), so the downloads and
log lines of one scene are not cut off by the others. A run with `--only` never updates the picture
or debug-layer baselines. A full scenes run takes a few minutes.

The suite repackages and registers its payload on every scenes or perf run. `--no-package` reuses
the one already registered, so it does not pick up a rebuilt payload or emulator.

A picture or debug-layer change can be correct, for example after fixing a scene that was wrong.
Look at the PNGs, then accept the change with `--update-baseline`.

### perf

The same program in its performance mode: 1500 identical frames at 1280x720, each with 5000 draws,
per-draw constants, a state change every 4 draws and 4 textures. The emulator runs with
`gpu_profile=1`. Its 300-frame windows are read with `tools/xbox/perf-summary.py`'s parser. The
first window, which includes start-up, is dropped. Of the remaining windows, the median counts. Of
`--perf-runs` runs (2 by default), the best counts.

| Measure | Regression when |
|---|---|
| draws per frame | it changes at all |
| pipeline creates, pipeline stalls, new resources, downloads per window | it grows |
| FPS | more than 15 % below the baseline |
| p95 frame time | more than 15 % + 1 ms above the baseline |

The baseline is per machine (`baselines/perf-<computer>.json`). The FPS margin is there because
the PC also does other work. A change that moves FPS by less than the margin needs a console run
(`tools/xbox/perf-run.ps1`, `perf-summary.py`). On the development PC the perf scene holds 30 FPS
with the GPU thread busy about 31.6 ms per frame, so it catches a slowdown but not a speed-up
above 30.

## Reading a result

`summary.txt` has one section per check: `REGRESSION` lines fail the run, `IMPROVED` means
something that failed now passes, `KNOWN` is a failure the baseline already had. A scene regression
lists the scene's own `PROBE` lines, which say which pixel or value was wrong and what it had to be.

The scene expectations come from the documented behaviour, not from real hardware: no Switch was
used to check them. A scene that fails in the first baseline may be an emulator bug, or a scene that
expects the wrong thing. Either way the baseline records it as a known failure, and the suite then
reports when it changes.

Failures the first baseline records, by cause (2026-11-01):

| Cause | Scenes |
|---|---|
| Early syncpoints (the default `gpu_fence=safe`) tell the guest the work is done before what a compute shader or a query wrote reaches guest memory; with `gpu_fence=wait` they pass | `compute_fill`, `compute_fill_copied`, `compute_indirect`, `compute_ids`, `compute_uniforms`, `compute_samples_texture`, `math_float`, `control_flow`, `report_primitives`, `report_samples_passed` |
| Integer clears and integer draw targets | `clear_*int`, `draw_*_Sint` |
| Shader instructions the recompiler does not translate (CCTL, IMAD and IMUL forms) | the scenes that use them |
| Tessellation | `tess_triangles`, `tess_quads` |
| Other query reports read zero | the remaining `report_*` |
| D3D12 has no equivalent | blend constant mixes, logic op on UNORM, last provoking vertex, custom restart index, wide and stippled lines, points, polygon modes and stipple, viewport swizzles |

The blend scenes expect what deko3d really programs: it turns the alpha destination factor into a
different hardware factor, so a scene written from the API documentation alone fails on that.
