// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include "video_core/renderer_d3d12/d3d12_heap_packing.h"

namespace D3D12::HeapGrowth {
struct Budget {
    std::uint64_t used{}, limit{};
    [[nodiscard]] std::uint64_t Free() const {
        return used < limit ? limit - used : 0;
    }
};

// Match the existing cache's Pressure entry point, but include the proposed heap growth.
// App and GPU accounting overlap on UMA: compare independently, never add their free space.
[[nodiscard]] inline bool UnderPressure(std::uint64_t bytes, Budget app, Budget gpu) {
    const auto tight = [bytes](Budget budget, std::uint64_t margin) {
        const auto free = budget.Free();
        return budget.limit && (bytes >= free || free - bytes <= margin);
    };
    return tight(app, 768ULL << 20) || tight(gpu, gpu.limit / 5);
}

// Under pressure allocate only the resource's required, heap-aligned capacity. Keep normal
// pooling otherwise. Resource placement alignment can be 4 KiB; heap alignment cannot.
[[nodiscard]] inline std::uint64_t Capacity(std::uint64_t size, std::uint64_t alignment,
                                          bool pressure, std::uint64_t pool = 64ULL << 20) {
    return HeapPacking::BlockSize(size, alignment, pressure ? 0 : pool);
}

// Below this much free app memory after growing, padding stops and growth may wait on the GPU.
inline constexpr std::uint64_t exhausted_reserve = 256ULL << 20;

[[nodiscard]] inline bool Exhausted(std::uint64_t bytes, Budget app) {
    const auto free = app.Free();
    return app.limit && (bytes >= free || free - bytes <= exhausted_reserve);
}

// Exact-size heaps under pressure made every following texture miss and grow its own heap.
// Pad to a small block so the next few fit, unless that padding would exhaust the app budget.
[[nodiscard]] inline std::uint64_t PressureCapacity(std::uint64_t size, std::uint64_t alignment,
                                                  Budget app, std::uint64_t pad = 16ULL << 20) {
    const auto padded = HeapPacking::BlockSize(size, alignment, pad);
    return Exhausted(padded, app) ? Capacity(size, alignment, true) : padded;
}

// Hundreds of tiny render targets live for the whole session. Sharing heaps with the large
// ones, recreated at every resolution change, they kept every RT heap from emptying.
inline constexpr std::uint64_t small_render_target = 1ULL << 20;
} // namespace D3D12::HeapGrowth
