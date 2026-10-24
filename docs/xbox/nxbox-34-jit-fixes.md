# JIT correctness fixes ported from NXbox

This branch starts at Jadyx `main` (`0fc5c0183694`). Reference repository:
`/Users/miguel/.local/share/nxbox/repo`, read only. The matching inventory is
`nxbox/0c-inventory-c-core:docs/xbox/nxbox-port-inventory-c-core.md`, CPU/JIT rows.
These are instruction/ABI bug fixes, not performance experiments. They require no
new setting, dependency, profile format or migration; existing boot.cfg settings
and stored profiles remain supported. No cache budget or fastmem default changes.

## Validation boundaries

Host: macOS ARM64, Clang C++20. The standalone regressions in
`tools/xbox/tests/jit-*.py` compile extracted production functions or emitter
operations with portable adapters and undefined-behavior sanitization. They do
not execute native x64 code, Windows exception dispatch, or a UWP application.
Windows build and Series tests are pending. No network, PC or console run here.

Existing Jadyx baseline: `python3 -B tools/xbox/tests/pipeline-build-trace.py`,
`python3 -B tools/xbox/tests/address-wait-trace.py` and
`python3 -B tools/xbox/tests/gc-trace.py` pass before changes. The Catch2 application
suite (`src/tests/CMakeLists.txt`) needs the full application/dependencies; no
configured build exists in this worktree. `full-capture-parser.py` requires an
external capture-fixture directory; running without one raises IndexError, so it
is not counted as a runnable regression here. The NXbox tests/port discovery
command does not apply: Jadyx has no tests/port directory.

## Windows x64 exception callback home space

Reference: NXbox `575ccdd7f`. The thunk reserved only the 8-byte alignment slot.
A conforming callback spilling its four register arguments could overwrite the
thunk's return PC. Reserve/restore 40 bytes (32 home + 8 alignment), preserving
the JIT's existing unwind table and callback convention.

`python3 -B tools/xbox/tests/jit-exception-home.py` failed before the fix at the
return-PC sentinel and passes afterward: read and write AVs, four home spills,
saved guest PC/SP and wrapper return checked. This models the ABI; it does not
prove native SEH unwinding through a faulting callback.

Console gate: on the Windows build host first exercise the generated callback
with a native callee spilling all four home slots. On Series, use an original
native diagnostic to dispatch controlled read and write AVs at a registered JIT
site with a known fallback/resume pair. Require one callback each, preserved
return/stack sentinels, and the expected resumed PC. Keep fastmem disabled for
normal original-homebrew boot and library/relaunch smoke tests; do not enable
regional fastmem merely to reach this path. Such a diagnostic is not supplied by
the existing boot NRO; a normal boot alone does not pass this ABI gate.

## Exception fault filtering and disposition

Reference: selected `cb6cc4fef` hunks only, without its fastmem allocator changes.
Forward the exception record in the third callback argument, accept only read or
write AVs with at least two parameters, reject unwind dispatch, and decline an
empty callback/resume address. Return the callback's disposition rather than
unconditionally continuing execution. Declined faults leave the context and
saved stack untouched. This prevents unrelated exceptions from being redirected.

`python3 -B tools/xbox/tests/jit-exception-filter.py` failed before this change
on an unrelated breakpoint invoking the callback. Afterward all nine cases pass:
read/write, breakpoint, short record, execute AV, unwind/exit-unwind, missing call
and missing resume. The home-space regression still passes.

Console gate: extend the original native diagnostic above with all nine cases.
Record callback count, disposition, PC/SP and stack sentinel; only read/write
cases may redirect. Raise a controlled breakpoint under an outer diagnostic
handler, and exercise an unwind: they must reach that handler, not the fastmem
callback. Test deliberately absent call/resume mappings and require continue
search. This requires native Windows/Series exception dispatch; the portable
adapter cannot certify handler chaining.
