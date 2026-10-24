# Controller preference and synthetic motion port

## Restricted controller preference

Jadyx already supports per-game manual controller styles and sideways single
Joy-Con mapping. Its automatic policy keeps an accepted Pro controller, or picks
handheld before a single Joy-Con. NXbox's inventory A rows 120 and 124–125 identify
the missing title policy and motion path. Reference: NXbox commits 3946ecf16 and
a2aafaeb6 (read locally, GPL-3.0-or-later); no game data was copied.

`joycon_preference=1` in the installed package's `boot.cfg` enables the policy.
It defaults off and changes no saved options. In Automatic mode only, IDs
`010003F003A34000` and `0100187003A36000` prefer dual Joy-Cons, then a right Joy-Con
when docked. Other IDs and explicit styles keep Jadyx's behavior. The existing
single Joy-Con mapping makes the left stick drive the right Joy-Con's only stick.

Host baseline: four existing portable gates (controller-style, controller-selection,
player-slots, stick-navigation) pass on macOS. The requested `tests/port` unittest
discovery cannot run: that directory belongs to NXbox, not this repository.
The new controller-preference regression failed to compile before the change
(missing policy), then passed under Clang C++20 with `-Wall -Wextra -Wshadow
-Wconversion -Werror`. It checks both IDs, switch off, undocked fallback, unrelated
IDs, empty styles and the single-stick mapping. No Windows build or console run.

Console test (pending): install a normal Jadyx package with the switch absent,
then with `joycon_preference=1`. Use a synthetic HID homebrew/policy fixture with
each listed program ID: request Pro+dual, then handheld+left+right while docked.
With Automatic expect dual then right, and check left-stick directions and face/
SL/SR buttons in the homebrew readout. With switch absent expect the old policy.
Repeat undocked (handheld fallback), with an unrelated ID, and with explicit Pro,
left and right styles (manual choice wins). Connect a second pad and reconnect
both: player ownership must remain stable. Collect `jadyx_log.txt` and
`jadyx_diag.txt`; style reconnection logs must match the fixture. This verifies
policy, not commercial-title compatibility.

Host command:

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror -Isrc \
  tools/xbox/tests/controller-preference.cpp -o /tmp/jadyx-controller-preference
/tmp/jadyx-controller-preference
```

## Synthetic motion

Ported the corrected NXbox synthesis from c1ecc6f54 with its original NSPX notice,
plus the activation/arbitration ideas from 79f93cf15 and 09977cf42. The 1ea432c2a
threshold correction needs no change in Jadyx: its virtual motion mappings already
use the real poller's default **0.007 turns/s**, rather than NXbox's old `1`.
The real delivery test verifies that deadband through production code.

`synthetic_motion=1` in `boot.cfg` enables player-one motion. It defaults off;
there is no new UI string, dependency, settings schema or migration. Existing
controller options, style numbers, scripts and saved files are untouched. No
additional Pulse state is serialized. Existing Pulse files remain readable;
sensor-request recovery across Pulse resume is not certified by these host tests.

Motion activates only after the guest starts a six-axis sensor for the connected
player-one style, and while the existing motion setting is enabled. Guest starts
are idempotent and tracked per style/device; stopping one dual Joy-Con half does
not cancel the other. In the opt-in path, HID delivers only requested sensor
halves. With the switch off, legacy sensor enable/delivery behavior is retained.
The service constructor clears transient requests for a new service lifetime.

With a sensor requested:

- Hold **View + right stick** for yaw/pitch; the left stick still moves normally.
  A stationary View tap sends Minus on release. View+Menu still belongs to Jadyx.
- Tap **L3** for less than **250 ms** without moving a stick: send one **40 ms**
  guest L3 pulse. Holding for 250 ms, or moving a stick while held, consumes L3
  for motion. The left stick tilts up to **60 degrees**; the right stick aims at
  up to **0.5 turns/s**. During this L3 gesture both sticks are consumed.
- Release sends level gravity/zero gyro. A modal menu, software keyboard or menu
  shortcut cancels motion and pending taps. Captured controls stay consumed until
  released after closing. Menu navigation still reads raw controls.

Pad, rebound keyboard and existing scripted input converge before synthesis, and
style rotation happens afterwards. Only virtual ports **0 and 8** receive motion;
players 2–8 retain their own input. Motion is sent on every 4 ms input poll, even
when values repeat. Timestamp deltas stay in **1–100000 microseconds** after
repeated timestamps or scheduling stalls. Stop sends a neutral sample and removes
the opt-in delivery mode. Suspend/resume acceptance remains a console test.

### Host evidence

The new gesture/sensor gate failed before implementation because the production
synthetic-motion header did not exist. Afterwards it passes with real headers,
covering off/no-sensor passthrough, View, L3 tap/hold/movement, sensor transitions,
modal cancellation and held-input capture, View+Menu precedence, unit gravity,
finite rates, timestamp bounds, idempotent starts, independent dual sensor stops,
and legacy delivery fallback. One existing header (`game_menu.h`) has six missing
aggregate-field warnings, so this gate suppresses that warning only; shadowing
and conversion warnings remain errors.

The production virtual gamepad/poller/converter/MotionInput delivery gate passes
**420 consecutive samples** (240 yaw + 180 pitch), across **four callbacks** (two
motion entries on each of ports 0 and 8). Rates remain **0.5 turns/s** after the
real gyro filter, repeated values still trigger callbacks, neutral returns zero,
and a player-two sample changes none of those callbacks. It does not mock the
sensor engine/filter, but it does not instantiate WinRT or guest shared memory.

After: the four baseline gates still pass, both new portable gates pass, the
real delivery gate passes: **7 passing executables**. The additional existing
`game-menu` gate aborts at line 168: its system-settings row-height assertion
(`RowTop + (RowCount - 1) * RowStride + RowHeight < Notice`). An instrumented
copy of that same test also aborts at line 168 when compiled with the `main`
controller-style header; all its other production dependencies are unchanged
from `main`. Thus this extra gate has **one pre-existing failure**, not a new
motion failure. It is outside this input port and remains unchanged. `tests/port` discovery
still reports the same missing-directory ImportError. Clang also accepts the
changed `six_axis.cpp` with `-fsyntax-only`. MSVC/UWP, packaging and Series tests
were not run. Changes were formatted with the repository's Clang style; the
Python harness passes offline Ruff formatting/lint and `git diff --check` passes.

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror \
  -Wno-missing-field-initializers -Isrc tools/xbox/tests/stick-motion.cpp \
  -o /tmp/jadyx-stick-motion
/tmp/jadyx-stick-motion
python3 -B tools/xbox/tests/motion-delivery.py
```

