# Applet errors and shared-buffer stride (NXbox port 38)

## Shared-buffer stride

Reference: NXbox `167b461ec` (GPL), frontend inventory branch
`nxbox/0a-inventory-a-frontend`. Jadyx published 5120 bytes as the
`GraphicBuffer::stride`, but nvdisp forwards that value unchanged to
`FramebufferConfig`. D3D12 CPU present multiplies it by bytes per pixel;
OpenGL uses it as `GL_UNPACK_ROW_LENGTH`; Vulkan also consumes pixels.
The original port published 1280 pixels unconditionally. Follow-up
[jx-39](nxbox-jx-39-applet-errors-optin.md) gates this unvalidated correction
behind `shared_buffer_stride_fix=1`; missing or `0` retains the original 5120.

Allocation and old data remain unchanged: 1280 x 768 x 4 = 3,932,160 bytes
per slot, seven slots = 27,525,120 bytes. Visible height stays 720. No
settings, saved sessions or disk layout are migrated.

Host test: `python3 -B tools/xbox/tests/applet-stride.py` compiles the real
production geometry constants and `MakeGraphicBuffer` with a minimal producer.
Before: aborts at `buffer.stride == 1280`. After: one test passes, checking all
seven offsets, handles, dimensions, row pitch, padded size and pool bounds.
Existing `frontend-input.cpp` and `storage-path.cpp` compile with
`clang++ -std=c++20 -Isrc` and pass on macOS before this change.
The requested NXbox unittest discovery fails because Jadyx has no `tests/port`.
The Catch2 target in `src/tests` links the whole core/video stack and is not
configured on this host; no dependencies were fetched.

Console acceptance (not run): install a reviewed UWP build through the normal
`build-uwp/package/jadyx.appx` path, Game mode. Use a synthetic homebrew applet
which acquires a shared framebuffer, fills 1280 x 768 RGBA8 with distinct
corner colors and numbered rows, and presents a 1280 x 720 crop. Inspect the
returned graphic buffer: stride 1280, byte row pitch 5120; all visible rows and
corners must align. Exercise each presentation slot, capture, close/reopen and
suspend/resume. Check for Render errors and out-of-range memory access. Repeat
with CPU present and the native accelerated path. Desktop OpenGL/Vulkan gates
remain pending; their pixel-stride contracts were inspected locally.

## HLE guest-error overlay

References: NXbox `17fd8c6a9` and `9cffeb41d` (GPL), frontend inventory
above and applet-selection row in `nxbox/0c-inventory-c-core`. Jadyx's default
error frontend only logs; it does not call the completion callback. Firmware
error selection also bypasses an HLE frontend. The port supplies
`UwpErrorApplet` and an `ErrorDisplay` queue to the existing play loop and
`GameMenuSurface`. It keeps Jadyx's painter, controller prompts, modal input
capture and EN/ES/PT-BR catalogs; no NXbox UI framework or dependency is added.

Opt in using these lines in the installed package's `boot.cfg`:

```ini
play=1
nxbox_error_overlay=1
```

Absent or `nxbox_error_overlay=0`: existing applet selection and frontend remain
in use. The switch is session-only, does not change the settings file, and only
forces HLE in manual play. The previous `error_applet_mode` is restored when the
session exits, including early returns. All old boot.cfg entries, data formats
and saved settings are retained. No firmware-first/global applet policy is
ported. The existing Pulse snapshot format is unchanged; resuming a snapshot
which already contains a firmware error applet is not converted to HLE.

Emulator threads enqueue requests; A or B marks the visible request by ID.
The play loop completes it on the following tick, outside the queue mutex and
after removing it. The entire input batch is consumed; held-button suppression
uses the existing `MenuInputCapture`. Error dialogs take precedence over the
menu and game keyboard and restore the covered surface when dismissed.
`Close` marks pending requests for completion. Explicit shutdown and a scope
guard stop the queue before core teardown and reject late producers.

The core error applet callback formerly captured raw `this`. It now locks a
weak reference to the shared error applet before completion. An expired guest
cannot be called, and repeated completion publishes output/exits only once.
These lifetime corrections apply to HLE errors generally and are safe without
the experimental UI switch. The error number uses the console convention
`2000 + module`, then description, each padded to four digits. Custom main and
detail text are joined by a blank line; a timestamped request displays the code
while the core reporter retains the timestamp. The body uses the existing
painter's ten-line ellipsis; long messages remain complete in the core report.

