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

/// Advanced before every fence and syncpoint the GPU thread signals. A CPU read that skips the
/// flush check because an earlier read checked the same area must not skip it once the guest can
/// have learned of GPU work done after that check.
inline std::atomic<u64> download_area_epoch{0};

inline u64 DownloadAreaEpoch() {
    return download_area_epoch.load(std::memory_order_acquire);
}

inline void AdvanceDownloadAreaEpoch() {
    download_area_epoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace VideoCore
