// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>

#include "common/common_types.h"

namespace D3D12 {

class Scheduler;
class StagingBufferPool;
struct TracedBuffer;

/// Diagnostic ("cs_inputs="): the storage buffers one compute shader binds, read off the GPU
/// before and after a few of its dispatches into
/// LogDir/cs_inputs_<hash>_<dispatch>_<binding>_<before|after>.bin, for
/// tools/xbox/cs-inputs-report.py. The dispatches are the 32nd to the 35th: warm, and before a
/// hunt_cs capture starts at the 64th, so its timing is not disturbed. 0 turns it off.
void SetComputeInputsShader(u64 hash);

/// Whether `count`, the dispatches of `hash` so far, names one to dump.
[[nodiscard]] bool ComputeInputsWanted(u64 hash, u64 count);

/// Copies each storage binding (at most its first MAX_BYTES) and writes it out, waiting for the
/// GPU: before a dispatch, all of them; after it, the written ones. GPU thread only.
void DumpComputeInputs(Scheduler& scheduler, StagingBufferPool& staging,
                       std::span<const TracedBuffer> bindings, u64 hash, u64 count,
                       std::array<u32, 3> groups, bool after);

} // namespace D3D12
