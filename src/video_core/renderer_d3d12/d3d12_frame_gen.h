// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <optional>

#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_frame_gen_pacer.h"

namespace D3D12 {

class CommandStream;
class DescriptorRing;
class SamplerHeap;
class Scheduler;
class ShaderCompiler;

/// Frame generation: frames shown between the game's frames, so a game locked to 15-20 fps is
/// shown at 30 (and one at 30 at 60) with even pacing. It works on the presented frame, after
/// the present blit and FSR: generated frames are output sized (up to 4K) and need no upscale.
///
/// Ingest (GPU thread, on the scheduler's list): each presented frame is copied into a history
/// slot, reduced to a half-resolution luma pyramid, and the motion from the previous frame is
/// estimated level by level (d3d12_fg_luma.comp, d3d12_fg_motion.comp). The present frame goes
/// back to its pool at once: the GPU thread never waits on the display.
///
/// Output (present thread, on the present list): at every display slot FrameGenPacer picks a
/// content time; a real frame is copied to the back buffer, an in-between one composed from the
/// pair around it (d3d12_fg_compose.comp).
///
/// Synchronization: a slot is written by the scheduler's queue and read by the present list. The
/// present list waits for the slot's ingest tick (scheduler fence) as frame copies do; an ingest
/// takes a slot no output holds (pinned) and waits on the CPU for the present fence of its last
/// read. The slot's resources stay in read states between the two.
class FrameGen {
public:
    /// History frames: the pair shown, the newest and one being ingested. Full output size (4K
    /// with FSR), so no more than that.
    static constexpr u32 NUM_SLOTS = 4;
    static constexpr u32 NUM_LEVELS = 4;

    FrameGen(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
             DescriptorRing& descriptor_ring, SamplerHeap& sampler_heap, ID3D12Fence* present_fence,
             u32 width, u32 height);
    ~FrameGen();

    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;

    /// GPU thread: a new guest frame composed on guest vsync vsync_index, queued by the game at
    /// host time queue_time (seconds, 0 when unknown). Returns the output rate it goes out at (30
    /// or 60), or 0 when it is presented as it is.
    u32 OnFrame(u64 vsync_index, bool allowed, double arrival, double queue_time = 0.0);

    /// GPU thread: records the ingest of image (in COPY_SOURCE) on the scheduler's list. Returns
    /// the slot to Commit once the list has been flushed, or nothing when the frame is skipped.
    [[nodiscard]] std::optional<u32> RecordIngest(ID3D12Resource* image);
    /// GPU thread: the slot's commands were submitted as tick; the present thread may show it at
    /// the content time of the last OnFrame.
    void Commit(u32 slot, u64 tick, u64 vsync_index);
    /// GPU thread: a frame went out without frame generation; motion does not span it.
    void Break();

    struct Output {
        FrameGenPacer::Choice::Kind kind{FrameGenPacer::Choice::Kind::None};
        u32 a{}; ///< slots
        u32 b{};
        float phase{};
        u64 tick{}; ///< scheduler tick the slots' contents were submitted in
        u32 sync_interval{1};
        bool idle{};
    };

    /// Present thread: whether there is anything to show (call before waiting for the swapchain).
    [[nodiscard]] bool HasFrames();
    /// Present thread: the output rate frame generation runs at now (0: not running).
    [[nodiscard]] u32 Target();
    /// Present thread: what to show at host time now; pins its slots until Unpin.
    [[nodiscard]] Output Next(double now);
    /// Present thread: records output into back_buffer (in COPY_DEST afterwards it is back in
    /// PRESENT) on list.
    void RecordOutput(ID3D12GraphicsCommandList* list, const Output& output,
                      ID3D12Resource* back_buffer);
    /// Present thread: the list that read output's slots signals present fence value.
    void Unpin(const Output& output, u64 fence_value);

private:
    struct Texture {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state{};
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        D3D12_CPU_DESCRIPTOR_HANDLE uav{};
        u32 width{};
        u32 height{};
    };

    struct Slot {
        Texture color;
        std::array<Texture, NUM_LEVELS> luma;
        Texture motion; ///< level 0 field from the previous serial to this one
        double content{};
        u64 serial{};
        u64 vsync{};
        u64 tick{};
        u64 last_read{}; ///< present fence value of the last output that read the slot
        u32 pins{};
        bool valid{};
        bool has_motion{};
    };

    bool EnsureReady();
    void CreateResources();
    void CreatePipelines();
    Texture CreateTexture(u32 width, u32 height, DXGI_FORMAT format, bool uav,
                          D3D12_RESOURCE_STATES state, const wchar_t* name);
    void Transition(CommandStream* cmd, Texture& texture, D3D12_RESOURCE_STATES next);
    void Dispatch(CommandStream* cmd, ID3D12PipelineState* pipeline,
                  const std::array<u32, 8>& constants,
                  const std::array<const Texture*, 5>& sources, Texture& dest, u32 groups_x,
                  u32 groups_y);
    void WaitForPresentFence(u64 value);

    const Device& device;
    Scheduler& scheduler;
    const ShaderCompiler& compiler;
    DescriptorRing& descriptor_ring;
    SamplerHeap& sampler_heap;
    ID3D12Fence* present_fence;
    u32 width;
    u32 height;
    u32 luma_scale{2}; ///< output pixels per level 0 luma texel

    enum class State { NotBuilt, Ready, Failed } state{State::NotBuilt};

    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> luma_pipeline;
    ComPtr<ID3D12PipelineState> motion_pipeline;
    ComPtr<ID3D12PipelineState> compose_pipeline;

    ComPtr<ID3D12DescriptorHeap> cpu_heap;   ///< offline views, uploaded into the ring by ingest
    ComPtr<ID3D12DescriptorHeap> table_heap; ///< shader visible: one compose table per slot pair
    u32 cpu_used{};
    u32 descriptor_size{};
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv{};
    D3D12_CPU_DESCRIPTOR_HANDLE null_uav{};

    std::array<Slot, NUM_SLOTS> slots;
    std::array<Texture, NUM_LEVELS> scratch_motion; ///< levels 1-3 of the field; level 0 unused
    ComPtr<ID3D12Resource> stats;
    Texture output;

    // GPU thread.
    std::optional<u32> last_slot;
    bool continuity{};
    u64 next_serial{1};

    std::mutex mutex; ///< slots' bookkeeping and the pacer
    FrameGenPacer pacer;
    double frame_content{}; ///< GPU thread: content time of the last OnFrame
    u32 target{};
    double slot_clock{};
    bool clock_valid{};
};

} // namespace D3D12
