// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class CommandStream;
class GraphicsPipeline;
class PipelineLayout;
class Scheduler;
class ShaderCompiler;

/// Runs the draws of pipelines that emulate tessellation (Shader::TessellationEmulation,
/// TessEmulationPlan), in four steps recorded one after the other:
///   1. Capture: the vertex shader alone over the draw's vertices, as points, storing each vertex
///      to the capture buffer at its slot (its position in the draw, or its index when indexed).
///   2. Control: the control shader as compute, one record per patch.
///   3. Tessellate: d3d12_tessellate.comp turns each record's levels into domain points and
///      indices (count, prefix sum, generate), and writes the indexed draw record.
///   4. Draw: the evaluation shader as the vertex shader of an ExecuteIndirect over that record.
///
/// The domain point and index buffers grow from what earlier draws needed (the record says it);
/// a draw that needs more than they hold draws the patches that fit.
class TessEmulation {
public:
    TessEmulation(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler);
    ~TessEmulation();

    TessEmulation(const TessEmulation&) = delete;
    TessEmulation& operator=(const TessEmulation&) = delete;

    struct DrawShape {
        u32 num_vertices;
        u32 num_instances;
        u32 first_index;
        u32 base_vertex;
        u32 base_instance;
        bool is_indexed;
        /// Indexed draws: the index buffer the draw reads, the bytes of an index, and the largest
        /// index plus one (the capture slots of an instance).
        D3D12_GPU_VIRTUAL_ADDRESS index_address;
        u32 index_size;
        u32 index_slots;
    };

    /// Step 1, on the draw state BindDrawState bound for pipeline (render targets, root
    /// arguments, the guest's vertex and index buffers). Returns the number of patches, 0 when
    /// there is nothing to draw. Leaves the topology as a point list.
    [[nodiscard]] u32 Capture(CommandStream* cmd, const GraphicsPipeline& pipeline,
                              const DrawShape& shape);

    /// Step 2. The compute root signature (the pipeline's layout) and the control shader's
    /// guest root arguments are set; this sets the emulation's own and dispatches.
    void Control(CommandStream* cmd, const GraphicsPipeline& pipeline, u32 patches);

    /// Step 3. Changes the compute root signature.
    void Tessellate(CommandStream* cmd, const GraphicsPipeline& pipeline, u32 patches,
                    u32 yz_flip_mask, u32 draw_id);

    /// Step 4, with the pipeline's graphics PSO and root arguments bound. Sets the index buffer
    /// and the topology the evaluation shader draws (returned) and writes the runtime data the
    /// record carries.
    D3D12_PRIMITIVE_TOPOLOGY Draw(CommandStream* cmd, const GraphicsPipeline& pipeline);

private:
    /// A buffer of this class: its state only holds within the list that set it.
    struct TrackedBuffer {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS address{};
        u64 size{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        u64 state_tick{};
    };

    /// Queues the buffer's barrier; FlushBarriers records the queued ones with one call, before
    /// the next command that touches them. Each step queues what it leaves behind, so the next
    /// step's barriers go in the same call.
    void Transition(TrackedBuffer& buffer, D3D12_RESOURCE_STATES next);
    void FlushBarriers(CommandStream* cmd);
    void Reserve(TrackedBuffer& buffer, u64 size, bool unordered_access, const char* what);
    void CreateTessellatorPipeline();
    /// Folds the sizes finished draws reported into the point and index needs.
    void ReadNeeds();
    /// gpu_profile=1: a timestamp after each step of one draw in STAMP_EVERY, read back into the
    /// Tess*GpuNs counters once the GPU ran it.
    void Stamp(CommandStream* cmd, u32 step);
    void ReadStamps();

    const Device& device;
    Scheduler& scheduler;
    const ShaderCompiler& compiler;

    TrackedBuffer capture;
    TrackedBuffer records;
    TrackedBuffer counts;
    TrackedBuffer points;
    TrackedBuffer indices;
    TrackedBuffer record;
    boost::container::small_vector<D3D12_RESOURCE_BARRIER, 8> barriers;
    ComPtr<ID3D12RootSignature> tessellator_root_signature;
    ComPtr<ID3D12PipelineState> tessellator_pipeline;

    /// The needs of recent draws, copied from their records (RECORD_NEEDED_POINTS and
    /// RECORD_NEEDED_INDICES) to a persistently mapped readback ring.
    static constexpr u32 READBACK_SLOTS = 64;
    ComPtr<ID3D12Resource> readback;
    const u32* readback_words{};
    std::array<u64, READBACK_SLOTS> readback_ticks{};
    u32 readback_next{};
    u64 needed_points{};
    u64 needed_indices{};

    static constexpr u32 STAMP_SLOTS = 16;
    static constexpr u32 STAMPS_PER_DRAW = 7;
    static constexpr u32 STAMP_EVERY = 16;
    ComPtr<ID3D12QueryHeap> stamp_heap;
    ComPtr<ID3D12Resource> stamp_readback;
    const u64* stamp_data{};
    double stamp_ns_per_tick{};
    std::array<u64, STAMP_SLOTS> stamp_ticks{};
    u32 stamp_next{};
    u32 stamp_slot{};
    u32 stamp_countdown{};
    bool stamping{};
    bool stamps_tried{};

    /// What Capture left for Control: the constants of how the capture is laid out and where
    /// the indices are.
    u32 patches_per_instance{};
    u32 slots_per_instance{};
    u32 index_size{};
    u32 first_index_byte{};
    D3D12_GPU_VIRTUAL_ADDRESS index_address{};
};

} // namespace D3D12
