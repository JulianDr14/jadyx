// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;
class ShaderCompiler;

/// Where fragment shaders keep large guest local memory (Shader::LocalMemoryScratch).
enum class PixelShaderScratchMode : u8 {
    Auto,   ///< Pool only when the boot probe finds pixel shader private arrays broken
    Uav,    ///< Always the pool
    Native, ///< Always private arrays
};

/// Boot setting (ps_scratch=auto|uav|native), read when the pipeline cache is created.
void SetPixelShaderScratchMode(PixelShaderScratchMode mode);

/// Lanes for Profile::fragment_local_memory_scratch_lanes: 0 keeps private arrays. In Auto mode
/// it draws one pixel with a large private array and enables the pool if the value is lost.
[[nodiscard]] u32 SelectFragmentScratchLanes(const Device& device, const ShaderCompiler& compiler,
                                             bool wave_ops, u32 wave_lanes);

/// The storage buffer bound to the fragment shaders that keep local memory in the pool. One pool
/// serves every pipeline: slots are taken per wave, so its size depends only on the largest local
/// memory drawn. GPU thread only.
class LocalMemoryPool {
public:
    explicit LocalMemoryPool(const Device& device, Scheduler& scheduler);
    ~LocalMemoryPool();

    /// Address of a pool that holds `words` words per lane, created or grown (zeroed) as needed.
    /// A replaced pool stays alive until the GPU finishes the draws that use it: its waves keep
    /// their slots there while new draws allocate from the new one.
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Address(u32 lanes, u32 words) {
        if (words <= rows && lanes == pool_lanes) [[likely]] {
            return address;
        }
        return Grow(lanes, words);
    }

private:
    D3D12_GPU_VIRTUAL_ADDRESS Grow(u32 lanes, u32 words);

    const Device& device;
    Scheduler& scheduler;
    ComPtr<ID3D12Resource> buffer;
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    u32 rows{};
    u32 pool_lanes{};
};

} // namespace D3D12
