// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <atomic>
#include <fstream>
#include <fmt/format.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_compute_inputs.h"

namespace D3D12 {
namespace {

constexpr u64 FIRST_DISPATCH = 32;
constexpr u64 DISPATCHES = 4;
/// A job list or a counter table is far smaller; a large binding is kept to its start.
constexpr u32 MAX_BYTES = 4U << 20;

std::atomic<u64> inputs_shader{};

} // Anonymous namespace

void SetComputeInputsShader(u64 hash) {
    inputs_shader.store(hash, std::memory_order_relaxed);
}

bool ComputeInputsWanted(u64 hash, u64 count) {
    return hash != 0 && hash == inputs_shader.load(std::memory_order_relaxed) &&
           count >= FIRST_DISPATCH && count < FIRST_DISPATCH + DISPATCHES;
}

void DumpComputeInputs(Scheduler& scheduler, StagingBufferPool& staging,
                       std::span<const TracedBuffer> bindings, u64 hash, u64 count,
                       std::array<u32, 3> groups, bool after) {
    const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const char* const when = after ? "after" : "before";
    u32 binding = 0;
    for (const TracedBuffer& source : bindings) {
        if (source.kind != TracedBuffer::Kind::Storage) {
            continue;
        }
        const u32 index = binding++;
        if (after && !source.written) {
            continue;
        }
        const u32 size = std::min(source.size, MAX_BYTES);
        if (size == 0 || !source.buffer || !source.buffer->Handle()) {
            LOG_INFO(Render, "D3D12 cs_inputs: {:016x} dispatch {} binding {} {}: nothing bound",
                     hash, count, index, when);
            continue;
        }
        StagingBufferRef readback = staging.Request(size, MemoryUsage::Download, true);
        if (readback.mapped_span.size() < size) {
            staging.FreeDeferred(readback);
            LOG_WARNING(Render, "D3D12 cs_inputs: no {} bytes of readback for binding {}", size,
                        index);
            continue;
        }
        Buffer& buffer = *source.buffer;
        const auto previous = buffer.State();
        buffer.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        scheduler.CommandList()->CopyBufferRegion(readback.buffer, readback.offset,
                                                  buffer.Handle(), source.offset, size);
        // Buffers decay on ExecuteCommandLists; restoring before Finish is safe.
        buffer.Transition(previous, false);
        scheduler.Finish();
        const std::string name = fmt::format("cs_inputs_{:016x}_{}_{}_{}.bin", hash, count, index,
                                             when);
        std::ofstream{directory / name, std::ios::binary | std::ios::trunc}.write(
            reinterpret_cast<const char*>(readback.mapped_span.data()), size);
        staging.FreeDeferred(readback);
        LOG_INFO(Render,
                 "D3D12 cs_inputs: {:016x} dispatch {} groups {}x{}x{} binding {} {} @{:x}+{} "
                 "size {} written {} -> {} ({} bytes)",
                 hash, count, groups[0], groups[1], groups[2], index, when, source.device_addr,
                 source.offset, source.size, source.written, name, size);
    }
}

} // namespace D3D12
