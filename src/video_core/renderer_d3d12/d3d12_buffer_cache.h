// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "common/memory_ledger.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/renderer_d3d12/d3d12_index_rewrite_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_uniform_stream.h"

namespace D3D12 {

class CommandStream;
class Scheduler;
class BufferCacheRuntime;
class GuestDescriptorQueue;

/// Opt-in regular SSE2 stores for short uploads, for subsequently created runtimes.
void SetUniformUploadCopy(bool enabled);
/// Opt-in rebinding of a streamed uniform buffer whose bytes did not change within a submission,
/// for subsequently created runtimes.
void SetUniformReuse(bool enabled);
/// Draws recorded since boot with an enabled vertex slot the command list did not hold (a list
/// restarted inside the draw).
[[nodiscard]] u64 LostVertexSlotDraws() noexcept;

class Buffer : public VideoCommon::BufferBase {
public:
    explicit Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams);
    /// sparse_compatible is ignored: D3D12 buffers here are committed, not reserved resources.
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr, u64 size_bytes,
                    bool sparse_compatible = false);
    /// The resource outlives the Buffer until the GPU is done with it.
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) noexcept = default;
    Buffer& operator=(Buffer&&) noexcept;

    [[nodiscard]] ID3D12Resource* Handle() const noexcept { return buffer.Get(); }
    /// Read once at creation: GetGPUVirtualAddress is a call into the runtime, and draws take the
    /// address of every index buffer and persistent uniform buffer they bind.
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Address() const noexcept { return address; }
    operator ID3D12Resource*() const noexcept { return Handle(); }
    /// The generic cache marks what each copy and binding touches, for backends that reorder
    /// uploads ahead of the draws using a buffer. Uploads are never reordered here (see
    /// CanReorderUpload): tracking it took ~3% of the GPU thread in a heavy scene.
    void MarkUsage(u64, u64) noexcept {}

    /// Records a barrier to next unless the buffer is already there (a UAV barrier for UAV ->
    /// UAV). Buffers decay to COMMON at the end of every ExecuteCommandLists, so the tracked state
    /// only holds within the list that set it.
    void Transition(D3D12_RESOURCE_STATES next, bool mark_written = true);
    [[nodiscard]] D3D12_RESOURCE_STATES State() const noexcept { return state; }
    /// State as the current command list sees it: COMMON when it was set in an earlier list.
    [[nodiscard]] D3D12_RESOURCE_STATES CurrentState() const noexcept;
    /// Whether a binding of this draw or dispatch (Scheduler::Work) made it a UAV.
    [[nodiscard]] bool WrittenByWork(u64 work) const noexcept {
        return work != 0 && uav_work == work;
    }

    /// Unique among the buffers of a runtime, never 0 (IndexRewriteCache keys).
    [[nodiscard]] u64 Id() const noexcept { return id; }
    /// Bumped by every write to the buffer: uploads, copies and UAV use (Transition to a write
    /// state), so an unchanged generation means unchanged contents.
    [[nodiscard]] u64 Generation() const noexcept { return generation; }
    /// Bytes the next write (Transition to a write state) covers. Without it a write counts as
    /// the whole buffer: a UAV or stream output binding says nothing about its range.
    void ExpectWrite(u64 begin, u64 end) noexcept {
        pending_begin = begin;
        pending_end = end;
    }
    /// Whether a write after `since` may have touched [begin, end). True when the writes since
    /// then are more than the buffer remembers.
    [[nodiscard]] bool WrittenSince(u64 since, u64 begin, u64 end) const noexcept {
        if (generation - since > NUM_WRITES) {
            return true;
        }
        for (u64 at = since + 1; at <= generation; ++at) {
            const Write& write = writes[at % NUM_WRITES];
            if (write.generation != at || (write.begin < end && begin < write.end)) {
                return true;
            }
        }
        return false;
    }

private:
    /// Hands the host buffer back to the runtime (BufferCacheRuntime::RecycleBuffer).
    void ReleaseBuffer();

    BufferCacheRuntime* runtime{};
    Scheduler* scheduler{};
    u64 id{};
    u64 generation{};
    /// The last writes by generation, for CheckIndexRange.
    struct Write {
        u64 generation{};
        u64 begin{};
        u64 end{};
    };
    static constexpr u64 NUM_WRITES = 16;
    std::array<Write, NUM_WRITES> writes{};
    u64 pending_begin{};
    u64 pending_end{}; ///< 0: the next write has no known range
    ComPtr<ID3D12Resource> buffer;
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    u64 state_tick{}; ///< scheduler tick of the list that set state
    u64 uav_work{};   ///< Scheduler::Work() that last made it a UAV
    u64 read_work{};  ///< Scheduler::Work() that last took it from UAV to a read state
    Common::MemoryCharge charge;
};

