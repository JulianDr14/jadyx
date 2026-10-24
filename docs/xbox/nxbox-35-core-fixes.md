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
writers. MapPages releases the writer lock before its caller performs host map/unmap
operations, so those operations do not nest a reverse acquisition. No new recursive locking is introduced.

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

## 2. Exception-safe destructive ScratchBuffer growth

Reference: NXbox `e178234e2bc37fe418a63eeb7b3b8e0345c5f517`.

`resize_destructive` raised capacity before allocating. A failed allocation left
an empty/undersized buffer claiming the new capacity, and a retry skipped the
allocation. Allocate first, then publish capacity. Ownership, previous size,
capacity and data now remain intact on failure. Successful destructive growth
still makes no promise to preserve previous contents.

`run.py scratch-buffer` fails before the fix on the capacity/size invariant.
Afterwards it passes with a deterministic throwing element-array allocator:
empty and populated buffers, two failed retries each, preserved pointer/data,
successful recovery, shrink/regrow without allocation, resize to zero and move.
It includes the real header and requests only 32 elements, never actual OOM.

Console test (pending): build/package as in section 1. Run the original 600-frame
homebrew to completion, return to library and launch it ten more times. Require
unchanged output, successful diagnostics and no Render/Critical error. Exercise
normal rendering/texture upload paths with an owned local title if available;
leave gameplay duration under manual control. Do not deliberately exhaust console
memory. The host throwing-allocation test establishes the failure guarantee;
the ordinary console smoke cannot establish that guarantee by itself.

## 3. BSD Read receives socket data

Reference: NXbox `52f2eb0c0f8678ed44cf859507899ecaba95ac6e`.

BSD Read always returned success with zero bytes, fabricating EOF on a live
socket. Delegate to Jadyx's existing deferred RecvWork with flags zero and an
output vector sized to the IPC buffer, exactly as Recv does. Do not introduce a
second socket/error/blocking implementation. No service endpoint, online default
or network feature is added.

`run.py bsd-read` fails before the change: four synthetic available bytes produce
return zero. After the change it passes eight calls: payload, bounded partial
receive, zero-sized output, real EOF, two invalid descriptors, would-block and
interrupted receive. It executes the production Read, RecvWork Execute/Response
and RecvImpl with a fake socket/IPC scheduler. Descriptor nonblocking state and
existing errno/response shape are preserved. No host socket or peer is opened.

Console test (pending; original diagnostic homebrew and separately authorized
controlled local peer required):

1. Build/package as in section 1. Start a peer on the private LAN serving the
   literal eight bytes `JADYX35!`, then closing. Use an original homebrew client
   calling BSD Read (command 25), with a three-byte receive buffer, until EOF.
   Require concatenated `JADYX35!`, positive partial returns and zero only after
   peer close; sentinel bytes around the IPC buffer must remain unchanged.
2. Have the peer hold the connection open with no data. A blocking Read must
   remain pending, then complete when the peer sends one byte. With the socket
   nonblocking, require would-block and unchanged flags; invalid fd must report
   BADF. Exercise an interrupted pending receive using the platform's supported
   interruption path and compare to Recv flags=0 on the same setup.
3. Repeat after returning to library/relaunching the diagnostic, and with the
   normal original boot homebrew. Require no stuck worker, crash or unexpected
   error. These payload/peer fixtures are a future console gate; the mock host
   tests do not establish actual Xbox network permission or wake behavior.

## 4. Validate FSP IStorage reads

Reference: NXbox `166a71fa55162423dafd0dc8160c5ccc2405d579`.

Read checked only the signs of offset/length, could overrun the IPC output buffer
or backend range, and ignored the actual bytes read. Preserve the existing
negative-size/offset errors; reject length exceeding the IPC buffer with
ResultInvalidSize, and a range outside GetSize with ResultOutOfRange. Bounds
use `offset <= size && length <= size - offset`, avoiding signed/unsigned sum
overflow. Only after validation convert arguments to the backend's size_t.
Zero-length requests within the storage (including its end) succeed without
calling Read. A short backend read returns ResultUnknown and logs requested vs
actual length, rather than claiming a complete successful transfer. On error a
backend may have written a prefix; no rollback or atomic-buffer guarantee is
introduced. Pulse opener metadata/capture and GetSize remain untouched.

`run.py storage-read` fails before the fix: an empty backend result for an
eight-byte request returns success. After the fix it passes exact/short/zero
backend reads, negative arguments, small output buffers, storage-end and
out-of-range cases, 64-bit offsets above 4 GiB and a valid range crossing signed
INT64_MAX. Another **726 offset/length combinations** (66 offsets × 11 lengths)
check error precedence, absence of backend calls for invalid/empty requests and
sentinel preservation. All bytes in rejected buffers remain unchanged.

Console test (pending; original diagnostic payload required):

1. Build/package as in section 1. Give a diagnostic homebrew an original 64-byte
   data storage, bytes 0 through 63, opened with FSP OpenDataStorageByCurrentProcess
   (command 200). Call IStorage Read (command 0) with an eight-byte IPC output
   buffer surrounded by sentinels. Offset 3/length 8 must return bytes 3..10;
   offset 56/length 8 must return bytes 56..63.
2. Require InvalidSize for length -1 or 9, InvalidOffset for offset -1,
   OutOfRange for offset 60/length 8, offset 65/length 0 and INT64_MAX/length 8.
   Offset 64/length 0 must succeed. Compare error codes to FileSys constants and
   verify sentinels after every call. Repeat the 726-case host matrix through
   guest IPC. Oversized IPC buffers with a smaller requested length must succeed.
3. A host-side console diagnostic using a synthetic VfsFile reporting size 64
   but returning 0 or 7 bytes for Read(8, 0) must return ResultUnknown. This
   requires a diagnostic wrapper; do not damage a real file or simulate a
   console storage fault. Repeat with a fake >4 GiB VfsFile to check wide offsets.
4. Open the original homebrew storage, save a Pulse session, return to library,
   restart and resume, including a session produced before these fixes. Require
   the same bytes/state, no restore rejection, no crash or Critical log. Ordinary
   boot and Pulse smoke alone do not test short backend reads or wide offsets.

## Final host results and remaining gates

All four new regressions failed on their unfixed production code and pass with
the corresponding fix. All eight existing macOS checks listed above pass again;
no new failure. The inherited tests/port command still reports the same missing
start directory. No FPS, memory-saving or console-compatibility result is claimed.

C++ fixtures were formatted with the repository's `src/.clang-format`; the Python
runner uses the locally cached Ruff formatter, without fetching a package. New
service work aggregates use named fields. No package version is bumped because
this task does not create a deliverable UWP package; a future release must use
its normal version/symbol discipline. The four fixes are separate local commits.
The Windows warning/build gate, real IPC/worker scheduling, console diagnostics,
normal boot and old-session Pulse checks remain unexecuted. No push or network.
