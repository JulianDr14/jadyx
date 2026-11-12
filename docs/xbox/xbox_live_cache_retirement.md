# Retirement-driven live PSO index cleanup

`LivePipelineCache` indexes weak entries, with a fixed capacity. Previously a full
index was scanned before every request, including exact live hits. The periodic
128-request sweep also ran when no indexed owner had retired.

An indexed entry now owns a shared atomic retirement token. Its destructor sets
the token; bypass entries do not own it. The token outlives the cache if an entry
outlives it. Periodic cleanup keeps its 128-request cadence after retirement, and
a full-index miss can reclaim immediately. The signal is cleared before walking
the index so concurrent retirement requests another pass. Exact full hits no
longer scan the index solely because it is full.

This changes bookkeeping, not PSO identity, capacity, ownership, in-flight build
sharing or draw readiness. Driver compilation itself is unchanged. No Xbox FPS
improvement is claimed; this draft has not been tested on a console.

## Validation

On a Windows x64 MSVC Developer prompt, from the repository root:

```bat
mkdir build-uwp\live-cache-retirement
cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\live-cache-retirement.cpp /Fobuild-uwp\live-cache-retirement\ /Febuild-uwp\live-cache-retirement\gate.exe
build-uwp\live-cache-retirement\gate.exe
```

The gate covers exact hits, 16 concurrent readers at capacity, immediate reclaim,
weak ownership, capacity bypass, hash collisions, failed build retries, 12 rounds
of concurrent retirement, readmission and a consumer outliving the cache.

The shared token is notification only; it retains neither a PSO nor a cache key.
Each Entry gains a shared-pointer field (empty for bypass entries), and each
cache allocates one shared atomic token; the index capacity is unchanged.
There is no new work on the renderer's per-draw ready fast path.
