# Parallel shader compilation experiment (jx-70)

## Finding from code, 2026-10-24

Jadyx is already parallel. This is a scheduling/signing experiment, with no measured
console speedup yet. The NXbox 1.0.13 result supplied with the task (worst stall
500 -> 60 ms) is not a Jadyx measurement and cannot be extrapolated to Mesa.

| Work | Existing Jadyx | `parallel_shaders=1` |
| --- | --- | --- |
| Pipeline worker count | `min(max(hardware_concurrency, 2) - 1, 4)` | Same CPU allowance, capped at 3 |
| Worker placement | Background, low priority | Default placement, normal priority |
| Graphics Mesa conversion and PSO build | Same worker job; up to 4 concurrent jobs on a host reporting at least 5 threads | Up to 3 pool jobs; independent validators |
| DXIL signing | One shared `IDxcValidator`, mutex across `Validate` and result/error handling | Exclusive reusable instances; no pool mutex during creation, validation or result handling |
| Draw waiting for a queued graphics PSO | GPU thread atomically claims that exact job, bypassing FIFO | Preserved |
| Draw whose graphics build already started | Waits for that worker; no duplicate build | Preserved |
| Dormant PSO prewarm | Separate one-worker background pool, one outstanding PSO | Preserved |
| Disk serialization | Separate one-worker background pool | Preserved |

Counts are static code analysis, not observed Xbox thread counts. A GPU-thread
claim can add a compiler outside the pool; the PSO prewarm thread is also outside
the pool. Three workers is not a global limit of three D3D12 calls.

Relevant production paths:

- `d3d12_pipeline_cache.cpp`: `GetTotalPipelineWorkers`, constructor,
  `CreateGraphicsPipeline`, `CreateComputePipeline`, `LoadDiskResources`.
- `d3d12_graphics_pipeline.cpp`: the build lambda converts signed linked DXIL,
  then builds the PSO. `QueueBuild`/`WaitBuilt` already implement exact-once claim
  for a dependent draw. The `pso_probe` diagnostic intentionally serializes PSO
  creation to measure private commit; leave it disabled for performance tests.
- `d3d12_shader_compiler.cpp`: Mesa calls have no Jadyx-wide conversion mutex.
  The shared-validator mutex was the serial signing section.
- `d3d12_linked_shader_cache.h`: only bookkeeping holds its mutex. Identical
  linked inputs deliberately share one in-flight conversion; different inputs
  can compile concurrently. PSO caches likewise coalesce identical builds.
- `tools/xbox/mesa/eden_pipeline.c`: links a pipeline's stages together. Do not
  translate them independently to imitate an HLSL compiler pool.

First-use Maxwell -> IR -> SPIR-V remains on the GPU thread. First-use compute
also converts Mesa DXIL there, before queuing its PSO; disk-load compute conversion
runs on the loading workers. Compute waiting remains FIFO. A cold graphics draw
must still wait for its exact pipeline, so this does not invent three-way overlap
when only one cold pipeline is available. It targets contention during concurrent
builds, especially disk loading. It cannot shorten a single Mesa conversion by
itself.

## Change and local reference

`boot.cfg` accepts `parallel_shaders=0|1`; omission means **off**. It is applied
before renderer construction and is boot-only. There is no saved user setting,
new UI text, dependency, HLSL backend or compiler optimization change.

The opt-in cap of three and independent compiler resources follow the idea in the
read-only local NXbox reference:
`src/video_core/renderer_d3d12/d3d12_pso_frame_budget.h` (`RuntimePsoThreadCount`,
2 -> 3), `d3d12_pipeline_cache.cpp` (`RuntimePsoThreads`, per-thread DXC caches),
and `d3d12_shader.cpp` (DXC optimization flags). No NXbox source was copied;
Jadyx retains Mesa, linked stages and its signed-DXIL path.

The validator pool checks an idle instance out under a short mutex or creates
one outside the mutex. Validation owns that instance exclusively; success and
exceptions return it without allocation. Idle instances belong to
`ShaderCompiler` and are released before `dxil.dll` unloads, rather than living
in thread-local storage. Retained instance count follows peak simultaneous
signing calls; each creation logs `independent DXIL validator created`.

Normal-priority workers and multiple validator instances may increase memory
and compete with guest CPU execution. The experiment therefore stays off until
Series validation. Disabled mode retains the previous worker count, placement,
shared validator and lock. The exact-draw claim path stays unchanged in both modes.

## Host gates

The worktree started at `main` (`0fc5c0183694d4cc91b062f00a5641315de30003`).
No Windows/UWP compilation, PC app run, network access or console run was performed.

Before and after: all three selected existing host gates pass:

```sh
python3 -B tools/xbox/tests/pipeline-build-trace.py
c++ -std=c++20 -Wall -Wextra -Werror -pthread -Isrc tools/xbox/tests/linked-shader-cache.cpp -o /tmp/jadyx-linked-shader-cache
/tmp/jadyx-linked-shader-cache
c++ -std=c++20 -Wall -Wextra -Werror -pthread -Isrc tools/xbox/tests/pipeline-residency.cpp -o /tmp/jadyx-pipeline-residency
/tmp/jadyx-pipeline-residency
```