/// A buffer bound for a traced draw (RasterizerD3D12::SetDrawTrace), checked against guest memory.
struct TracedBuffer {
    enum class Kind { Uniform, NullUniform, StreamedUniform, Vertex, Index, RewrittenIndex,
                      Storage, Texture, Image };
    Kind kind{};
    u32 slot{};
    Buffer* buffer{}; ///< null for staging memory and null bindings
    u32 offset{};
    u32 size{};
    u32 stride{}; ///< vertex stride or index size
    DAddr device_addr{};
    const u8* mapped{}; ///< what the GPU reads of a streamed cbuf
    bool written{};
};

class BufferCacheRuntime {
    friend Buffer;
    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    BufferCacheRuntime(const Device& device, Scheduler& scheduler, StagingBufferPool& staging,
                       TransferBufferPool& transfer_buffers,
                       Tegra::MaxwellDeviceMemoryManager& device_memory);
    ~BufferCacheRuntime();

    /// Where guest uniform, storage and texel buffer views go (set by the rasterizer).
    void SetDescriptorQueue(GuestDescriptorQueue* queue) noexcept { descriptor_queue = queue; }
    /// The next constant buffer is a root CBV the shader reads past size (see
    /// GuestDescriptorQueue::ConstantBufferReach): streaming it pads the rest with zeros.
    [[nodiscard]] bool UniformNeedsPadding(u32 size) const;
    /// Collects the buffers of the next bindings (null stops); diagnostics only.
    void SetTraceBuffers(std::vector<TracedBuffer>* out) noexcept {
        trace_buffers = out;
        traced_uniforms = 0;
    }

    void TickFrame(Common::SlotVector<Buffer>& buffers) noexcept;

    /// GPU progress, used by the generic cache to avoid overwriting buffers still in flight.
    u64 CurrentTick();
    bool IsFree(u64 tick);
    void Wait(u64 tick);
    void Finish();
    u64 GetDeviceLocalMemory() const;
    u64 GetDeviceMemoryUsage() const;
    bool CanReportMemoryUsage() const { return true; }
    u32 GetUniformBufferAlignment() const { return D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT; }
    u32 GetStorageBufferAlignment() const { return 16; }

