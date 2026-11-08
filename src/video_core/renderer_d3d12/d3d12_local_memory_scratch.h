// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <span>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace Shader {
struct Info;
}

namespace D3D12 {

class ShaderCompiler;

/// Where the shaders of one host stage keep large guest local memory (Shader::LocalMemoryScratch).
enum class LocalMemoryScratchMode : u8 {
    Auto,   ///< Pool only when the boot probe finds the stage's private arrays broken
    Uav,    ///< Always the pool
    Native, ///< Always private arrays
};

/// Boot settings (ps_scratch= and vs_scratch=auto|uav|native), read when the pipeline cache is
/// created.
void SetPixelShaderScratchMode(LocalMemoryScratchMode mode);
void SetVertexShaderScratchMode(LocalMemoryScratchMode mode);

/// Boot setting (scratch_private=N): indexed local memory of up to N words stays in a private
/// array when the pool is on. Default 64 (LocalMemoryScratch::DEFAULT_PRIVATE_WORDS).
void SetLocalMemoryPrivateWords(u32 words);
[[nodiscard]] u32 LocalMemoryPrivateWords();

/// Logs once per shader hash its local memory size, whether it is indexed and where the backend
/// placed it, so a broken pass can be matched to its shader.
void LogLocalMemory(const char* stage, u64 hash, const Shader::Info& info);

struct LocalMemoryScratchSelection {
    u32 lanes{};  ///< Profile::local_memory_scratch_lanes, 0 without the pool
    u32 stages{}; ///< Profile::local_memory_scratch_stages
};

/// Stages that keep indexed local memory in the pool. In Auto mode each host stage draws one
/// pixel with a large private array in that stage, and gets the pool if the value is lost.
[[nodiscard]] LocalMemoryScratchSelection SelectLocalMemoryScratch(const Device& device,
                                                                   const ShaderCompiler& compiler,
                                                                   bool wave_ops, u32 wave_lanes);

/// The storage buffer bound to the shaders that keep local memory in the pool. One pool
/// serves every pipeline: slots are taken per wave, so its size depends only on the largest local
/// memory drawn and on the waves the GPU runs at once.
///
/// The slot count starts small and doubles while the pool's counters (read back a few times per
/// second, never waited on) show waves waiting for a free slot or sharing the overflow slot, up to
/// a memory cap. Shaders read the count from the pool, so growing never recompiles anything, and
/// draws pay nothing for it: one copy of 16 bytes per sample. GPU thread only.
class LocalMemoryPool {
public:
    explicit LocalMemoryPool(const Device& device, Scheduler& scheduler);
    ~LocalMemoryPool();

    /// Address of a pool that holds `words` words per lane, created or grown (zeroed) as needed.
    /// A replaced pool stays alive until the GPU finishes the draws that use it: its waves keep
    /// their slots there while new draws allocate from the new one.
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Address(u32 lanes, u32 words) {
        if (words <= rows && lanes == pool_lanes) [[likely]] {
            if (scheduler.CurrentTick() == list_tick) [[likely]] {
                return address;
            }
            return BeginList();
        }
        return Grow(lanes, words);
    }

    /// Slots of the current pool, without the overflow slot; 0 before the first pool.
    [[nodiscard]] u32 Slots() const noexcept {
        return slots;
    }

private:
    struct Counters {
        u32 acquires{};
        u32 overflows{};
        u32 waits{};
    };
    struct Sample {
        u64 tick{};
        u64 generation{};
        bool pending{};
    };
    static constexpr u32 SAMPLES = 4;

    /// First draw of a command list that uses the pool: the pool is in the common state here.
    D3D12_GPU_VIRTUAL_ADDRESS BeginList();
    D3D12_GPU_VIRTUAL_ADDRESS Grow(u32 lanes, u32 words);
    /// Replaces the pool with a zeroed one and records the write of its header.
    void Recreate(u32 lanes, u32 new_rows, u32 new_slots);
    /// Reads the samples the GPU finished; returns whether the pool was grown.
    bool ReadSamples();
    void RecordSample();

    const Device& device;
    Scheduler& scheduler;
    ComPtr<ID3D12Resource> buffer;
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    u32 rows{};
    u32 pool_lanes{};
    u32 slots{};
    /// Bumped by every Recreate: samples of a replaced pool are dropped.
    u64 generation{};
    u64 list_tick{};
    bool capped{};

    ComPtr<ID3D12Resource> readback;
    std::span<u8> readback_data;
    std::array<Sample, SAMPLES> samples{};
    u32 next_sample{};
    Counters last{};
    std::chrono::steady_clock::time_point next_sample_time{};
};

} // namespace D3D12
