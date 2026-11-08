# Bounded additional PSO prewarm below the plentiful-memory cutoff

Candidate, 7 November 2026. Xbox gameplay and performance remain unverified.

## Motivation

A Series X session had 0/135 sampled gameplay memory snapshots eligible for the
existing AppFree >1536 MiB prewarm path. Its last pipeline checkpoint recorded
35.271 s waiting for dormant disk-cache pipelines, versus 0.789 s for evicted
pipelines. Most waits were still for newly encountered pipelines, which this
change cannot prewarm. The release probe had stopped ordinary PSO eviction:
278 releases coincided with only1696KiB lower untracked commit. This heuristic
does not prove PSOs cost no memory on all hardware.

## Policy and integration

Keep the existing plentiful-memory path, boot cap4096 and eviction policy.
Add a GPU-thread-owned allowance of at most128 successful background Wake
requests per PipelineCache session, only when all of these hold:

- AppMemoryUsageLimit is known and at least1024MiB remains.
- GPU budget is known and at least512MiB remains.
- The existing release probe returned DoesNotPay and stopped pressure eviction.
- No allocation OOM was observed in this session.
- The candidate was known directly in the hot set or recognized through a hot
  folded alias during disk loading, and was never evicted.

The allowance uses the same single background worker. Its next queue waits at
least seven times the preceding observed build duration, including during loading
screens that remove the ordinary pacer's rest. The128 cap applies only to the
additional path; ready/in-flight Wake failures do not consume it. It is a work
count, **not a guarantee of how many bytes driver objects can allocate**. Quota
samples can lag concurrent allocations, so the margins still need hardware testing.

Ordinary evicted entries can be skipped as before. Unknown entries and explicit
loading-screen rebuilds retain their queue position for the original path when
memory becomes plentiful; they are not silently discarded by the allowance.
Known entries already precede unknown entries in the disk-prewarm order. This
does not depend on the separate session/frame recency or shader-hash patches.

`D3D12 adaptive prewarm` log lines report the additional queued count and the
number of those pipelines reaching their first draw. A first-draw hit is evidence
of use, not a measurement of milliseconds saved. Shader bytecode, host identity,
draw submission and readiness waits are unchanged.

## Offline validation

- `tools/xbox/tests/renderer-suite/run-unit.ps1 -Name pipeline-residency`:
  329988 app/GPU/probe/OOM combinations; exact byte-boundary and unknown/overrun
  cases;128-queue cap;8 candidate/loading-rebuild cases; minimum rest despite a
  loading-screen override. Existing residency, release-probe, hot-set and latency
  tests also pass.
- The isolated patch was applied to main a0a3fa5ce6, without other local patches.
  Its policy test compiled under MSVC /O2 /W4 /WX and passed independently.
- Release UWP x64 build/link passed for the combined local Canary candidate.
- Desktop production PSO sharing/fence/Wake/readback regression is separate from
  policy tests and does not exercise actual Xbox memory pressure.

In the motivating trace, only6/94 memory snapshots after the probe completed met
the new app/GPU margins. That is an opportunity count, not a replay of new builds:
the extra allocations would change subsequent snapshots, and the queue might
have no eligible candidate then. No FPS improvement or freeze fix is claimed.

## Review and hardware gate

Keep the proposal draft pending Xbox testing. Inspect additional queued/first-drawn
counts, quota headroom, OOM/device errors and dormant first-use waits. The cache
must remain useful rather than spending CPU on guesses. Use matched paths and
cache state to evaluate performance; a warm replay alone cannot isolate this
policy. Do not infer a gameplay speedup from the pure-policy grid.

Design references: the existing PipelineResidencyPolicy, PipelineReleaseProbe,
PrewarmPacer and the Vulkan/shared texture-cache memory-policy separation in
this source tree; [Microsoft MemoryManager](https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager)
and [DXGI QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)
describe the two measured domains. The candidate reuses existing snapshots rather
than querying OS memory once per draw.

Review follow-up: preserve folded hot aliases for the lifetime of the prewarm
queue. Disk ordering and TickPrewarm use the same FoldedHotPipelineSet::Known
predicate. Clear aliases on each disk load without recording synthetic draws in
HotPipelineSet. The regression checks a folded-only head followed by a directly
known candidate and an unknown tail: the first two advance, the unknown tail
still waits, evicted/loading-rebuild rules remain intact, and a later load clears
folded-only knowledge. Hardware execution of this policy is still unverified;
keep this proposal draft.
