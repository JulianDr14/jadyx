<!-- SPDX-FileCopyrightText: Copyright 2026 Jadyx Project -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# R038/R039: guest write cache coherence

Branch: `nxbox/31-cache-coherence`, based on `main` at `0fc5c01836`.
Reference: renderer inventory on `nxbox/0b-inventory-b-renderer`, rows R038/R039.
NXbox was inspected read-only; no network, Windows PC or console was used.

## R038: software DMA constant fills

Adapted the ownership distinction from NXbox `374695e93`, without its unrelated
presentation changes or tracing. Jadyx previously ignored `BufferClear`'s return
value and always used `WriteBlockUnsafe`. A declined clear therefore changed guest
RAM while buffer/texture caches could retain old bytes.

Only a successful four-byte accelerated clear now uses the unsafe RAM mirror.
Declined acceleration and one/two-byte components use `WriteBlock`, whose existing
mapped-page path invalidates the affected caches. Component packing, byte counts,
semaphore handling and the accelerated path are preserved. This is a correctness
fix, enabled unconditionally; no settings or saved-data formats change.

Host regression: `python3 -B tools/xbox/tests/cache-coherence.py -k dma`.
It compiles the actual production fill block against synthetic memory/cache
adapters using C++20, ASan/UBSan and warnings as errors. Eighteen cases cover
component widths 1/2/4, lengths 1/3/8 and accepted/declined acceleration. Checks
include byte-exact writes, untouched tails, cache visibility and accelerated RAM
mirroring. The original code aborts on the safe-write assertion; the fix passes
all 18 cases. Narrow-fill bytes deliberately retain Jadyx's existing layout.

### Console acceptance test (pending)

Use an original synthetic homebrew command-stream workload, with no game assets.
This protocol requires such a workload; the host fixture is not a console NRO.

1. On Series X|S Dev Mode, install the eventual normal package in Game mode.
   Keep an existing configuration and save/session data to check compatibility.
2. Allocate a mapped 4 KiB scratch range; initialize it to `0x11223344`, bind it
   as both a buffer and a linear RGBA8 texture, draw/sample it, and wait for GPU
   completion so both caches contain the old bytes.
3. Submit single-line Maxwell DMA with `remap_enable=1`, `dst_x=CONST_A`,
   `remap_consta_value=0x591642c8`, `line_length_in=8`, and component width 4.
   Exercise once with buffer acceleration accepted and once with acceleration
   declined (a diagnostic test adapter must force the latter). Do not add a CPU
   flush/invalidation between DMA completion and the next buffer/texture use.
4. After each fill, sample/read the same cached resources and read guest RAM;
   compare the first 32 bytes to eight copies of `c8 42 16 59`. Verify the rest
   of the scratch range still contains the initialization pattern.
5. Repeat with widths 1 and 2, expecting respectively 8 and 16 bytes from the
   same repeated word byte stream. Repeat lengths 1 and 3. Require exact matches
   for all 18 host-equivalent cases, no Render errors/device removal, and normal
   return to the library. Reopen an existing title/session without resetting data.

The repository's current boot NRO does not implement this adversarial workload;
ordinary boot/gameplay alone cannot certify this fix on the console.

## R039: drained inline writes bypass the CPU skip option

Adapted NXbox `02479f68c` to Jadyx's existing batched override. Removed only the
early return controlled by `skip_cpu_inner_invalidation`. `FlushCaching` drains
GPU command writes, including `Maxwell3D::ProcessCBMultiData`'s cached constant
buffer writes; dropping those notifications permanently loses their cache
ownership transition. Vulkan already invalidates these batches unconditionally.

Texture/buffer locking remains once per batch; query and pipeline invalidation,
range order, and Jadyx's null-address/zero-length guards are unchanged. This is
also an unconditional correctness fix. The existing setting still loads with its
old key/type/default and retains its meaning in other invalidation paths. No
boot.cfg, translation, persistent profile or Pulse format changes are needed.

Host regression: `python3 -B tools/xbox/tests/cache-coherence.py -k inline`.
The fixture compiles the production bodies of `WriteBlockCached`, `FlushCaching`
and D3D12 `InnerInvalidation`. Synthetic adapters model address translation,
accumulation and cache upload eligibility; this is not a full memory-manager or
GPU test. Both skip values execute two rounds of three writes, including overlap
and a disjoint range (12 writes total). All four cache adapters must see the
actual new bytes, with exact translated ranges. Empty drains cannot replay old
ranges, and invalid address/size pairs cannot reach cache methods. The original
code aborts on stale cached bytes with skip enabled; after the fix both settings
pass. Batch lock counts are checked separately.

### Console acceptance test (pending)

Use an original synthetic command-stream NRO or a diagnostic test adapter; the
current boot NRO does not yet implement this workload.

1. Install the eventual package in Game mode on Series X|S. Run the workload twice,
   setting `Settings::values.skip_cpu_inner_invalidation` to false, then true in
   the diagnostic adapter before renderer creation. Do not change the setting's
   persisted format or reset user data.
2. Allocate 64 mapped bytes and initialize them to zero. Bind/sample the range as
   a buffer and texture so both caches contain zeros. Wait for completion.
3. Through Maxwell3D constant-buffer data commands (`ProcessCBMultiData`), write
   twelve `0x53` bytes at offsets 0, 8 and 40, without a CPU invalidation command.
   Issue a draw that consumes the range, exercising the normal pre-draw
   `FlushCaching` path. Require the first 20 and bytes 40..51 to equal `0x53`,
   with all other bytes zero, in guest RAM and both GPU consumers.
4. Repeat with `0xa7`, without recreating caches or resources. Repeat an otherwise
   identical draw without writes, checking that no stale batch is replayed.
5. For each skip value require byte-exact buffer/texture results and normal
   library return, with no Render errors/device removal. A diagnostic adapter
   must also verify affected query/shader ranges become invalidated. Resume an
   existing session afterward; retain existing data and configuration.

No console result, frame-rate gain, MSVC build or GPU correctness is claimed.

## Local baseline

Before changes, the existing macOS-compatible scripts `gc-trace.py`,
`address-wait-trace.py`, `pipeline-build-trace.py`, and `release.py` under
`tools/xbox/tests/` all pass (release tests intentionally print rejection errors).
The inherited NXbox command
`python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`
fails discovery because Jadyx has no `tests/port`; the NXbox baseline of about
three failures/five errors does not apply to this repository.
No UWP/MSVC build or GPU execution is claimed by the host tests.

After both fixes, all four selected existing scripts still pass, and
`python3 -B tools/xbox/tests/cache-coherence.py` passes both compiled regressions
with ASan/UBSan and `-Wall -Wextra -Wshadow -Wconversion -Werror`.
The inherited `tests/port` discovery command still reports the same missing
directory. `git diff --check` passes. No dependencies were added.
