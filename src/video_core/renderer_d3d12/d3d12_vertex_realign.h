// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>

#include "common/common_types.h"
#include "common/memory_ledger.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_vertex_layout.h"

namespace D3D12 {

class Buffer;
class CommandStream;
class Scheduler;
class ShaderCompiler;
class StagingBufferPool;

/// Repacks vertex streams on the GPU (d3d12_vertex_realign.comp) into the layout a VertexRepack
/// plans, for the input assembler to fetch instead of the guest's bytes.
///
/// A copy is cached by source buffer, range, stride and layout, and reused while the buffer's
/// write log (Buffer::WrittenSince) says nothing touched the range: uploads, copies and GPU
/// writes alike, so a stream a shader rewrites is repacked again and a static one only once. A
/// copy is overwritten in place when it goes stale, ordered after the draws that read it by its
/// barriers. Nothing is created until a stream needs it.
class VertexRealigner {
public:
    /// Largest copy of one stream, and of all of them: past these a stream is bound in place.
    static constexpr u64 MAX_STREAM_BYTES = u64{32} << 20;
    static constexpr u64 MAX_RESIDENT_BYTES = u64{128} << 20;

    VertexRealigner(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                    StagingBufferPool& staging);
    ~VertexRealigner();

    VertexRealigner(const VertexRealigner&) = delete;
    VertexRealigner& operator=(const VertexRealigner&) = delete;

    /// The view of source[offset, offset + size) repacked as plan says, in a state vertex fetch
    /// reads; empty when it is too large (the caller binds it in place). May record a dispatch,
    /// which changes the compute root signature and the pipeline state.
    [[nodiscard]] std::optional<D3D12_VERTEX_BUFFER_VIEW> Repack(Buffer& source, u32 offset,
                                                                 u32 size,
                                                                 const VertexRepack& plan);

    /// Releases the copies no draw used for a while; once per frame.
    void TickFrame();

private:
    static constexpr u32 NUM_SETS = 64;
    static constexpr u32 NUM_WAYS = 2;
    /// Frames a copy stays without being used.
    static constexpr u64 IDLE_FRAMES = 600;

    struct Entry {
        // What the copy holds.
        u64 buffer_id{}; ///< 0: empty
        u64 generation{};
        u64 signature{};
        u32 offset{};
        u32 size{};
        u32 src_stride{};
        u32 bytes{};
        // Bookkeeping.
        u64 last_frame{};
        u64 work{}; ///< Scheduler::Work() of the draw that last bound it
        // Where it lives.
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS address{};
        u64 capacity{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        u64 state_tick{};
        Common::MemoryCharge charge;

        [[nodiscard]] bool Holds(u64 id, u32 offset_, u32 size_, u32 stride,
                                 u64 signature_) const noexcept {
            return buffer_id == id && offset == offset_ && size == size_ && src_stride == stride &&
                   signature == signature_;
        }
    };

    /// The way of the set to overwrite, or null when both are bound by the draw being set up.
    [[nodiscard]] Entry* Victim(u32 set) noexcept;
    /// Gives entry room for bytes; false over MAX_RESIDENT_BYTES.
    [[nodiscard]] bool Reserve(Entry& entry, u64 bytes);
    void Release(Entry& entry);
    void Dispatch(Entry& entry, Buffer& source, u32 offset, u32 size, const VertexRepack& plan);
    void Transition(CommandStream* cmd, Entry& entry, D3D12_RESOURCE_STATES next);
    void CreatePipeline();

    const Device& device;
    Scheduler& scheduler;
    const ShaderCompiler& compiler;
    StagingBufferPool& staging;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<Entry, NUM_SETS * NUM_WAYS> entries{};
    u64 frame{};
    u64 resident_bytes{};
    // Statistics, logged every few seconds while streams are repacked.
    u64 hits{};
    u64 dispatches{};
    u64 dispatched_bytes{};
    u64 refused{};
};

} // namespace D3D12