### Host evidence

Before implementation, `python3 -B tools/xbox/tests/applet-errors.py` reports
**two failures**: no registered UWP error frontend and a raw-owner callback.
The new queue fixture also cannot compile against the old tree because no
error display exists. After implementation:

```sh
python3 -B tools/xbox/tests/applet-errors.py
clang++ -std=c++20 -Wall -Wextra -Werror -Wshadow -Wconversion -pthread \
  -Isrc tools/xbox/tests/applet-errors.cpp -o /tmp/jadyx-applet-errors
/tmp/jadyx-applet-errors
python3 -B tools/xbox/tests/applet-stride.py
```

Three error tests and one stride test pass. The C++ queue fixture passes:
producer-thread publication, owner-thread completion, two queued requests,
wrong/stale IDs, repeated dismissal, callback reentry, Close, shutdown discard
and rejection after Stop. The Python suite compiles the actual UWP adapter and
production core callback/completion bodies with minimal Result/storage stubs:
three error variants, empty-main handling, empty callback, duplicate completion,
expired guest and a queued callback after guest destruction. This is portable
logic coverage, not a whole-core or UWP binary build.

Existing portable tests, before (using `main` catalogs/header) and after:
`frontend-input`, `storage-path`, `localization-catalogs`, `text-entry` pass;
`game-menu` aborts with SIGABRT in both versions. Catalog/text/menu fixtures link
`src/jadyx/i18n/localization.cpp` and `src/jadyx/i18n/locales/*.cpp`; other fixtures
compile with `clang++ -std=c++20 -Isrc`. No new existing-test failure observed.
Requested `python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 |
tail -4` fails before/after with the same missing-directory ImportError.
`git diff --check` passes. New C++ files were clang-formatted; existing-file
formatting is limited to edited code. Windows-only surface/build validation,
MSVC and Xbox tests remain pending. No network, PC run, package or console used.

### Exact console acceptance (pending)

1. Build/review on the normal UWP build host; package only through
   `build-uwp/package/jadyx.appx`, archive matching symbols, install in Game mode.
   Use a synthetic homebrew that opens `AppletId::Error` as a foreground library
   applet, supplies the normal common arguments then one of the argument buffers
   defined in `src/core/hle/service/am/frontend/applet_error.cpp`. Zero padding
   and reserved bytes; no network, keys, firmware or game data are needed for HLE.
2. First boot with `play=1`, `nxbox_error_overlay=0`, recording the baseline. Then
   enable `nxbox_error_overlay=1` and cold boot the same homebrew. Request mode 0
   (`ShowError`, 0x14 bytes), `use_64bit_error_code=false`, raw Result
   `(123 << 9) | 7`. Expect **2007-0123**, localized title/fallback, no automatic
   completion before input. Press A: exactly one applet-exit notification and one
   0x1000-byte output storage, then the homebrew increments its completion count.
3. Request mode 5 (`ShowErrorRecord`, 0x18 bytes), 64-bit error
   `(uint64_t{2007} << 32) | 123`, timestamp 12345. Expect **2007-0123** and the
   core report timestamp 12345. Dismiss with B; expect one completion.
4. Request mode 2 (`ApplicationErrorArg`, 0x1014 bytes), raw Result as above,
   language code `en`, UTF-8 main `Synthetic applet error` and detail
   `Dismiss once to continue.` (zero-terminated). Expect both texts separated
   by a blank line and **2007-0123**. Repeat with empty main, empty detail and
   non-ASCII text. Repeat all three variants with console UI language English,
   Spanish and Brazilian Portuguese; chrome follows the UI, guest text is intact.
5. Hold A/B while dismissing: the homebrew's gameplay button counter must not
   advance until release and a new press. Queue another error: stale/duplicate
   dismissal must not close it. Try View+Menu during an error, and an error over
   an already open menu/keyboard; it must capture actions and restore the prior
   surface. No callback may run under the queue mutex.
6. Close the error through the applet RequestExit path and verify one completion.
   Suspend/resume while visible; then leave/terminate the session while visible
   and launch the homebrew again. No old error or callback may survive. Confirm
   the prior applet-mode setting is unchanged after a switch-off cold boot.
   Record homebrew counters, logs, language, and memory; require no Render error,
   crash or callback into a destroyed applet. No performance claim is made.
