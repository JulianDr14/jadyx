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

## Signed rel32 calls

Reference: `d33eeea3d`. `IsWithin2G` accepted forward displacements up to 4 GiB
and rejected valid backward calls. Check the signed 32-bit displacement from the
end of the five-byte call. Out-of-range targets retain the existing full-address
indirect-call fallback.

`python3 -B tools/xbox/tests/jit-rel32.py` failed on the old predicate and now
passes 11 targets: both signed endpoints, adjacent out-of-range values, near
calls in either direction and distances around 4 GiB. It compiles the production
predicate, rather than a duplicate implementation.

Console gate: in an original native x64 diagnostic allocate JIT code and targets
at signed displacements -2147483648, -2147483649, 2147483647 and 2147483648 from
CALL+5 (where address-space placement permits). Emit via `CallFarFunction`;
require near calls only for the two in-range values and successful sentinel
returns for every target. Report unavailable placements as skipped, not passed.
Then boot the original boot NRO, return to library and relaunch twice. A normal
console layout may never exercise a far target; host boundary tests remain
mandatory.

## Full-width SM4 table address

Reference: `3ab0dfe95`, adapted to Jadyx register allocation. Load the table's
64-bit address with MOV, then zero-extend its indexed byte into a 32-bit view.
RIP-relative LEA cannot address a static table arbitrarily far from a JIT mapping.
Both SM4E and SM4EKEY use this lookup.

`python3 -B tools/xbox/tests/jit-sm4.py` failed before the fix on the modeled
out-of-range LEA. Now all 65536 inputs pass, checking every table entry and
ignoring high input bits. The production table and emitter are extracted at run
time. Added Dynarmic Catch2 test `A64: SM4 rounds differential` compares both
variants with Unicorn (512 cases); execution of that x64 suite is pending.

Console gate: an original A64 diagnostic executes SM4E `0xcec08420` and SM4EKEY
`0xce62c820`, with zero, all-one and deterministic random vectors, including
source/destination aliases. Compare all 128 output bits against an independent
SM4 software oracle, print an explicit mismatch count and require zero. On the
Windows host additionally place the JIT more than 2 GiB from its static table and
repeat the Catch2 test. A close console mapping alone cannot validate reach.

## Scalar rounding thunk fields

Reference: rounding hunks of `0e9cc0f0b`. The caller packs rounding mode in bits
0–7 and exactness in bits 8–15; the thunk instead decoded bits 8–15 and 16–23.
Decode the documented fields so FP16 and fallback scalar rounds use the requested
mode and update FPSR only as specified by the instruction.

`python3 -B tools/xbox/tests/jit-round-thunk.py` failed before at the exactness
contract. Afterward 72 contracts pass: production caller packing and thunk
unpacking, three widths, six modes, exact/non-exact and ignored upper bits. The
FP utility is an observing adapter; this checks dispatch, not FP arithmetic.
Added `A64: Scalar rounding differential` to the existing Dynarmic/Unicorn suite:
5376 random cases across FRINTN/P/M/Z/A/X/I and FP16/32/64, with IXC comparison.
That native suite has not run on this Mac.

Console gate: original A64 diagnostic executes those seven scalar FRINT variants
in half/single/double precision with each FPCR rounding mode. Inputs: ±0, ±0.5,
±1.5, ±2.5, finite integers, subnormals, infinities, qNaN and sNaN; clear FPSR
before each instruction. Compare result bits and IOC/IXC/IDC against Dynarmic's
software FP oracle with matching FZ/DN/AHP. Require zero mismatches, including
FRINTX exactness. Keep the diagnostic source original and independent of games.

## Signed-to-unsigned saturating shift fallback

Reference: saturation hunks of `0e9cc0f0b`. Integer promotion made 8/16-bit
shift/reverse-shift overflow checks run at host int width, and `auto result = 0`
truncated wider results. Keep shifted values and results in the unsigned lane
type; narrow explicitly when storing the lane. Negative inputs saturate to zero,
positive overflow to unsigned maximum, setting QC in either case.

`python3 -B tools/xbox/tests/jit-unsigned-saturation.py` failed before on QC.
Afterward 1,444,512 vectors pass against an unsigned 128-bit arithmetic oracle:
exhaustive 8/16-bit inputs at every legal shift, plus boundary/random 32/64-bit
inputs. UBSan reports no issue. Added Dynarmic/Unicorn SQSHLU differential test
(3840 cases across four widths); its native execution is pending.

Console gate: original A64 SQSHLU diagnostic uses every legal immediate for
8/16/32/64-bit lanes, both scalar and vector forms, with zero, -1, signed minimum,
signed maximum, 1, and values immediately below/at/above the overflow threshold.
Compare every output lane and FPSR.QC against a wider-integer saturating oracle;
repeat with QC initially set to verify sticky behavior. Require zero mismatches.
Exercise the fallback without AVX2 on the Windows host as well as the normally
selected console path; this fix does not alter the existing AVX2 lowering.