The delivery harness uses installed fmt/Boost headers at `/opt/homebrew/include`;
no dependencies are downloaded. The new portable gates are also included in
`tools/xbox/tests/run-frontend.ps1` for a future Windows build.

### Exact console acceptance (not executed)

Use an instrumented, self-authored HID homebrew that displays player/style,
buttons, sticks, both six-axis streams, and controls to Start/Stop each sensor.
The existing renderer-pattern boot homebrew does not exercise six-axis input;
a suitable HID fixture is required for this acceptance, not supplied here.
Build/package on the normal authorized Windows build host, outside this task:

```powershell
# Baseline: both switches absent.
.\tools\xbox\package-appx.ps1 -BootNro C:\tests\hid-motion.nro -BootCfg @('play=1')
# Motion candidate, same payload and build.
.\tools\xbox\package-appx.ps1 -BootNro C:\tests\hid-motion.nro -BootCfg @('play=1','synthetic_motion=1')
# Combined controller-policy candidate, for the two synthetic title-ID fixtures above.
.\tools\xbox\package-appx.ps1 -BootNro C:\tests\hid-motion.nro -BootCfg @('play=1','synthetic_motion=1','joycon_preference=1')
```

Install each `build-uwp/package/jadyx.appx` using Device Portal, in Game mode.
Do not use `-Release` (that mode rejects diagnostic boot overrides). For each run:

1. Before Start, press/release View and L3 and move both sticks. Expect normal
   buttons/sticks and no synthetic rotation, with the switch both absent/present.
2. Start the connected style's sensor. Hold View+right for four seconds, then
   View+up for three; expect nonzero yaw/pitch (up to pi rad/s after guest unit
   conversion), no guest Minus during motion, normal left-stick input. Release:
   level gravity and zero gyro. A separate short View tap sends Minus once.
3. Tap L3 for about 100 ms: exactly one guest press/release. Hold stationary for
   over 250 ms, then aim/tilt: no guest L3, both sticks consumed, nonzero sensor
   values. Releasing after the hold must not produce a tap. Repeat with the
   keyboard's rebound L3/right-stick controls: the result stays player one's.
4. Start both dual sensors; stop left, then right. Right must keep delivering
   after left stops, and motion must end when right stops. Repeat Start twice
   and Stop once on a handle: it must stop. Restart the sensor while L3 is
   already held: release must not invent a tap.
5. During motion open View+Menu, navigate with d-pad/left stick, then close while
   holding controls. Expect zero guest motion/controls until release, functional
   menu navigation and no delayed tap. Repeat with the software keyboard and
   Xbox guide/focus loss. Suspend/resume: no stuck gyro, button or tap.
6. Add a second pad: it neither drives player-one motion nor changes menu
   ownership. Switch player one between Pro, dual, single right and handheld;
   restart the corresponding sensor and verify the intended controller receives
   both sensor mappings with no secondary-player leakage.
7. Reinstall baseline with both switches absent, preserving existing controller
   preferences and a pre-change Pulse session. Confirm prior button/style/script
   behavior and session readability. For candidate Pulse resume, verify sensor
   reactivation explicitly; do not infer it from successful deserialization.

Keep gameplay open until the tester closes it (Q with a keyboard, or the normal
Jadyx return flow); no timed performance claim. Collect version, boot.cfg,
`jadyx_log.txt`, `jadyx_diag.txt` and the homebrew's numeric readout for every
case. No console or commercial-title compatibility is claimed by this change.
