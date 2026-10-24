<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Startup warmup controls (jx-22)

## Finding and scope

Acceptance comes from `nxbox/0e-feature-diff`,
`docs/xbox/jadyx-vs-nxbox-features.md`: R09, rank 3, step 8. The reference
was read locally, without changing the NXbox checkout, at `rc/next`:
`src/eden_uwp/ui/launch_screen.{h,cpp}` and
`src/eden_uwp/game_session.cpp` (preload/skip callback). Jadyx keeps its own
D3D12 cache, profile format, renderer, storage and frontend. This implements
that control concept directly in Jadyx rather than importing NXbox's UI.

Jadyx previously passed an empty stop token to shader loading and offered no
JIT stop request. Its existing `ThreadWorker::WaitForRequests(token)` stops
the persistent worker pool when the token fires. It waits for active work,
but the stopped pool cannot service later on-demand compilation. Therefore
passing a new UI token straight through that existing wait is unsafe for skip.
There was also no scope-exit drain if the load coordinator threw while jobs
still referenced its local counters and callback.

## Change

- `NXBOX_WARMUP_CONTROL=1` opts into a startup screen on **fresh interactive
  D3D12 launches** (`play=1`). It can be set in the existing `boot.cfg` or
  supplied as a process environment variable. Default is off. Removing the
  boot line (and unsetting the environment variable) restores the existing UI.
  `NXBOX_WARMUP_CONTROL=0` clears the boot-file opt-in.
- Uses `GameMenuSurface`, `UiPainter`, `Palette`, `TypeRole`, theme-resolved
  colors and the existing renderer menu-image bridge. EN/ES/PT-BR show the
  current phase, processed/planned counts, elapsed seconds, skip/cancel and
  the warning that skipping can cause gameplay stutter. Unknown totals say
  that saved preparation is being read. No ETA or combined percentage is
  invented; the count resets between shader and CPU phases.
- Uses the existing controller catalog and selected player-1 device. Physical
  prompts follow face-button mapping. Initial/held input and reconnection do
  not synthesize presses. A second pad cannot navigate this screen unless
  selected by the existing controller policy.
- **Skip preparation** requests a cooperative stop for both phases. Shader
  parsing stops at a cache-entry boundary; queued build jobs check the token
  before compiling. Active builds finish, and an **unconditional drain** keeps
  the pool alive for gameplay. A scope guard drains before callback/state
  destruction on exceptional coordinator exit as well.
- CPU owners check the same token between blocks and while preparing owners.
  The existing RAII coordinator joins every worker, including on exceptions.
  Cancelled progress retains processed counts rather than jumping to 100%.
  Already compiled code remains valid and profile observers are reattached.
  If shaders were skipped first, profile setup still runs in record-only mode
  so subsequent gameplay can continue learning.
- **Back to library** upgrades a skip if needed, drains work, joins/removes the
  painter, then uses the existing shutdown/return status **10** before
  `System::Run`. There is no Pulse snapshot creation during this cancellation.
- Pulse restore keeps its existing screen and semantics. Automated/non-play
  boots and other renderers keep the existing presentation. No settings,
  library, profile, save or shader-file schema changes; no storage access or
  authorization bypass. No dependencies added.

A running driver/Mesa call and synchronous profile reads are not forcibly
interrupted. The screen says it is finishing active work while stopping. This
is cooperative cancellation, not a fixed wall-clock timeout. No worker is
abandoned to meet a deadline.

## Numbers and host validation (macOS, 2026-10-24)

Base: local `nxbox/integration3` (`31c2a7709735`), descended from `main`.
The task branch was fast-forwarded to this specified integration base before
implementation. No network, console, Windows PC, packaging or UWP build ran.

| Gate | Before | After |
| --- | --- | --- |
| 12 existing portable frontend/budget executables | 10 pass, 2 fail | Same 10 pass, same 2 fail |
| `warmup-control.py` | New API absent: host test fails to compile | 5 tests pass |
| Real `ThreadWorker` shader harness with old token wait | Fails `later_builds == 1` | Passes with unconditional drain |
| Existing locale order / theme bridge Python gates | 1 + 1 tests pass at base | 1 + 1 tests pass |

The two existing failing executables are `game-menu` and `system-settings`
(abort/assertion gates). They were not changed. The locale catalog executable
also verifies complete translations and matching placeholders in all languages.