    StagingBufferRef UploadStagingBuffer(size_t size);
    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);
    void ReadCheckpointBuffer(Buffer& buffer, u64 offset, std::span<u8> output);
    void FreeDeferredStagingBuffer(StagingBufferRef& ref);
    bool CanReorderUpload(const Buffer&, std::span<const VideoCommon::BufferCopy>) { return false; }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}

    void CopyBuffer(Buffer& dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool barrier, bool can_reorder = false);
    void CopyBuffer(Buffer& dst, ID3D12Resource* src,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder = false);
    void CopyBuffer(ID3D12Resource* dst, Buffer& src,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder = false);
    void ClearBuffer(Buffer& dst, u32 offset, size_t size, u32 value);

    /// Index and vertex buffers are collected here and set by ApplyGeometry() right before the
    /// draw, so a flush between binding and drawing (descriptor ring or staging exhaustion)
    /// cannot leave the new command list without them.
    /// The restart index of the next indexed draws, empty when restart is off.
    void SetIndexRestart(std::optional<u32> value) noexcept { index_restart = value; }
    /// Vertices the largest per-vertex stream of the next indexed draw holds (0: unknown) and
    /// its base vertex: indices past them fetch zeros (out-of-range check and fix).
    void SetVertexLimit(u32 limit, s64 base) noexcept {
        vertex_limit = limit;
        vertex_base = base;
    }
    /// The draw's inline indices, which live in no guest memory; empty for any other draw.
    void SetInlineIndices(std::span<const u8> indices) noexcept {
        inline_indices = indices;
    }

    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat format, u32 first,
                         u32 num_indices, Buffer& buffer, u32 offset, u32 size);
    /// Non-indexed quads are rewritten by EmulateTopology, like fans and loops.
    void BindQuadIndexBuffer(PrimitiveTopology, u32, u32) {}
    void BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffer(u32 index, ID3D12Resource* buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);
    /// A vertex buffer binding the draw keeps: storage writes to it since it was bound (a
    /// dispatch or draw in the same command list) need a barrier before vertex fetch reads it.
    void KeepVertexBufferReadable(Buffer& buffer);
    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings);
    /// Transform feedback buffers of the last draw that enabled it, still in their previous
    /// state: stream output (StreamOutput) or the vertex shader's storage writes
    /// (Shader::TransformFeedbackStorage) move them to theirs. Size 0: unbound.
    struct TransformFeedbackBinding {
        Buffer* buffer{};
        D3D12_GPU_VIRTUAL_ADDRESS address{};
        u32 size{};
    };
    [[nodiscard]] const std::array<TransformFeedbackBinding, 4>& TransformFeedbackBindings()
        const noexcept {
        return transform_feedback;
    }
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size);
    /// Streams the guest bytes of a uniform buffer slot, or rebinds the copy streamed for the slot
    /// earlier in this submission when they are the same. False: the caller streams them.
    bool BindGuestUniformBuffer(size_t stage, u32 index, DAddr device_addr, u32 size);
    bool UseWriteCombinedUniformCopy(u32 size) const noexcept {
        return uniform_upload_copy && !trace_buffers && size <= 512;
    }
    void BindUniformBuffer(Buffer& buffer, u32 offset, u32 size);
    void BindStorageBuffer(Buffer& buffer, u32 offset, u32 size, bool is_written);
    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format);
    void BindImageBuffer(Buffer& buffer, u32 offset, u32 size,
                         VideoCore::Surface::PixelFormat format);
    bool ShouldLimitDynamicStorageBuffers() const { return false; }
    u32 GetMaxDynamicStorageBuffers() const { return UINT32_MAX; }

    /// Topologies D3D12 lacks (quads, quad strips, fans, polygons, line loops), drawn without an
    /// index buffer: builds the equivalent list of vertex numbers first..first+count.
    void EmulateTopology(PrimitiveTopology topology, u32 first, u32 count);

    /// Index count of the rewritten index buffer when the draw was rewritten (emulated topology or
    /// 8-bit indices): the draw then reads it from index 0, with the guest's base vertex.
    [[nodiscard]] std::optional<u32> RewrittenIndexCount() const noexcept {
        return rewritten_count;
    }

    /// Sets the collected index and vertex buffers on the command list and forgets them. An index
    /// buffer the list already has is not set again.
    void ApplyGeometry(CommandStream* cmd);

    /// A new command list starts without an index buffer or vertex buffers.
    void InvalidateGeometry() noexcept;

    /// Vertex slots set on the command list being recorded (ApplyGeometry), for diagnostics.
    [[nodiscard]] u32 AppliedVertexMask() const noexcept { return applied_vertex_mask; }
    /// Where the guest-uniform path last put a stage's cbuf on the host, for diagnostics.
    struct StreamedHost {
        ID3D12Resource* resource{};
        u64 offset{};
        DAddr device_addr{};
        u64 tick{};
        u32 size{};
    };
    [[nodiscard]] const StreamedHost& LastStreamedHost(size_t stage, u32 index) const noexcept {
        return streamed_hosts[stage][index];
    }
    /// The view ApplyGeometry last set for a vertex slot (zero when none), for diagnostics.
    [[nodiscard]] const D3D12_VERTEX_BUFFER_VIEW& LastVertexView(u32 slot) const noexcept {
        return last_vertex[slot];
    }
    /// The slots of `enabled` the draw about to be recorded would run without: neither set on this
    /// list nor pending. Counted for LostVertexSlotDraws and logged; GPU thread only.
    u32 CheckVertexSlots(u32 enabled);

    /// The index buffer ApplyGeometry set on the command list, if any.
    [[nodiscard]] const std::optional<D3D12_INDEX_BUFFER_VIEW>& AppliedIndex() const noexcept {
        return applied_index;
    }

    [[nodiscard]] static bool IsEmulatedTopology(PrimitiveTopology topology) noexcept;

