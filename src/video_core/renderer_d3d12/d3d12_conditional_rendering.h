// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>

#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace Tegra {
class MemoryManager;
}

namespace VideoCommon {
template <class P>
class BufferCache;
}

namespace D3D12 {

class HostCounter;
class QueryCache;
class Scheduler;
class ShaderCompiler;
struct BufferCacheParams;
struct PendingReport;
using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

/// Guest conditional rendering (render enable) evaluated by the GPU with SetPredication.
///
/// Without it, Maxwell3D reads the guest's query reports back on the CPU, which waits for the GPU
/// to finish the query (QueryCacheLegacy flushes it) every time a condition is set. Here the
/// condition is reduced to whether one host query slice counted zero, or to a result already known
/// on the CPU; the slice is resolved into a predicate buffer where the guest sets the condition,
/// and the guest's draws and clears are predicated on it. Conditions that need more than one
/// slice are left to the CPU. Reports the GPU wrote to guest memory (not host queries) are
/// compared where the buffer cache holds them by a compute shader writing the predicate: reading
/// them on the CPU downloaded them, waiting for the GPU twice a frame in a game (~6.5 ms). A copy
/// of what the shader read lets the CPU decide the same condition again without waiting once the
/// GPU has run it, so draws it skips are dropped unrecorded instead of predicated.
///
/// D3D12 also predicates copies, clears and dispatches, so predication is only set around guest
/// draws and clears (Scope), never across the texture and buffer caches' own work.
class ConditionalRendering {
public:
    explicit ConditionalRendering(const Device& device, Scheduler& scheduler,
                                  const ShaderCompiler& compiler, QueryCache& query_cache,
                                  BufferCache& buffer_cache);

    /// Evaluates the guest's render enable (RasterizerInterface::AccelerateConditionalRendering).
    /// False when the CPU has to evaluate it.
    bool Accelerate(const Tegra::Engines::Maxwell3D::Regs& regs, Tegra::MemoryManager& gpu_memory);

    /// Whether guest draws and clears are known to be disabled: they are dropped unrecorded.
    [[nodiscard]] bool SkipsDraws() const noexcept {
        return mode == Mode::Skip;
    }

    /// Predicates the guest draw or clear recorded while it lives.
    class [[nodiscard]] Scope {
    public:
        Scope(ConditionalRendering& owner, CommandStream* cmd)
            : list{owner.mode == Mode::Predicated ? cmd : nullptr} {
            if (list) {
                owner.Bind(list);
            }
        }
        ~Scope() {
            if (list) {
                list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        CommandStream* list;
    };

private:
    enum class Mode { Render, Skip, Predicated };

    /// Guest reports at the condition's address: cached queries or values in guest memory.
    /// None when the value is in memory the GPU wrote and the buffer cache has not downloaded.
    std::optional<PendingReport> Report(GPUVAddr address, Tegra::MemoryManager& gpu_memory);
    /// Draws render only when slice counted zero (or only when it did not).
    void Predicate(const HostCounter& slice, bool render_if_zero);
    /// Draws render only when the report at address is not zero, or with compare_second, only
    /// when it differs from the one 16 bytes later (or only when it does not, with render_if_equal).
    /// False when the buffer cache cannot provide the reports.
    bool PredicateOnGpuReports(GPUVAddr address, bool compare_second, bool render_if_equal);
    /// Copies the reports a comparison read into the snapshot of address.
    void RecordSnapshot(CommandStream* cmd, GPUVAddr address, u64 buffer_id, u64 generation,
                        ID3D12Resource* source, u64 offset);
    void CreateComparePipeline();
    void Bind(CommandStream* cmd);

    const Device& device;
    Scheduler& scheduler;
    const ShaderCompiler& compiler;
    QueryCache& query_cache;
    BufferCache& buffer_cache;
    /// The slice's resolved data or the comparison's result. In PREDICATION while predicate_tick
    /// is the list being recorded, else COMMON (buffers decay to it after each submission).
    ComPtr<ID3D12Resource> predicate;
    u64 predicate_tick{};
    u64 value_offset{};
    D3D12_PREDICATION_OP op{};
    Mode mode{Mode::Render};
    ComPtr<ID3D12RootSignature> compare_root_signature;
    ComPtr<ID3D12PipelineState> compare_pipeline;
    /// The reports a comparison read, copied where the CPU reads them once the GPU ran the copy.
    /// A game sets the same condition again before rewriting its reports; while the buffer's
    /// generation is unchanged the CPU decides from the copy and drops skipped draws unrecorded.
    struct Snapshot {
        GPUVAddr address{};
        u64 buffer_id{}; ///< Buffer::Id, never 0 for a valid snapshot
        u64 generation{};
        u64 offset{};
        u64 tick{};
    };
    static constexpr size_t NUM_SNAPSHOTS = 16;
    std::array<Snapshot, NUM_SNAPSHOTS> snapshots{};
    size_t next_snapshot{};
    ComPtr<ID3D12Resource> readback;
    std::span<u8> readback_data;
    u64 read_back_conditions{};
    u64 predicated_conditions{};
    bool logged_gpu{};
    bool logged_gpu_reports{};
    bool logged_cpu{};
};

} // namespace D3D12