The new native harness tests 10 independent sessions, initial held input,
reconnection, skip-to-cancel escalation, exact **1%, 40%, 95%** stops of three
100-item owners (3/120/285 processed out of 300), zero-sized owners, ordinary
completion and UI callback exceptions. The shader harness holds one active
compilation, skips **20** queued builds, waits for the active callback, and
then executes new work on the same pool. The old wait strategy fails this
last assertion, reproducing the regression a naive token-only port introduces.
Both native harnesses also pass Clang `-Wall -Wextra -Wshadow -Wconversion
-Werror`. MSVC and the WinRT painter have not been built here.

The screen polls/paints about every **33 ms** while open. Its CPU bitmap is
capped at **1920 × 1080**, **7.91 MiB** per RGBA buffer (not total UI/GPU
memory), centered on larger outputs. Existing shader-worker counts, JIT code
budgets and process memory limits are unchanged. No console latency, memory,
FPS or visual result is claimed.

Run the new host gates from the repository root:

```sh
python3 -B tools/xbox/tests/warmup-control.py
python3 -B tools/xbox/tests/integration-locales.py
python3 -B tools/xbox/tests/themes-integration.py
```

The existing portable baseline can be repeated on macOS with:

```sh
out=$(mktemp -d /tmp/jadyx-host.XXXXXX)
for name in frontend-input controller-selection player-slots stick-navigation \
    game-menu game-keyboard system-settings mods-page text-entry \
    localization-catalogs play-history prewarm-budget; do
    clang++ -std=c++20 -DJADYX_NO_WINRT -Isrc \
        "tools/xbox/tests/$name.cpp" src/jadyx/i18n/localization.cpp \
        src/jadyx/i18n/locales/*.cpp -o "$out/$name" && "$out/$name"
done
rm -rf "$out"
```

## Exact console acceptance test (pending)

1. On the Windows build machine, run the normal incremental UWP build:
   `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`.
   Use normal library packaging in `build-uwp/package/jadyx.appx`; do not use
   an unattended homebrew payload or overwrite a layout in use. Install over
   the existing app in Game mode, preserving LocalState. No new package was
   produced in this task.
2. Start with the switch absent. Open a disposable original homebrew using
   the existing library/storage authorization flow; verify normal shader/JIT
   preparation and return/relaunch. Check an old library/settings fixture
   still loads and a Pulse resume still uses its original screen. Preserve
   the owner's saves, library, profiles and authorization tokens.
3. In the existing LocalState `boot.cfg`, set `play=1`, `jit_prewarm=1` and
   `NXBOX_WARMUP_CONTROL=1`. Use an original homebrew with enough independent
   pipelines and CPU blocks to make both warm phases observable. Run once
   with that test title's caches absent, exit normally and run again warm.
   For a cold case, move **only the disposable test title's** shader/JIT
   caches aside through the existing authorized storage flow; never delete
   the owner's caches, saves or Pulse sessions.
4. Repeat in EN, ES and PT-BR, with both Jadyx and Old NXbox themes, at 1080p
   and 4K. Check phase/count/time, no clipped translated prompts, theme
   contrast, readable centered canvas, and no fake 100% while draining.
   Empty caches/zero totals must advance without a hang.
5. On each populated phase, press the **displayed Skip preparation button**
   at <10%, 40–60%, and >90%. Default unswapped pad uses X. Keep the pad
   connected, let active work finish, and verify gameplay starts and can
   compile a new shader/block. Check `warmup: skipped; workers drained` and,
   where applicable, `D3D12: startup shader preparation stopped` /
   `JIT prewarm ... stopped after ...` in the logs. Partial caches must
   remain usable on the next launch. Existing on-demand caches must not hang.
6. Repeat early/middle/late with the **displayed Back to library button**
   (default B), including B immediately after X while active work drains.
   Require `warmup: cancelled; workers drained`, normal shutdown, boot return
   **10**, and library reappearance without starting guest gameplay. No new
   Pulse session or changes to the test program's save sentinel are allowed.
7. Repeat cancel/return/relaunch **10 times**. Hold the displayed skip/cancel
   buttons across launch, then release/repress; reconnect a held pad; try
   player 2 while player 1 remains selected; repeat with swapped face mapping.
   Held buttons must not activate controls, and only the selected pad acts.
   Track app memory and threads: no accumulation, stale workers, late callback
   or painter after the screen is removed. A driver stall may delay a stop;
   record the delay instead of treating the request as instantly completed.
8. Remove the boot opt-in/unset the environment variable and repeat fresh
   launch and Pulse resume. Require the original UI and cache behavior.
   Record Series model, package version, action/phase/progress, stop-to-return
   time, app memory peak and full startup/shutdown logs. Close manual gameplay
   with Q when finished; no timed gameplay or performance certification.
