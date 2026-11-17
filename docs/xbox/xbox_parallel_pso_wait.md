# Parallel graphics PSO waits on D3D12Recorder

With `async_pipelines=off` ("Shaders: Wait") and `async_record=1`, `DrawRun` previously
waited inline on the GPU thread via `WaitPending()` for each unbuilt graphics pipeline
before recording `SetPipelineState` and the draw call. In frames that hit several cold or
dormant pipelines back-to-back, the GPU thread claimed the first unclaimed build and
blocked until it finished, leaving the other background pipeline worker threads idle for
those draws or draining them sequentially instead of letting the recorder thread and
workers overlap compilation with state/draw setup.

When `pso_parallel_wait=1` (enabled by default whenever `async_record=1`) and a draw does
not involve tessellation, split-stencil faces, or GPU diagnostics/captures:

- `DrawRun` checks the acquire-guarded `GraphicsPipeline::Handle()`. While a build is in
  flight, state and resource bindings are recorded normally, `BindDrawState` calls
  `Scheduler::ForgetPipelineState()`, and `RecordDraw` queues a lambda on `D3D12Recorder`
  capturing `GraphicsPipeline::RecordedDrawRef` and `DrawParams`. Inside the worker
  recorder, `WaitHandle()` waits for the PSO (if still pending) and emits
  `SetPipelineState` followed by the draw in exact FIFO command order. Rejected pipelines
  return `nullptr` from `WaitHandle()` and skip the draw cleanly.
- `GraphicsPipeline::RecordedDrawRef` is copyable and reference-counts
  `pending_recorded_draws` so `GraphicsPipeline::Evict()` will not evict or drop DXIL
  while a recorded chunk referencing that pipeline is still queued.
- `PipelineCache::NoteDeferredWait()` tracks the unique pending pipelines for the batch.
  Before blocking in `Scheduler::Wait()` (on the recording thread), in
  `PipelineCache::TickResidency()`, and in `~PipelineCache()`, `DrainDeferredWaits()`
  raises active builders to at least Normal priority and lets the GPU thread claim any
  remaining unclaimed builds **from the tail backward** while background workers drain
  from the front.
- `Scheduler::DrainForShutdown()` also discards and drains any unsubmitted recorder
  chunks on removed-device or teardown failure before `~PipelineCache()` runs so all
  `RecordedDrawRef` handles release before pipelines are destroyed.
- `boot.cfg` switch `pso_parallel_wait=0` restores synchronous inline waits for A/B testing.

## Validation

On a Windows x64 MSVC Developer prompt after building `common.lib` and `fmt.lib`:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\xbox\tests\renderer-suite\run-unit.ps1 -Name parallel-pso-wait
```

The standalone concurrency gate covers `RecordedDrawRef` copy/move eviction pinning,
250-draw multi-chunk FIFO command order across mixed ready/deferred/duplicate/rejected
pipelines, and reverse helper draining across 4 workers alongside true per-pipeline build
execution.
