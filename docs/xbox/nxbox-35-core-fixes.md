# NXbox core correctness ports (jx-35)

Jadyx branch `nxbox/35-core-fixes` starts at main `0fc5c0183694`.
Reference source is the read-only GPL-3.0-or-later NXbox checkout, inventory C rows
“Serialize normal and JIT page-table writers”, “ScratchBuffer exception-safe
destructive growth”, “BSD Read delegates to receive”, and “FSP IStorage validates
output size/range and short reads”. These are pure bug fixes, enabled without a
new setting. Existing boot configuration, stored profiles and Pulse wire data
keep their formats. No new dependency or imported guest data.

## Host baseline (macOS, 2026-10-24)

Before changes, these eight existing checks pass:

```sh
for test in pipeline-build-trace gc-trace draw-preparation-report address-wait-trace material-trace; do
  python3 -B tools/xbox/tests/$test.py
done
for test in prewarm-budget heap-packing cache-pressure; do
  clang++ -std=c++20 -Isrc tools/xbox/tests/$test.cpp -o /tmp/jx35-$test
  /tmp/jx35-$test
done
```

Heap packing covers 1,248,000 aligned placements. The inherited command
`python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`
reports `ImportError: Start directory is not importable: 'tests/port'` here.
NXbox's failure counts do not apply to Jadyx. No network, Windows/PC runtime,
UWP build or Xbox run is performed by this task.

New host regressions run with:

```sh
python3 -B tools/xbox/tests/core-fixes/run.py
```

The runner compiles the actual production method bodies (and the actual
PageTable declaration), with synthetic IPC, storage, host protection and sparse
allocation boundaries. It does not duplicate the algorithms under test. This
isolates the bugs on macOS; it does not prove UWP scheduling or ABI behavior.
Compiler checks include `-Wall -Wextra -Werror -Wshadow -Wconversion` (signedness
warnings excluded for inherited code). MSVC C4456/C4457/C4459/C4244 remain a
required Windows build gate; clang is not a substitute for that gate.

## 1. Serialize page-table writers

Reference: NXbox `39fbd92cc4b581859a7fa38cae7f999411f83d0b`.

Canonical metadata and the clean JIT slot were independently atomic, but two
writers could interleave their updates and publish a stale JIT pointer. Add one
`writer_mutex` to each Common::PageTable, held once over MapPages,
MarkRegionDebug and RasterizerMarkRegionCached, including host protection.
Readers remain lock-free. This serializes writers, not the guest's in-flight
loads against unmapping; existing guest synchronization is still required.
Resize remains a process-quiescent lifecycle operation. Pulse maps through the
existing kernel/memory paths; no mutex or page-table layout is serialized.

Lock order is writer_mutex then host protection's placeholder mutex. The
inspected HostMemory protection implementation does not call back into these
writers. Mapping's host map/unmap operations finish before MapPages acquires the
writer lock. No new recursive locking is introduced.

`run.py page-writers` fails before the fix: map/unmap completes while a
rasterizer writer is paused inside host protection. After the fix, both debug
and rasterizer directed overlap cases block the competing mapper, then finish
with matching canonical/JIT slots. A further 100 joined rounds execute 50,000
writer calls over four pages, checking equivalence after each round and after a
final unmap. Sparse backing/host protection are mocked, actual entry atomics and
all three production method bodies are used. No FPS or contention benefit is
claimed.

Console test (pending; requires an original diagnostic payload):

1. Build with MSVC warning gates, package through the normal
   `tools/xbox/package-appx.ps1` flow into `build-uwp/package/jadyx.appx`, archive
   matching symbols and install in Game mode on Series X/S.
2. In a console-callable host diagnostic, initialize a four-page mapping and run
   the same 100-round map/remap vs debug/cache-mark workload used by
   `page-writers.cpp`. Join writers before checking all four canonical/JIT pairs;
   then unmap and require zero JIT slots. Run the two paused-protection cases;
   require the competing mapper to wait until protection is released. Do not
   execute guest loads while deliberately unmapping their backing memory.
3. Run the original Pulse homebrew: boot its 600-frame pattern, save to library,
   restart and resume; repeat ten times including an existing pre-fix session.
   Require the expected pattern/state, no deadlock, no stale-access crash and no
   Pulse restore rejection. Collect jadyx log/diag. The current ordinary boot
   payload alone does not exercise concurrent debug/cache marks; implementing
   and running the diagnostic wrapper in step 2 remains a console gate.
