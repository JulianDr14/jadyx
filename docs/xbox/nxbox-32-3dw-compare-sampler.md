<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Per-instruction comparison samplers (R048 / R102)

## Finding and scope

Base: Jadyx `main` at `0fc5c0183694d4cc91b062f00a5641315de30003`.
Branch: `nxbox/32-3dw-compare-sampler`. Reference source was read locally from
`/Users/miguel/.local/share/nxbox/repo`, without modifying that repository.
Inventory: `nxbox/0b-inventory-b-renderer:docs/xbox/nxbox-port-inventory-b-renderer.md`,
rows **R048** and **R102**; the frontend/core inventory branches have no matching rows.
The implementation idea comes from NXbox **`9c878c47dfaa12bfd926d92b8334b48a4eba07cc`**.
This is one correctness fix: R102 is the ambient-cube application of R048,
not a second independent title-specific patch.

Jadyx previously created a comparison filter whenever the guest TSC enabled depth
comparison. That same TSC can also be used by ordinary texture instructions.
The shader's `TextureDescriptor::is_depth` distinguishes those uses. Passing an
ordinary Sample/SampleLevel through a D3D12 comparison sampler is undefined;
NXbox's commit records zero ambient-cube results on Xbox. That historical report
is not a new Jadyx console measurement.

## Change

- Every TSC retains a normal descriptor; a compare-enabled TSC also gets a separate
  comparison descriptor. Each descriptor has a distinct monotonic sampler-table key.
  TSC lookup/draw reuse remains keyed by the original guest sampler ID.
- The shared table builder walks each stage's texture descriptors, including every
  array element, in the same order as `GatherStageViews`. Graphics and compute choose
  the variant with `descriptor.is_depth`. Table allocation still happens before
  resource/UBO allocation, preserving the existing heap-exhaustion/reset contract.
- `DrawTexture` explicitly selects the normal handle/key. Sampler diagnostics report
  the selected filter. Integer lowering still reads the original border/LOD/wrap data.
- Existing point MIN/MAX canonicalization, unsupported filtered reduction fallback,
  anisotropy, address modes, comparison function, LOD bounds and bias clamp remain.
  Move construction/assignment and destruction transfer/free both descriptors.

This is a pure API correctness fix, enabled unconditionally; no experimental game
behavior or new boot option is introduced. With TSC comparison disabled, shader
depth requests retain the previous normal-sampler fallback. No settings, guest data,
Pulse format, shader/DXIL disk format or game identifiers change. Samplers/tables are
runtime objects; existing caches need no deletion. Existing boot.cfg files remain valid.

Descriptor cost: **1** CPU descriptor per non-comparison TSC, **2** per
compare-enabled TSC (previously **1**). Shader-visible table length remains **1**
per sampled binding. No FPS, RAM savings or visual improvement was measured.

## R102: keep CPU BC-array/cube decoding

The sampler fix does **not** prove that Jadyx's BC workaround is unnecessary.
`Image::Image` in `d3d12_texture_cache.cpp` decodes single-sample 2D BC images
with non-block-aligned top-level extents, and, by default, images with more than
one layer. A cube has six layers and therefore takes that existing path too.
`DecodedBcFormat`, decoded image uploads and decoded SRVs are untouched.

The independently documented cause in `xbox_d3d12_phase4.md` (0.2.44–0.2.45)
is disagreement between BC array copies and Xbox texture sampling: some layers
were displaced by **32 rows**, with a repeating **16-layer** pattern. CPU decode
made the tested arrays match exactly. Changing sampler reduction cannot repair
that copy/layout contract, nor the separate normalized-coordinate problem for
non-aligned BC extents. The existing `bc_arrays=native` diagnostic opt-out and
default decoded path are preserved. Removing this workaround needs separate
Series evidence for BC formats, faces/layers and mips; ambient lighting alone
would not establish that.

## Host verification on macOS

Run from the repository root:

```sh
python3 -B tools/xbox/tests/comparison-sampler.py
python3 -B tools/xbox/tests/pipeline-build-trace.py
python3 -B tools/xbox/tests/gc-trace.py
python3 -B tools/xbox/tests/draw-preparation-report.py
python3 -B tools/xbox/tests/address-wait-trace.py
python3 -B tools/xbox/tests/material-trace.py
clang++ -std=c++20 -Isrc tools/xbox/tests/draw-sampler-cache.cpp -o /tmp/jx32-draw-sampler-cache
/tmp/jx32-draw-sampler-cache
```