New gate, written before implementation: initially fails to compile because the
new policy/pool header is absent; passes after implementation. It tests CPU
reports 0..31, default off, toggle reset, three simultaneous exclusive resources,
100 exception/reuse cycles, factory failure/retry and destruction before owner
unload. A temporary mutation holding one mutex across operations fails its
five-second concurrency assertion, demonstrating detection of serialization.

```sh
c++ -std=c++20 -Wall -Wextra -Werror -Wshadow -Wconversion -pthread -Isrc tools/xbox/tests/parallel-shaders.cpp -o /tmp/jadyx-parallel-shaders
/tmp/jadyx-parallel-shaders
```

The requested NXbox discovery command was run before and after:
`python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`.
Both fail discovery (`tests/port` does not exist in Jadyx). This is not a report
of three failures/five errors, nor a full Jadyx suite pass. The new host test does
not exercise COM, Mesa, D3D12, Windows priorities or MSVC diagnostics; those remain
Windows/console gates.

## Exact console test (pending)

1. On Windows, build this branch with the normal UWP toolchain, then package in
   `build-uwp/package/`; install `build-uwp/package/jadyx.appx` on a Series in
   **Game** mode. Build command:

   ```powershell
   tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
   ```

   Compile with the project's warning policy, including
   C4456/C4457/C4459/C4244 as errors. Keep the same binary for both arms.
2. Preserve saves, keys and firmware. Choose a repeatable route in homebrew or a
   tester-owned dump with first-use shaders. Back up only that title's generated
   shader/pipeline/hot caches with the app stopped. Prepare the same cold cache
   state for every cold trial; never compare a cold baseline with a warm candidate.
3. In the package's `boot.cfg` (next to `boot.nro`, before packaging), use the
   same title selection and these lines for baseline:

   ```ini
   play=1
   parallel_shaders=0
   ```

   Package from PowerShell with the existing script (it writes boot.cfg):

   ```powershell
   .\tools\xbox\package-appx.ps1 -Library -BootCfg "parallel_shaders=0" -Version $baselineVersion
   .\tools\xbox\package-appx.ps1 -Library -BootCfg "parallel_shaders=1" -Version $candidateVersion
   ```

   Run each packaging command immediately before installing that arm. Set the
   version variables to successive four-component versions above the installed
   package, incrementing the fourth component for every delivery. This changes
   only the staged manifest; retain the same executable. The library package
   already supplies `play=1`. Never uninstall or delete saves to force a downgrade.

   For the candidate change only `parallel_shaders=1`, repackage/install, then
   fully restart the app. Leave `pso_probe` off, preserve all other options,
   memory cap and cache policy. The boot diagnostic must echo the selected
   value and `pipeline cache ready` must report the actual worker count and
   `parallel shaders true/false`.
4. Alternate baseline/candidate for at least three cold trials per arm, following
   the same route. Gameplay has no timeout or scripted input; close with Q when
   the manual test is complete (or normal in-app exit without a keyboard).
   Also compare warm starts from identical copies of the generated cache to
   exercise overlapping disk rebuilds. Check rendering, transitions back to the
   library and a second title launch for crashes, hangs or resource leaks.
5. Save `jadyx_log.txt` and `jadyx_diag.txt` separately for every arm/trial. Collect:

   | Quantity | Comparison |
   | --- | --- |
   | D3D12 frame times p50/p95/p99 and worst frame | Cold route and warm route separately |
   | Pipeline stages `frontend`, `queue`, `dxil`, `pso`, `wait-new`, `wait-dormant`, `wait-evicted` | Count, p50/p95/p99, max and total from `D3D12 pipeline stages` at boot/shutdown |
   | Inline builds, prewarmed PSOs, actual worker count | Verify scheduling and equivalent work |
   | Shader-ready boot duration | Compare identical warm cache inputs |
   | Peak app commit / minimum remaining memory | Stay below the console's 5120 MiB limit |
   | Validator creation lines | Count per renderer lifetime; investigate growth across reuse |
   | Render errors, validation failures, device removal, crashes | Must not regress |

   If stage totals suggest signing contention, take a separate existing T trace
   in both modes and run `tools/xbox/analyze-frame-trace.py` on the logs. Compare
   `pipeline-validator-lock-long`, `pipeline-sign-long`,
   `pipeline-translate-long`, `pipeline-worker-long`, and `pipeline-wait-long`.
   In enabled mode the validator-lock span measures checkout plus lazy creation,
   not a mutex held during signing. Keep trace overhead out of the main FPS trials.

Do not enable by default from a host test, an isolated boot speedup or NXbox's
reported 500 -> 60 ms result. Require lower cold-route stalls without rendering,
memory, warm-route or CPU-contention regressions on Series X and Series S.
