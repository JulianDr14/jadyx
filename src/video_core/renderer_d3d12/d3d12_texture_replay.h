// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "video_core/texture_cache/texture_cache_base.h"

namespace D3D12 {

/// The inputs of a pipeline's last fully bound draw that texture_replay compares (see
/// SetTextureReplay): a draw resolving the same views and samplers under the same binding stamp
/// and depth target binds the same texture descriptors.
struct TextureReplayKey {
    [[nodiscard]] bool Matches(const VideoCommon::GraphicsBindingStamp& stamp_,
                               VideoCommon::ImageId depth_image_, bool writable_depth_,
                               std::span<const VideoCommon::ImageViewInOut> views,
                               std::span<const VideoCommon::SamplerId> samplers) const {
        if (!valid || stamp != stamp_ || depth_image != depth_image_ ||
            writable_depth != writable_depth_ || views.size() != view_ids.size() ||
            samplers.size() != sampler_ids.size()) {
            return false;
        }
        for (size_t i = 0; i < views.size(); ++i) {
            if (views[i].id != view_ids[i]) {
                return false;
            }
        }
        return std::equal(samplers.begin(), samplers.end(), sampler_ids.begin());
    }

    void Store(const VideoCommon::GraphicsBindingStamp& stamp_, VideoCommon::ImageId depth_image_,
               bool writable_depth_, std::span<const VideoCommon::ImageViewInOut> views,
               std::span<const VideoCommon::SamplerId> samplers) {
        stamp = stamp_;
        depth_image = depth_image_;
        writable_depth = writable_depth_;
        view_ids.resize(views.size());
        for (size_t i = 0; i < views.size(); ++i) {
            view_ids[i] = views[i].id;
        }
        sampler_ids.assign(samplers.begin(), samplers.end());
        valid = true;
    }

    VideoCommon::GraphicsBindingStamp stamp;
    VideoCommon::ImageId depth_image;
    bool writable_depth{};
    bool valid{};
    std::vector<VideoCommon::ImageViewId> view_ids;
    std::vector<VideoCommon::SamplerId> sampler_ids;
};

} // namespace D3D12
