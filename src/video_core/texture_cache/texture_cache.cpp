// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "video_core/control/channel_state_cache.inc"
#include "video_core/texture_cache/texture_cache_base.h"

namespace VideoCommon {

namespace {
std::atomic<bool> mip_render_targets{true};
} // namespace

void SetMipRenderTargets(bool enabled) noexcept {
    mip_render_targets.store(enabled, std::memory_order_relaxed);
}

bool MipRenderTargets() noexcept {
    return mip_render_targets.load(std::memory_order_relaxed);
}

TextureCacheChannelInfo::TextureCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept
    : ChannelInfo(state)
{}

template class VideoCommon::ChannelSetupCaches<VideoCommon::TextureCacheChannelInfo>;

} // namespace VideoCommon