The six existing checks passed **before and after**. Draw sampler reuse checks
**2,560,000** requests, with **619,902** reuses. The new regression was run before
the fix: it compiled successfully, then failed the assertion
`normal.Filter == 0` for a compare-enabled TSC used by a plain ambient-cube read.
It passes after the fix.

The new gate compiles the actual production `Sampler` declaration/implementation
and `SamplerTableBuilder` against a recording D3D12 shim, using C++20 with
`-Wall -Wextra -Wshadow -Wconversion -Werror`. It tests mixed normal/comparison
uses of one TSC, both table orders, array elements, fresh table construction,
unique keys, move/free ownership, enabled/disabled comparison, MIN/MAX and
anisotropy, unsupported reduction, no-mip LOD, clamping and integer border state.
Additional source assertions verify both pipeline callers and `DrawTexture` wiring.
It records API descriptors; it does not execute DXIL, GPU filtering, real heap
rollover or the whole graphics/compute translation unit. No new dependency.

Formatting: repository clang-format rules on changed C++ regions and the new fixture;
Ruff formatter from the local offline cache for the Python runner.
No UWP/MSVC build, Windows PC execution, console run, network access or push was
performed. C4456/C4457/C4459/C4244 still require the Windows compiler gate.

## Console acceptance tests — pending

On the authorized Windows build machine, build/package the baseline and candidate
separately, following the normal version/symbol discipline and keeping the active
package at `build-uwp/package/jadyx.appx`:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -BootNro tools\xbox\boot_nro\boot.nro
```

Install via Device Portal, **Game** mode. Smoke-test the original 600-frame
homebrew pattern and successful `RunHeadlessBoot returned 0`. This smoke test
alone does not exercise the sampler fix.

### R048: mixed ordinary/depth use

For the deterministic feature gate, use an original synthetic guest fixture that
reuses one compare-enabled TSC for a constant-color cube/plain 2D read and a depth
reference instruction. Such a GPU fixture is **not added by this host-only change**;
do not count the homebrew smoke or recording shim as that gate passing.

Fixture specification: use RGBA8 texels `(64,128,192,255)` on every cube face and
a D32 depth texture filled with `0.5`; point filtering, clamp, LOD 0, compare LESS.
Plain samples must reproduce the RGBA values, depth references `0.25` and `0.75`
must return **1** and **0**. Run vertex/pixel and compute variants, normal/compare
bindings in both orders, descriptor arrays, repeated draws, and after command-list
reset/heap rollover. Plain DrawTexture must preserve color. Repeat with TSC compare
disabled against the baseline behavior; include integer-border and MIN/MAX regression
cases. Require byte-exact readback for the point cases and no Render/Critical,
invalid descriptor, device removal or allocation failure. This specification uses
only original constants, no game-derived fixture data.

### R102: 3D World ambient lighting and BC workaround

1. Use only the owner's existing local dump/save. Record title/update version,
   console model, level/save/camera, cache state and package version. Use the exact
   previously dark view if available; the inventory does not identify a universal
   reproducible level. Fully restart between baseline and candidate.
2. Package a diagnostic library build with the following command (boot.cfg is
   bundled in the **package install directory**, not read from LocalState):

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -Library -GraphicsCapture -BootCfg gpu_capture_full=1
   ```

   `-Library` supplies `play=1`; leave `bc_arrays=native` **absent** for both builds.
   Enter the same scene with ambient
   lighting and shadows visible; take a GPU capture through the existing in-game
   capture menu (View + Menu, **Capture graphics and resume**). Preserve captures privately, alongside screenshots,
   `LocalState/jadyx/log/jadyx_log.txt` and `jadyx_diag.txt`.
3. Inspect captured shader instructions/bindings. A plain ambient-cube binding
   with TSC compare enabled must now have a normal filter (filter reduction bits
   `(filter >> 7) & 3` are not **1**). Depth-comparison bindings with that TSC
   enabled must retain reduction **1** and the original comparison function.
   Verify ambient color and shadows separately. A scene without both binding types
   is insufficient to certify mixed-use behavior.
4. Check every visible cube face/mip for corruption, and the known BC-array scenes
   from the earlier workaround. Do not remove the decoded path based on a successful
   ambient test. Any optional `bc_arrays=native` comparison is a separate diagnostic
   run, after restart; it must not change the shipped default.
5. Repeat on Series X and S when available. Gameplay has no timeout or programmed
   input; end with Q when satisfied. Measure warm/cold runs separately and remove
   synchronous capture instrumentation before measuring FPS. No current console
   compatibility/performance result is claimed by this commit.
