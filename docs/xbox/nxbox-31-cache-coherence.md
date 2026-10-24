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

## Local baseline

Before changes, the existing macOS-compatible scripts `gc-trace.py`,
`address-wait-trace.py`, `pipeline-build-trace.py`, and `release.py` under
`tools/xbox/tests/` all pass (release tests intentionally print rejection errors).
The inherited NXbox command
`python3 -B -m unittest discover -s tests/port -p "test_*.py" 2>&1 | tail -4`
fails discovery because Jadyx has no `tests/port`; the NXbox baseline of about
three failures/five errors does not apply to this repository.
No UWP/MSVC build or GPU execution is claimed by the host tests.
