// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace D3D12::PipelineKeyTracking {

enum class Mode : u8 {
    Off,   ///< Refresh and compare the key on every draw, as before.
    On,    ///< Reuse the last pipeline while nothing the key reads changed.
    Check, ///< Refresh anyway and report every reuse that would have picked another pipeline.
};
void SetMode(Mode mode);
[[nodiscard]] Mode GetMode();

} // namespace D3D12::PipelineKeyTracking
