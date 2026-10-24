# R040: native MSAA allocation validation

Jadyx branch `nxbox/33-msaa-validation`, based on local `main`
`0fc5c0183694d4cc91b062f00a5641315de30003`. No network, Windows build,
PC app run, packaging or console execution was performed.

## Finding and scope

The renderer inventory on `nxbox/0b-inventory-b-renderer`, row R040, identifies
validation missing from Jadyx's existing native MSAA constructor. The frontend
and core inventory branches have no additional R040 row.

Jadyx passed `num_samples` to `SamplesLog2` before validating it. Invalid counts
could assert, and shifted dimensions were clamped to one, silently accepting
partial sample grids. BC, 1D/3D and multi-mip MSAA descriptions could reach the
allocator. There was no `MULTISAMPLE_QUALITY_LEVELS` query for the exact typed
attachment format and sample count.

Local read-only NXbox references reviewed: `838d6a0cc` (allocation and quality
checks), `ace9227c1` / `7ba71af70` (block dimensions/shared sample helpers),
`fc39a8d75` (physical formats), and `9f7b8bea6` (unsupported-image handling).
Both projects are GPL; this is an adaptation to Jadyx, not a transplant of
NXbox's context, resource ownership or nullable-image model.

## Change

- A portable shape gate accepts only 1/2/4/8/16 samples. For MSAA it requires
  uncompressed 2D storage, depth 1, one mip, nonzero extents and an exact guest
  sample grid: 2x1, 2x2, 4x2 or 4x4. Both guest and physical copy-format block
  dimensions are considered, so decoding a compressed format does not bypass it.
- Before the allocator, require attachment support and query the requested count
  once using the physical color `format.view` or depth `format.dsv`. A failed
  HRESULT or zero quality levels rejects construction. Quality remains zero;
  there is no retry at a lower sample count.
- Rejection logs the reason, formats, count, shape and mip count, then throws
  through Jadyx's existing GPU exception/controlled-shutdown path in
  `src/video_core/gpu_thread.cpp`. It does not manufacture a null image for callers
  that expect a valid handle. Recovery on Series remains to be tested.

These are allocation bug fixes, enabled without a configuration switch: inputs
rejected here cannot describe the requested native D3D12 MSAA allocation.
Single-sample images retain their allocation path and do not issue the MSAA
quality query. No settings, saved data, cache formats or Pulse formats change.
The allocator, PSO sample count, upload/download and resolve implementations are
unchanged. This does not establish full MSAA transfer parity or a rendering/FPS
improvement.

## Host checks and numbers

Run from the repository root on macOS:

```sh
python3 -B tools/xbox/tests/msaa-validation.py
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -Werror -Isrc \
  tools/xbox/tests/msaa-validation.cpp -o /tmp/jadyx-msaa-validation
/tmp/jadyx-msaa-validation
```

Before implementation the two constructor-wiring regressions failed, one for the
shape gate and one for the exact capability query. The new executable initially
could not compile because the portable validation header did not exist. After:
**2/2 wiring tests and 122 executable cases pass**. Removing either gate in an
isolated temporary header makes the executable abort at the matching assertion.
The executable covers invalid counts 0..64, valid counts, shapes, incomplete grids,
failed queries even with nonzero quality levels, zero/multiple quality levels,
attachment rejection without querying, exact count preservation and the unchanged
single-sample path. The Python checks only verify native wiring; they are not a
substitute for compiling or running D3D12.

Existing tests run before and after, all passing:
`gc-trace.py`, `address-wait-trace.py`, `pipeline-build-trace.py`,
`draw-preparation-report.py`, `material-probe-parser.py` in `tools/xbox/tests/`.
Additionally, `heap-packing.cpp` passes **1,248,000** placements and
`blit-coverage.cpp` passes **300,000** random blits on macOS.

The common task command was attempted before and after:

```sh
python3 -B -m unittest discover -s tests/port -p 'test_*.py' 2>&1 | tail -4
```

It cannot discover tests because this Jadyx fork has no `tests/port` directory;
the NXbox baseline of three failures/five errors does not apply.
`full-capture-parser.py`, `log-export-tar.py` and `updater-appx-integrity.py`
require external fixtures/packages and were not counted as test failures or
claimed passing. No new dependency was added. New C++ was formatted with
clang-format and checked with Clang conversion/shadow warnings as errors;
MSVC/UWP compilation is still pending.

## Exact console gate (pending)

A developer must build on Windows and install the normal library package through
Device Portal, without uninstalling the existing app, with Series in **Game**
mode. Use the existing Xbox build/package procedure; this branch does not deliver
an APPX. Run on both Series X and S before claiming coverage of both devices.

Use a diagnostic build/test fixture which constructs `D3D12::Image` with the
following `VideoCommon::ImageInfo` matrix through the real texture runtime. Catch
expected constructor exceptions in the fixture so the matrix can continue.
This matrix is a specification for that console fixture, not a claim that the
current boot NRO already generates malformed images.

1. **Shape validation:** base input is `e2D`, `A8B8G8R8_UNORM`, size 64x64x1,
   resources `{levels=1, layers=1}`. Try sample counts 0, 3, 32: each must log
   `image allocation rejected` before allocation. At 4 samples, independently try
   `BC3_UNORM`, `e1D`, `e3D`, depth 2, levels 2, width 63, height 63 and width 0:
   each must reject. Repeat the valid base at 2/4/8/16 samples, and with two array
   layers; shapes must pass to the capability gate without truncating dimensions.
   Single-sample BC and multi-mip fixtures must retain their old behavior.
2. **Exact capabilities:** repeat valid color and `D32_FLOAT` depth inputs at
   1/2/4/8/16 samples. Independently record `CheckFeatureSupport` for
   `R8G8B8A8_UNORM` / `D32_FLOAT` at that exact count, flags NONE. A successful
   query with nonzero levels must allocate with exactly that count and quality 0;
   unsupported counts must log rejection before allocation. If all requested
   counts are supported, an instrumented query seam is required to exercise a
   failed HRESULT and zero levels. Each must reject, with no lower-count retry.
3. **Supported rendering:** at supported 1/2/4 samples, clear color to
   `(0.25, 0.5, 0.75, 1)` and depth to `0.5`; draw a deterministic homebrew triangle
   with a matching PSO sample count. Compare with the unchanged build and verify
   per-sample values using a diagnostic `Texture2DMS.Load` shader. Do not use
   Jadyx's ordinary MSAA staging transfer as the oracle: transfer parity is outside
   R040. Check resulting resource dimensions 64x64 (1 sample), 32x64 (2),
   32x32 (4), 16x32 (8), 16x16 (16).
4. Confirm no device removal or unexpected Render/Critical errors. Expected
   rejected inputs emit explicit Render errors. Remove the diagnostic fixture and
   launch an owned/homebrew workload, return to the library, then resume an
   existing Pulse session to smoke-test unchanged compatibility. Export
   `jadyx_log.txt` and `jadyx_diag.txt`, console model, exact query results and
   fixture output. A host policy pass alone does not certify this gate.