private:
    /// Uploads indices to staging memory and binds them as a 32-bit or 16-bit index buffer.
    void BindRewrittenIndices(IndexRewriteCache::Entry& entry);
    [[nodiscard]] bool NeedsRestartRewrite(IndexFormat format) const noexcept;
    /// The out-of-range limit (exclusive, before the base vertex) the draw's indices must be
    /// rewritten with, 0 when they are all in range or the check is off. Reports bad ranges.
    [[nodiscard]] u32 CheckIndexRange(PrimitiveTopology topology, Buffer& buffer, u64 byte_offset,
                                      u32 num_indices, u32 index_size);
    /// Largest index of count indices of index_size bytes; the restart value counts as 0.
    [[nodiscard]] static u32 MaxIndex(const u8* data, u32 count, u32 index_size,
                                      std::optional<u32> restart) noexcept;
    /// Logs an out-of-range index range with its bad index count.
    void ReportIndexRange(PrimitiveTopology topology, Buffer& buffer, u64 byte_offset,
                          u32 num_indices, u32 index_size, u32 max_index, u32 limit);
    ComPtr<ID3D12Resource> CreateDefaultBuffer(u64 size);
    /// Keeps the host buffer of a deleted cache buffer for the next one of its size
    /// (VideoCore::FastPaths): some games make the cache delete and create a few buffers every
    /// frame, and each pair is a kernel call to commit memory and another to free it. A buffer
    /// is handed out again only after the command list that last used it was submitted, when
    /// it is back in the COMMON state a new one starts in.
    void RecycleBuffer(ComPtr<ID3D12Resource> resource, u64 size);
    void Copy(Buffer* dst, ID3D12Resource* dst_raw, Buffer* src, ID3D12Resource* src_raw,
              std::span<const VideoCommon::BufferCopy> copies);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    TransferBufferPool& transfer_buffers; ///< scratch for copies within one buffer
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    GuestDescriptorQueue* descriptor_queue{};
    std::vector<TracedBuffer>* trace_buffers{};
    u32 traced_uniforms{};

    std::optional<D3D12_INDEX_BUFFER_VIEW> pending_index;
    std::optional<D3D12_INDEX_BUFFER_VIEW> applied_index; ///< what the command list holds
    std::array<D3D12_VERTEX_BUFFER_VIEW, VideoCommon::NUM_VERTEX_BUFFERS> pending_vertex{};
    UniformStream uniform_stream;
    bool uniform_upload_copy{};
    /// What each uniform slot streamed last: its guest bytes (kept in cached memory, as the stream
    /// is write-combined) and where they went. Valid for the tick they were streamed in.
    struct StreamedUniform {
        DAddr device_addr{};
        u64 tick{};
        D3D12_GPU_VIRTUAL_ADDRESS address{};
        ID3D12Resource* resource{};
        u64 offset{};
        u32 size{};
        u32 reach{};
        std::vector<u8> bytes;
    };
    std::array<std::array<StreamedUniform, VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS>,
               VideoCommon::NUM_STAGES>
        streamed_uniforms;
    D3D12_GPU_VIRTUAL_ADDRESS last_uniform_address{};
    ID3D12Resource* last_uniform_resource{};
    u64 last_uniform_offset{};
    std::array<std::array<StreamedHost, VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS>,
               VideoCommon::NUM_STAGES>
        streamed_hosts{};
    bool uniform_reuse{};
    u32 pending_vertex_mask{};
    u32 applied_vertex_mask{}; ///< slots the command list being recorded holds
    u32 known_vertex_mask{};   ///< slots last_vertex holds a view for
    std::array<D3D12_VERTEX_BUFFER_VIEW, VideoCommon::NUM_VERTEX_BUFFERS> last_vertex{};
    std::optional<u32> rewritten_count;
    std::optional<u32> index_restart;
    std::array<TransformFeedbackBinding, 4> transform_feedback{};
    std::vector<u32> rewrite_scratch;
    IndexRewriteCache rewrite_cache;
    u64 next_buffer_id{1};
    struct RecycledBuffer {
        ComPtr<ID3D12Resource> resource;
        u64 size{};
        u64 tick{};  ///< scheduler tick it was last used in
        u64 frame{}; ///< recycle_frame it was handed back at
    };
    std::vector<RecycledBuffer> recycled;
    u64 recycled_bytes{};
    u64 recycle_frame{};
    u64 recycle_hits{};
    u64 recycle_misses{};
    std::vector<u8> guest_indices;
    std::span<const u8> inline_indices;
    u32 vertex_limit{};
    s64 vertex_base{};
    /// Largest index per guest index range, valid for the buffer generation it was scanned at.
    /// Direct mapped: one probe and no allocation per draw; a collision only costs a rescan.
    struct IndexRange {
        u64 buffer_id{};
        u64 offset{};
        u64 generation{};
        u32 count{};
        u32 restart{};
        u32 max_index{};
        u32 reported_limit{};
        u8 index_size{};
        bool has_restart{};
    };
    static constexpr size_t INDEX_RANGE_SLOTS = 4096;
    std::vector<IndexRange> index_ranges = std::vector<IndexRange>(INDEX_RANGE_SLOTS);
    bool logged_rewrite{};
    bool logged_inline_rewrite{};
};

struct BufferCacheParams {
    using Runtime = BufferCacheRuntime;
    using Buffer = D3D12::Buffer;
    using Async_Buffer = StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = true;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
    /// ~Buffer defers the resource to the tick being recorded (Scheduler::DeferRelease).
    static constexpr bool BUFFER_RELEASE_WAITS_FOR_GPU = true;
    static constexpr u64 IMMEDIATE_RELEASE_SIZE = 1ULL << 20;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

} // namespace D3D12
