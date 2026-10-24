# Gate the shared-buffer stride correction (Jadyx port 39)

## Finding and change

Branch `nxbox/39-applet-errors-optin` starts at local
`nxbox/38-applet-errors` (`cea1003fcb`). That branch changed every shared
GraphicBuffer stride from 5120 to 1280 while describing the error overlay as
opt-in. The stride correction has no Xbox validation, so enabling it globally
could affect unrelated games using shared buffers.

The correction now requires a separate `shared_buffer_stride_fix=1` in
`boot.cfg`. Missing or `shared_buffer_stride_fix=0` publishes the original
`origin/main` stride, 1280 * 4 = 5120. Enabling it publishes 1280 pixels.
The other published fields, allocations and offsets are unchanged: seven
3,932,160-byte slots, 27,525,120 bytes total, visible dimensions 1280 x 720,
padded allocation height 768. No saved data or settings migration is involved.

The boot option defaults to false, is applied before starting the core, and
restores the previous session-only value on exit, including early returns.
It is a plain non-persisted field, not a registered settings-file entry.
The existing `nxbox_error_overlay` option and other branch changes remain intact.
Enabling the overlay alone does not enable the stride correction. To test both:

```ini
play=1
nxbox_error_overlay=1
shared_buffer_stride_fix=1
```

## Host evidence (macOS)

Before the fix, the original stride test passed (one test), as did the error
integration suite (three tests). After extending `applet-stride.py`, the test
compiled but aborted at the disabled-state stride assertion; its new boot
wiring guard also failed. After the fix, both stride tests and all three error
tests pass. The stride harness compiles the production constants/publisher
with Clang C++20, `-Wall -Wextra -Werror -Wshadow -Wconversion`, and checks
all seven slots with the switch false, true, then false again (21 publications).

Commands:

```sh
python3 -B tools/xbox/tests/applet-stride.py
python3 -B tools/xbox/tests/applet-errors.py
```

The standalone `frontend-input`, `storage-path` and `applet-errors` C++ fixtures
pass before and after. Each was compiled and run from a temporary directory:

```sh
clang++ -std=c++20 -Isrc -pthread tools/xbox/tests/<name>.cpp -o <temporary-binary>
<temporary-binary>
```

The requested `python3 -B -m unittest discover -s tests/port -p "test_*.py"
2>&1 | tail -4` reports the same missing-directory ImportError before and
after: this is Jadyx, which has no `tests/port`. Its Catch2 tests in `src/tests`
require linking the core/video stack and are not configured on this host.
`git diff --check` passes. No network, PC application, UWP build or Xbox test
was run; MSVC and console rendering remain unverified.

## Exact console acceptance (pending; not run)

1. Build and review on the UWP build host; use the normal
   `build-uwp/package/jadyx.appx`, matching symbols, and Xbox Game mode.
   Use a synthetic homebrew that acquires the VI shared framebuffer, reports
   the returned GraphicBuffer fields and presents a distinct row/corner pattern
   in the 1280 x 720 visible crop of its 1280 x 768 RGBA8 allocation.
2. Cold boot with `play=1`, `nxbox_error_overlay=0`, and no stride option.
   Require returned stride **5120** and the same buffer fields/presentation
   as an `origin/main` build. Repeat with `shared_buffer_stride_fix=0`.
3. Cold boot with `nxbox_error_overlay=1` and `shared_buffer_stride_fix=0`.
   Require stride **5120**; exercise and dismiss the synthetic error dialog
   using the error acceptance procedure in `nxbox-38-applet-errors.md`.
4. Cold boot with both options set to **1**. Require stride **1280**, byte
   row pitch **5120**, all rows/corners aligned, valid offsets for every
   presentation slot, and unchanged slot allocation sizes. Exercise capture,
   close/reopen and suspend/resume, CPU present and accelerated present.
5. Cold boot again with the stride option removed. Require stride **5120**
   again. Record config, returned fields, captures and logs for each run;
   require no new Render error, out-of-range access or crash. Default-path
   visual comparison must use `origin/main`, even if its stride is imperfect.
