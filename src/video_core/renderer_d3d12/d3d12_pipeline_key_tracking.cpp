// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

#include "video_core/dirty_flags.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_tracking.h"

namespace D3D12::PipelineKeyTracking {
namespace {

using Maxwell = Maxwell3D::Regs;

#define FIELD(name)                                                                                \
    RegisterRange {                                                                                \
        static_cast<u32>(MAXWELL3D_REG_INDEX(name)),                                               \
            static_cast<u32>(sizeof(std::declval<const Maxwell&>().name) / sizeof(u32))            \
    }

constexpr u32 STREAM_WORDS = sizeof(Maxwell::VertexStream) / sizeof(u32);

// Only the address words of each vertex stream: stride, enable and frequency are in the key.
template <size_t... I>
constexpr auto StreamAddresses(std::index_sequence<I...>) {
    return std::array{RegisterRange{static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_streams) +
                                                     I * STREAM_WORDS + 1),
                                    2}...};
}

const auto IGNORED = [] {
    const std::array fields{
        // Draw parameters: the topology is compared from the draw state.
        FIELD(vertex_buffer),
        FIELD(index_buffer.start_addr_high),
        FIELD(index_buffer.start_addr_low),
        FIELD(index_buffer.limit_addr_high),
        FIELD(index_buffer.limit_addr_low),
        FIELD(index_buffer.first),
        FIELD(index_buffer.count),
        FIELD(index_buffer32_first),
        FIELD(index_buffer16_first),
        FIELD(index_buffer8_first),
        FIELD(index_buffer32_subsequent),
        FIELD(index_buffer16_subsequent),
        FIELD(index_buffer8_subsequent),
        FIELD(draw),
        FIELD(draw_inline_index),
        FIELD(inline_index_2x16),
        FIELD(inline_index_4x8),
        FIELD(vertex_id_base),
        FIELD(global_base_vertex_index),
        FIELD(global_base_instance_index),
        FIELD(vertex_array_instance_first),
        FIELD(vertex_array_instance_subsequent),
        FIELD(draw_auto_byte_count),
        FIELD(draw_auto_stride),
        FIELD(draw_auto_start_byte_count),
        // Bindings the caches resolve on every draw.
        FIELD(vertex_stream_limits),
        FIELD(const_buffer),
        FIELD(bind_groups),
        FIELD(tex_header),
        FIELD(tex_sampler),
        // Inline uploads, clears, synchronization and macro state.
        FIELD(notify),
        FIELD(wait_for_idle),
        FIELD(load_mme),
        FIELD(upload),
        FIELD(launch_dma),
        FIELD(inline_data),
        FIELD(sync_info),
        FIELD(report_semaphore),
        FIELD(clear_color),
        FIELD(clear_depth),
        FIELD(clear_stencil),
        FIELD(clear_surface),
        FIELD(raster_bounding_box),
        FIELD(invalidate_shader_cache),
        FIELD(invalidate_shader_cache_no_wfi),
        FIELD(invalidate_sampler_cache),
        FIELD(invalidate_sampler_cache_no_wfi),
        FIELD(invalidate_texture_header_cache),
        FIELD(invalidate_texture_header_cache_no_wfi),
        FIELD(invalidate_texture_data_cache),
        FIELD(invalidate_sampler_cache_lines),
        FIELD(invalidate_texture_header_cache_lines),
        FIELD(invalidate_texture_data_cache_lines),
        FIELD(shadow_scratch),
    };
    const auto streams = StreamAddresses(std::make_index_sequence<Maxwell::NumVertexArrays>{});
    std::array<RegisterRange, fields.size() + streams.size()> all{};
    std::ranges::copy(fields, all.begin());
    std::ranges::copy(streams, all.begin() + fields.size());
    return all;
}();

#undef FIELD

std::atomic<Mode> mode{Mode::Off};

} // Anonymous namespace

std::span<const RegisterRange> IgnoredRegisters() {
    return IGNORED;
}

void SetupTable(Maxwell3D::DirtyState::Table& table) {
    table.fill(KeyRegisters);
    for (const RegisterRange& range : IGNORED) {
        std::fill_n(table.begin() + range.first, range.count, VideoCommon::Dirty::NullEntry);
    }
}

void SetMode(Mode new_mode) {
    mode.store(new_mode, std::memory_order_relaxed);
}

Mode GetMode() {
    return mode.load(std::memory_order_relaxed);
}

} // namespace D3D12::PipelineKeyTracking
