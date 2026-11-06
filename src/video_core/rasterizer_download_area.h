// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCore {

struct RasterizerDownloadArea {
    VAddr start_address;
    VAddr end_address;
    bool preemtive;
    /// DownloadAreaEpoch() when the area was checked; a later epoch makes it stale.
    u64 epoch{~u64{}};
};

/// A game's "exact GPU reads": a CPU read waits for a GPU write not yet in guest memory, and a read
/// checks again after every fence. Off, the guest may read a preflushable range before its download
/// lands, but core 0 does not stop for the GPU's whole queue on each rewritten page.
inline std::atomic<bool> exact_gpu_reads{false};

inline bool ExactGpuReads() {
    return exact_gpu_reads.load(std::memory_order_relaxed);
}

inline void SetExactGpuReads(bool enabled) {
    exact_gpu_reads.store(enabled, std::memory_order_relaxed);
}

/// Advanced before every fence and syncpoint the GPU thread signals while exact GPU reads are on. A
/// CPU read that skips the flush check because an earlier read checked the same area must not skip
/// it once the guest can have learned of GPU work done after that check.
inline std::atomic<u64> download_area_epoch{0};

inline u64 DownloadAreaEpoch() {
    return download_area_epoch.load(std::memory_order_acquire);
}

inline void AdvanceDownloadAreaEpoch() {
    download_area_epoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace VideoCore
