// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

struct eden_spirv_to_dxil_stream_output;

namespace D3D12 {

class CommandStream;
class Scheduler;
class ShaderCompiler;

/// spirv_to_dxil's stream output entries (eden_spirv_to_dxil_pipeline_v4) as a PSO declaration,
/// with the semantic names of the DXIL output signature. Entries of an unknown semantic are gaps.
[[nodiscard]] std::vector<D3D12_SO_DECLARATION_ENTRY> MakeStreamOutputDeclaration(
    const eden_spirv_to_dxil_stream_output& stream_output);

/// The stream output of a PSO capturing entries (MakeStreamOutputDeclaration), which must outlive
/// it, as do strides (one per buffer). Without rasterization nothing streams to the rasterizer.
[[nodiscard]] D3D12_STREAM_OUTPUT_DESC DescribeStreamOutput(
    std::span<const D3D12_SO_DECLARATION_ENTRY> entries, std::span<const UINT, 4> strides,
    bool rasterize);

/// Transform feedback through D3D12 stream output, for the pipelines that capture it that way
/// (PipelineLayout::StreamOutput).
///
/// Each of the four buffers has a filled-size counter here, the bytes written past its binding
/// (the guest address plus start_offset, as in the Vulkan backend). Maxwell keeps appending to a
/// buffer from one draw to the next while transform feedback stays on, so a counter is only
/// cleared when its buffer registers change or capture starts again after being turned off.
/// The counters are what StreamingByteCount reports, and what DrawAuto draws from.
class StreamOutput {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

public:
    StreamOutput(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler);
    ~StreamOutput();

    StreamOutput(const StreamOutput&) = delete;
    StreamOutput& operator=(const StreamOutput&) = delete;

    /// Transform feedback was turned off: the next capture starts every buffer over.
    void Disable() noexcept {
        active = false;
    }

    /// Sets the stream output targets of a draw that captures transform feedback, with the
    /// buffers the buffer cache bound for it. Records no allocation, so it cannot submit the
    /// command list between the draw state and the draw.
    void Bind(CommandStream* cmd, const Maxwell& regs,
              const std::array<BufferCacheRuntime::TransformFeedbackBinding, 4>& bindings);

    /// The buffer a StreamingByteCount report of stream reads (the first enabled one of that
    /// stream, as in the Vulkan backend), when a capture wrote it with the current registers.
    [[nodiscard]] std::optional<u32> ByteCountSlot(const Maxwell& regs, u32 stream) const;

    /// Copies the byte count of a buffer (ByteCountSlot) to dst, which is in COPY_DEST.
    void CopyByteCount(CommandStream* cmd, u32 slot, ID3D12Resource* dst, u64 dst_offset);

    /// The four counters, 16 bytes apart: the byte count is the first word of each.
    static constexpr u64 COUNTERS_BYTES = 64;

    /// Copies the four counters (COUNTERS_BYTES) to dst, which is in COPY_DEST. A diagnostic; the
    /// next capture binds the targets again.
    void CopyCounters(CommandStream* cmd, ID3D12Resource* dst, u64 dst_offset);

    /// Builds the ExecuteIndirect record (INDIRECT_DRAW_WORDS) of a DrawAuto: byte_count / stride
    /// vertices from vertex 0, one instance. byte_count is a GPU address readable by a shader.
    /// Returns the record's buffer, in INDIRECT_ARGUMENT, at offset 0. Changes the compute root
    /// signature and the pipeline state.
    [[nodiscard]] ID3D12Resource* BuildDrawAutoRecord(CommandStream* cmd,
                                                      D3D12_GPU_VIRTUAL_ADDRESS byte_count,
                                                      u32 stride, u32 yz_flip_mask);

private:
    struct Slot {
        GPUVAddr address{};
        s32 size{};
        s32 start_offset{};
        bool enabled{};
        bool written{}; ///< a capture used the counter since it was cleared

        bool SameBinding(const Slot& other) const noexcept {
            return address == other.address && size == other.size &&
                   start_offset == other.start_offset && enabled == other.enabled;
        }
    };

    /// A buffer of this class: its state only holds within the list that set it.
    struct TrackedBuffer {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS address{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        u64 state_tick{};
    };

    void Transition(CommandStream* cmd, TrackedBuffer& buffer, D3D12_RESOURCE_STATES next);
    void UnbindTargets(CommandStream* cmd);
    void CreateDrawAutoPipeline();

    const Device& device;
    Scheduler& scheduler;
    const ShaderCompiler& compiler;
    std::array<Slot, Maxwell::NumTransformFeedbackBuffers> slots{};
    bool active{};
    u64 targets_tick{}; ///< the list that has the targets bound, so the counters lag behind
    TrackedBuffer counters;
    TrackedBuffer draw_auto_record;
    ComPtr<ID3D12RootSignature> draw_auto_root_signature;
    ComPtr<ID3D12PipelineState> draw_auto_pipeline;
};

} // namespace D3D12
