// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>

#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"

namespace Tegra {
class MemoryManager;
}

namespace VideoCommon {
template <class P>
class BufferCache;
}

namespace D3D12 {

class Scheduler;
class ShaderCompiler;

/// Guest conditional rendering (render enable) evaluated by the GPU with SetPredication.
///
/// Without it, Maxwell3D reads the guest's reports back on the CPU, which waits for the GPU every
/// time a condition is set. Reports in guest memory the CPU holds are compared on the CPU. Reports
/// the GPU writes (host counters the query cache sums on the GPU, or anything the buffer cache
/// holds as GPU-modified) are compared where the buffer cache holds them by a compute shader
/// writing the predicate, and the guest's draws and clears are predicated on it: reading them on
/// the CPU downloaded them, waiting for the GPU twice a frame in a game (~6.5 ms). A copy of what
/// the shader read lets the CPU decide the same condition again without waiting once the GPU has
/// run it, so draws it skips are dropped unrecorded instead of predicated.
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

    /// The report at address, when the CPU holds it; none when the GPU writes it (a host counter
    /// still pending, or memory the buffer cache holds as GPU-modified).
    std::optional<u64> CpuReport(GPUVAddr address, Tegra::MemoryManager& gpu_memory,
                                 bool& pending_counter);
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
    /// The comparison's result. In PREDICATION while predicate_tick is the list being recorded,
    /// else COMMON (buffers decay to it after each submission).
    ComPtr<ID3D12Resource> predicate;
    u64 predicate_tick{};
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
    /// Why predicated conditions were not read back.
    struct SnapshotMisses {
        u64 none;    ///< no snapshot of the address
        u64 moved;   ///< the reports are in another buffer or at another offset
        u64 written; ///< something wrote the buffer since the copy
        u64 pending; ///< the GPU has not run the copy yet
    };
    SnapshotMisses snapshot_misses{};
    bool logged_gpu_reports{};
};

} // namespace D3D12
