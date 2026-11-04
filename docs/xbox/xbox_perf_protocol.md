<!-- SPDX-FileCopyrightText: Copyright 2026 Jadyx Project -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Performance and stability protocol

A correctness fix that costs frame rate is a regression, not a fix. This protocol exists because one
change copied a whole color target before every draw that sampled it (about 450 copies per frame in a
deferred renderer). It was added for a single suite scene, was never measured in a game, and took a
title from a stable 30 FPS down to 14–22 FPS.

## Rules for renderer changes

1. **Check upstream first.** Before adding per-draw work (a copy, a barrier, a split, a readback), read
   how the Vulkan and OpenGL backends handle the same case. If upstream does nothing, the guest
   probably does not need it either. For example, `TextureCache::CheckFeedbackLoop` skips color
   feedback.
2. **Match guest semantics, not the most conservative reading.** The guest orders a texture fetch
   after render-target writes only across `fragment_barrier`, `tiled_cache_barrier` or
   `invalidate_texture_data_cache`. Code that guarantees more than that is paying for nothing.
3. **Every new per-draw or per-frame correction gets a counter** in the `D3D12 corrections` line
   (`perf_counters.h` → `d3d12_perf_report.cpp`). The gate reads that line. If work has no
   counter, the gate cannot see it.
4. **A suite scene alone never justifies per-draw cost.** A scene that only passes by adding work
   to the hot path is either wrong or needs a cheaper design. Drop it before shipping the cost.
5. **A/B on the console before merging.** Compare the build with the change against the last
   accepted build: same title, same spot, same save. A desktop run proves nothing about Series
   performance.

## The gate

`tools/xbox/perf-gate.py` judges one console log against a baseline for each title. Baselines live in
`tools/xbox/tests/perf-baselines.json` and are keyed by title ID only. It needs no boot.cfg keys,
because every build logs the 300-frame `D3D12 frame times` windows and the `D3D12 corrections`
counters.

```
powershell -File tools\xbox\fetch-logs.ps1 -OutDir build-uwp\gate\<version>
python tools\xbox\perf-gate.py build-uwp\gate\<version>
```

It fails when any of these is true:

| Check | Limit |
|---|---|
| Median p50 frame time over the gameplay windows | baseline + 5 % |
| Median p95 frame time | baseline + 10 % |
| Each correction counter, per frame | baseline × 1.25 + 1 |
| A correction counter the baseline has never seen | any nonzero value |
| `<Critical>` lines (device removed, presentation disabled, failed frame tick) | none |
| Gameplay length (soak) | 10 minutes |

The first window holds boot and JIT warm-up, so it is left out. Windows under 20 FPS are listed
because they are usually the game's own loads. They do not fail the run on their own; the medians
decide. The three longest hitches are printed with their likely cause.

A validation run against the regression showed what the gate catches. The bad build failed on p50
(+42.6 %), on p95 (+39.2 %) and on 492 new color snapshot copies per frame. The fixed build passed.

## Release checklist

1. Build. Grep the new log strings in `jadyx.exe`. Package a clean build (`library=1, play=1`
   only) and install it on the Series.
2. Play each baseline title for at least 10 minutes at the usual spots, then exit normally.
3. Fetch the log and run the gate. A FAIL blocks the merge until it is explained and fixed.
4. Only accept an intended cost increase with a console A/B that shows the frame rate holds.
   Then re-record that title's baseline on purpose:
   `python tools\xbox\perf-gate.py <log> --record --build <version>`
5. Commit the updated baseline together with the change that moved it, so the history explains it.

## Baseline status

| Title ID | Build | p50 | p95 | Note |
|---|---|---|---|---|
| 01007EF00011E000 | 0.3.9.188 | 34.55 ms | 50.09 ms | 2-minute session. Re-record from a 10-minute soak. |
