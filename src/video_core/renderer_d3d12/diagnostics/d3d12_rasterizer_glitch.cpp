// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The rasterizer side of the geometry glitch hunt (GlitchHunt): one line per draw and dispatch,
// one per binding it read, and host/guest comparisons of the bindings the cache says are clean.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/query_cache.h"
#include "video_core/renderer_d3d12/d3d12_conditional_rendering.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_key_tracking.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_stream_output.h"
#include "video_core/readback_trace.h"

namespace D3D12 {

namespace {

/// Guest bytes read per binding; larger ranges are hashed and compared up to this much.
constexpr u64 BINDING_CAP = 1ULL << 20;
/// Read-only compute storage buffers up to this size are compared with guest memory (v11).
constexpr u64 COMPUTE_COMPARE_CAP = 256ULL << 10;
/// Compute storage bindings dumped whole around a dispatch up to this size (v12).
constexpr u64 COMPUTE_DUMP_CAP = 1ULL << 20;
/// Read-only inputs above that cap, saved once per capture for a replay.
constexpr u64 COMPUTE_FULL_DUMP_CAP = 32ULL << 20;
/// v17: compute dumps only in the first frames; two of each parity are enough to diff.
constexpr u32 COMPUTE_DUMP_FRAMES = 6;
/// Indices read to find the vertex range an indexed draw touches.
constexpr u64 INDEX_CAP = 4ULL << 20;
/// Vertex bytes compared when the range is unknown (indirect draws, zero stride).
constexpr u64 UNKNOWN_RANGE = 64ULL << 10;
/// Host bytes read back from a GPU-written vertex stream or transform feedback buffer.
constexpr u64 INSPECT_CAP = 64ULL << 10;
/// v13 draw dumps: streamed uniforms up to this size, storage ranges cut at this size.
constexpr u32 DRAW_DUMP_UNIFORM_CAP = 4096;
constexpr u64 DRAW_DUMP_STORAGE_CAP = 16ULL << 10;
/// v13 spent the dump budget on draws only; v17 dumps the culling dispatch's lists again to find
/// what the guest's CPU-written instance list changes on alternate frames.
constexpr bool DRAW_DUMPS_ONLY = false;
constexpr u32 NUM_XFB_BUFFERS = Tegra::Engines::Maxwell3D::Regs::NumTransformFeedbackBuffers;

constexpr std::array OUTCOME_NAMES{"recorded", "skipped",  "conditional", "culled",   "no_pipeline",
                                   "rejected", "skip_shader", "empty",   "no_target", "expanded"};
constexpr std::array PATH_NAMES{"none", "fixed", "forced", "cpu", "snapshot", "gpu", "fallback"};

const char* OutcomeName(Hunt::Outcome outcome) {
    const auto index = static_cast<size_t>(outcome);
    return index < OUTCOME_NAMES.size() ? OUTCOME_NAMES[index] : "?";
}

struct WordStats {
    u32 nonfinite{};
    float maxabs{};
};

/// Treats the bytes as floats: how many are NaN or infinite and the largest finite magnitude.
WordStats Stats(std::span<const u8> bytes) {
    WordStats stats;
    for (size_t at = 0; at + 4 <= bytes.size(); at += 4) {
        u32 word;
        std::memcpy(&word, bytes.data() + at, 4);
        if ((word & 0x7f800000u) == 0x7f800000u) {
            ++stats.nonfinite;
            continue;
        }
        const float value = std::bit_cast<float>(word);
        stats.maxabs = std::max(stats.maxabs, std::fabs(value));
    }
    return stats;
}

u64 Hash(std::span<const u8> bytes) {
    return bytes.empty() ? 0
                         : Common::CityHash64(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size());
}

} // namespace

void RasterizerD3D12::GlitchDraw(const GlitchNote& note) {
    const u32 frame = glitch_hunt.Frame();
    const u32 call = glitch_hunt.NextCall();
    const auto& regs = maxwell3d->regs;
    const auto& draw = maxwell3d->draw_manager.draw_state;
    const ConditionalRendering::Decision& cond = conditional_rendering.LastDecision();
    const auto path = static_cast<size_t>(cond.path);
    std::string target = "fb=none";
    if (note.framebuffer) {
        const auto* const fb = static_cast<const Framebuffer*>(note.framebuffer);
        const auto extent = fb->Extent();
        target = fmt::format("fb={:x}:{}x{}:c{}:f{}:d{}:s{}",
                             reinterpret_cast<std::uintptr_t>(fb) & 0xffffff, extent.width,
                             extent.height, fb->ColorTargets().size(),
                             static_cast<int>(fb->ColorTargets().empty() ? DXGI_FORMAT_UNKNOWN
                                                                         : fb->ColorFormat(0)),
                             static_cast<int>(fb->DepthFormat()), fb->Samples());
    }
    glitch_hunt.Line(fmt::format(
        "D f={} n={} k={} o={} vs={:016x} ps={:016x} topo={} cnt={} inst={} first={} base={} "
        "binst={} cond={}:{}:{:x}:{}:{}:{} lost={:x} emu={} rw={} {} dt={} dw={} df={} cull={}:{}:{} "
        "ib={}:{}:{} vb={}:{}",
        frame, call, note.kind, OutcomeName(note.outcome), note.vs, note.ps,
        static_cast<u32>(draw.topology), note.count, note.instances, note.first, note.base_vertex,
        note.base_instance, path < PATH_NAMES.size() ? PATH_NAMES[path] : "?", cond.render ? 1 : 0,
        cond.address, cond.serial, cond.lhs, cond.rhs, note.lost_slots, note.emulated ? 1 : 0,
        note.rewritten ? 1 : 0, target, regs.depth_test_enable, regs.depth_write_enabled,
        static_cast<u32>(regs.depth_test_func), regs.gl_cull_test_enabled,
        static_cast<u32>(regs.gl_front_face), static_cast<u32>(regs.gl_cull_face),
        draw.index_buffer.count, draw.index_buffer.first, draw.index_buffer.FormatSizeInBytes(),
        draw.vertex_buffer.first, draw.vertex_buffer.count));
    if (note.outcome == Hunt::Outcome::Recorded) {
        // v6: depth/stencil state the D line leaves out, the host pipeline that ran and the
        // depth image it tested against.
        const auto& vt = regs.viewport_transform[0];
        const auto& vp = regs.viewports[0];
        std::string pipe = "pso=none";
        if (note.pipeline) {
            const auto& gp = *static_cast<const GraphicsPipeline*>(note.pipeline);
            const auto& key = gp.Key();
            pipe = fmt::format("pso={:x} key={:016x} kb={:g}/{:g}/{:g} ksm={:x}/{:x} ez={} kd={}",
                               reinterpret_cast<std::uintptr_t>(gp.Handle()) & 0xffffff,
                               gp.KeyHash(), std::bit_cast<f32>(key.depth_bias),
                               std::bit_cast<f32>(key.slope_scaled_depth_bias),
                               std::bit_cast<f32>(key.depth_bias_clamp), key.stencil_read_mask,
                               key.stencil_write_mask, static_cast<u32>(key.state.early_z),
                               static_cast<u32>(key.state.depth_enabled));
        }
        std::string depth_img = "zimg=none";
        if (note.framebuffer) {
            const auto* const fb = static_cast<const Framebuffer*>(note.framebuffer);
            if (const Image* const zi = fb->DepthImage()) {
                depth_img = fmt::format("zimg={} za={:x} zflags={:x} zmod={}",
                                        fb->DepthImageId().index, zi->gpu_addr,
                                        static_cast<u32>(zi->flags), zi->modification_tick);
            }
        }
        glitch_hunt.Line(fmt::format(
            "G f={} n={} st={}:{}:{} sf={}:{}:{}:{}:{}:{}:{} sb={}:{}:{}:{}:{}:{}:{} "
            "bias={}:{:g}:{:g}:{:g} bounds={} a2c={}:{} at={} zmode={} vz={:g}/{:g} "
            "dr={:g}/{:g} clip={:x} {} {}",
            frame, call, regs.stencil_enable, regs.stencil_two_side_enable,
            static_cast<u32>(regs.depth_mode), static_cast<u32>(regs.stencil_front_op.func),
            static_cast<u32>(regs.stencil_front_op.fail),
            static_cast<u32>(regs.stencil_front_op.zfail),
            static_cast<u32>(regs.stencil_front_op.zpass), regs.stencil_front_ref,
            regs.stencil_front_func_mask, regs.stencil_front_mask,
            static_cast<u32>(regs.stencil_back_op.func), static_cast<u32>(regs.stencil_back_op.fail),
            static_cast<u32>(regs.stencil_back_op.zfail),
            static_cast<u32>(regs.stencil_back_op.zpass), regs.stencil_back_ref,
            regs.stencil_back_func_mask, regs.stencil_back_mask,
            regs.polygon_offset_fill_enable, regs.depth_bias, regs.slope_scale_depth_bias,
            regs.depth_bias_clamp, regs.depth_bounds_enable,
            regs.anti_alias_alpha_control.alpha_to_coverage.Value(),
            regs.anti_alias_alpha_control.alpha_to_one.Value(), regs.alpha_test_enabled,
            static_cast<u32>(regs.depth_mode), vt.scale_z, vt.translate_z, vp.depth_range_near,
            vp.depth_range_far, std::bit_cast<u32>(regs.viewport_clip_control), pipe, depth_img));
    }
    if (note.configured && note.outcome == Hunt::Outcome::Recorded) {
        GlitchBindings(frame, call, &note, false);
        if (note.kind >= 2 && note.kind <= 3) {
            GlitchDrawDump(call);
        }
        GlitchTextures(frame, call);
        if (regs.transform_feedback_enabled != 0 && note.pipeline) {
            GlitchTransformFeedback(frame, call,
                                    *static_cast<const GraphicsPipeline*>(note.pipeline));
        }
    }
    glitch_views.clear();
    glitch_filters.clear();
    if (note.framebuffer && note.outcome == Hunt::Outcome::Recorded) {
        GlitchPassNote(static_cast<const Framebuffer*>(note.framebuffer), call, false);
    }
}

void RasterizerD3D12::GlitchTextures(u32 frame, u32 call) {
    // One S line per sampled view: which host image it is, where it lives in guest memory, who
    // last wrote it (flags, modification tick) and how the shader reads it (srv, sampler).
    for (size_t i = 0; i < glitch_views.size(); ++i) {
        const VideoCommon::ImageViewId id = glitch_views[i];
        const std::string_view desc =
            i < glitch_filters.size() ? std::string_view{glitch_filters[i]} : std::string_view{};
        const ImageView& view = texture_cache.GetImageView(id);
        const Image* const image = view.SourceImage();
        if (!image) {
            glitch_hunt.Line(fmt::format("S f={} n={} i={} view={} null {}", frame, call, i,
                                         id.index, desc));
            continue;
        }
        const auto& info = image->info;
        glitch_hunt.Line(fmt::format(
            "S f={} n={} i={} view={} img={} a={:x} {}x{}x{} fmt={} lv={}/{} ly={}/{} "
            "flags={:x} mod={} aliases={} {}",
            frame, call, i, id.index, view.image_id.index, image->gpu_addr, info.size.width,
            info.size.height, info.size.depth, static_cast<u32>(info.format), view.range.base.level,
            info.resources.levels, view.range.base.layer, info.resources.layers,
            static_cast<u32>(image->flags), image->modification_tick, image->aliased_images.size(),
            desc));
    }
}

void RasterizerD3D12::GlitchTransformFeedback(u32 frame, u32 call,
                                              const GraphicsPipeline& pipeline) {
    const auto& regs = maxwell3d->regs;
    const PipelineLayout& layout = pipeline.Layout();
    const bool tess = pipeline.TessPlan().Active();
    const bool native = layout.StreamOutput() && !tess;
    const bool storage = layout.TransformFeedbackConstantsIndex() != PipelineLayout::NO_TABLE;
    const char* const mode = native ? "native" : storage ? "storage" : tess ? "tess" : "none";
    const auto& bindings = buffer_runtime.TransformFeedbackBindings();
    std::string slots;
    for (u32 index = 0; index < NUM_XFB_BUFFERS; ++index) {
        const auto& buffer = regs.transform_feedback.buffers[index];
        const auto& control = regs.transform_feedback.controls[index];
        const auto& binding = bindings[index];
        const u64 host_offset =
            binding.buffer && binding.size != 0 ? binding.address - binding.buffer->Address() : 0;
        glitch_hunt.Line(fmt::format(
            "X f={} n={} i={} mode={} en={} gva={:x} size={} start={} stride={} stream={} "
            "varyings={} host={:x}+{:x}:{}",
            frame, call, index, mode, buffer.enable, buffer.Address(), buffer.size,
            buffer.start_offset, control.stride, control.stream, control.varying_count,
            binding.buffer ? binding.buffer->Id() : 0, host_offset, binding.size));
        if (buffer.enable == 0 || !binding.buffer || binding.size == 0 ||
            !binding.buffer->Handle()) {
            continue;
        }
        // What the pass left in the buffer, from its binding (guest address + start_offset).
        binding.buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ, false);
        glitch_hunt.Inspect(call, fmt::format("xfb{}", index), binding.buffer->Handle(),
                            host_offset, std::min<u64>(binding.size, INSPECT_CAP),
                            control.stride, fmt::format("gva={:x} mode={}", buffer.Address(), mode));
    }
    if (native) {
        // Filled sizes after this pass: bytes written past each binding, appended across passes.
        std::string extra = "sizes=";
        for (u32 index = 0; index < NUM_XFB_BUFFERS; ++index) {
            extra += fmt::format("{}{}", index == 0 ? "" : ",",
                                 regs.transform_feedback.buffers[index].enable != 0
                                     ? bindings[index].size
                                     : 0);
        }
        glitch_hunt.InspectWords(call, "so", StreamOutput::COUNTERS_BYTES,
                                 static_cast<u32>(StreamOutput::COUNTERS_BYTES /
                                                  NUM_XFB_BUFFERS),
                                 std::move(extra), [&](ID3D12Resource* dst, u64 offset) {
                                     stream_output.CopyCounters(scheduler.CommandList(), dst,
                                                                offset);
                                 });
    }
}

void RasterizerD3D12::GlitchDrawDump(u32 call) {
    const VideoCommon::BufferCacheChannelInfo* const channel = buffer_cache.DiagChannel();
    if (!channel || glitch_hunt.DumpRoom() == 0) {
        return;
    }
    std::vector<u8> guest;
    // Streamed uniforms (the per-frame camera cbuf among them): the upload bytes the draw's
    // descriptors point at, copied at this point of the command list, and guest memory now.
    for (size_t stage = 0; stage < VideoCommon::NUM_STAGES; ++stage) {
        const u32 mask = channel->enabled_uniform_buffer_masks[stage];
        for (u32 index = 0; index < VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS; ++index) {
            if (((mask >> index) & 1) == 0) {
                continue;
            }
            const VideoCommon::Binding& cb = channel->uniform_buffers[stage][index];
            const auto& host = buffer_runtime.LastStreamedHost(stage, index);
            if (cb.device_addr == 0 || !host.resource || host.device_addr != cb.device_addr ||
                host.tick != scheduler.CurrentTick() || host.size == 0 ||
                host.size > DRAW_DUMP_UNIFORM_CAP) {
                continue;
            }
            const std::string role = fmt::format("cb{}.{}", stage, index);
            const std::string extra =
                fmt::format("ph=draw a={:x} up={:x}", cb.device_addr, host.offset);
            guest.resize(host.size);
            device_memory.ReadBlockUnsafe(cb.device_addr, guest.data(), host.size);
            glitch_hunt.DumpGuest(call, role, guest, extra);
            // Upload heaps stay in GENERIC_READ, which copies need no barrier for.
            glitch_hunt.Dump(call, role, host.resource, host.offset, host.size,
                             D3D12_RESOURCE_STATE_GENERIC_READ, extra);
        }
    }
    // Read-only storage buffers: the range the draws' instances index from their base instance
    // (u32 per instance), from the guest argument words (equal to the host's in v8).
    if (glitch_args_call != call || glitch_args_stride == 0 || glitch_args_count < 4) {
        return;
    }
    u64 lo = ~u64{0};
    u64 hi = 0;
    const u32 base_word = glitch_args_count - 1;
    for (size_t at = 0; at + glitch_args_count <= glitch_args_words.size();
         at += glitch_args_stride) {
        const u32 instances = glitch_args_words[at + 1];
        const u32 base = glitch_args_words[at + base_word];
        if (glitch_args_words[at] == 0 || instances == 0) {
            continue;
        }
        lo = std::min<u64>(lo, base);
        hi = std::max<u64>(hi, u64{base} + instances);
    }
    if (lo >= hi) {
        return;
    }
    for (size_t stage = 0; stage < VideoCommon::NUM_STAGES; ++stage) {
        const u32 mask = channel->enabled_storage_buffers[stage];
        for (u32 index = 0; index < VideoCommon::NUM_STORAGE_BUFFERS; ++index) {
            if (((mask >> index) & 1) == 0 ||
                ((channel->written_storage_buffers[stage] >> index) & 1) != 0) {
                continue;
            }
            const VideoCommon::Binding& sb = channel->storage_buffers[stage][index];
            const u64 begin = lo * sizeof(u32);
            if (sb.device_addr == 0 || begin >= sb.size) {
                continue;
            }
            const u64 size =
                std::min<u64>({(hi - lo) * sizeof(u32), sb.size - begin, DRAW_DUMP_STORAGE_CAP}) &
                ~u64{3};
            Buffer* const buffer = buffer_cache.DiagBuffer(sb.buffer_id);
            if (size == 0 || !buffer || !buffer->Handle()) {
                continue;
            }
            const DAddr addr = sb.device_addr + begin;
            const std::string role = fmt::format("sb{}.{}", stage, index);
            const bool gpu_mod = buffer_cache.IsRegionGpuModified(addr, size);
            const std::string extra =
                fmt::format("ph=draw a={:x} at={} base={} end={} buf={:x}+{:x} gm={}",
                            sb.device_addr, begin, lo, hi, buffer->Id(), buffer->Offset(addr),
                            gpu_mod ? 1 : 0);
            if (!gpu_mod) {
                guest.resize(size);
                device_memory.ReadBlockUnsafe(addr, guest.data(), size);
                glitch_hunt.DumpGuest(call, role, guest, extra);
            }
            glitch_hunt.Dump(call, role, buffer->Handle(), buffer->Offset(addr), size,
                             buffer->CurrentState(), extra);
        }
    }
}

void RasterizerD3D12::GlitchComputeDump(u32 frame, u32 call, bool after) {
    const VideoCommon::BufferCacheChannelInfo* const channel = buffer_cache.DiagChannel();
    // v13: the culling dispatches were ruled out in v12; the budget goes to the draws.
    if (!channel || glitch_hunt.DumpRoom() == 0 || DRAW_DUMPS_ONLY ||
        frame >= COMPUTE_DUMP_FRAMES) {
        return;
    }
    std::vector<u8> guest;
    if (!after) {
        // v18: the constants too, so a dispatch can be replayed off the console.
        for (u32 index = 0; index < VideoCommon::NUM_COMPUTE_UNIFORM_BUFFERS; ++index) {
            if (((channel->enabled_compute_uniform_buffer_mask >> index) & 1) == 0) {
                continue;
            }
            const auto& ccb = channel->compute_uniform_buffers[index];
            const u32 shader_size = channel->compute_uniform_buffer_sizes
                                        ? (*channel->compute_uniform_buffer_sizes)[index]
                                        : 0;
            const u64 size = std::min<u64>(shader_size, ccb.size) & ~u64{3};
            if (ccb.device_addr == 0 || size == 0) {
                continue;
            }
            guest.resize(size);
            device_memory.ReadBlockUnsafe(ccb.device_addr, guest.data(), size);
            glitch_hunt.DumpGuest(call, fmt::format("ccb{}", index), guest,
                                  fmt::format("ph=pre a={:x} ssize={}", ccb.device_addr,
                                              shader_size));
        }
    }
    for (u32 index = 0; index < VideoCommon::NUM_STORAGE_BUFFERS; ++index) {
        if (((channel->enabled_compute_storage_buffers >> index) & 1) == 0) {
            continue;
        }
        const bool written = ((channel->written_compute_storage_buffers >> index) & 1) != 0;
        if (after && !written) {
            continue;
        }
        const auto& csb = channel->compute_storage_buffers[index];
        const u64 size = std::min<u64>(csb.size, COMPUTE_DUMP_CAP) & ~u64{3};
        Buffer* const buffer = buffer_cache.DiagBuffer(csb.buffer_id);
        if (csb.device_addr == 0 || size == 0 || !buffer || !buffer->Handle()) {
            continue;
        }
        const std::string role = fmt::format("csb{}", index);
        if (!after && !written && csb.size > COMPUTE_DUMP_CAP) {
            // v19: a replay needs all of a read-only input. Guest bytes only: nothing writes it.
            guest.resize(std::min<u64>(csb.size, COMPUTE_FULL_DUMP_CAP) & ~u64{3});
            device_memory.ReadBlockUnsafe(csb.device_addr, guest.data(), guest.size());
            if (glitch_hunt.FirstInCapture(Common::CityHash64(
                    reinterpret_cast<const char*>(guest.data()), guest.size()))) {
                glitch_hunt.DumpGuest(call, role + "_full", guest,
                                      fmt::format("ph=pre a={:x}", csb.device_addr));
            }
        }
        const std::string extra = fmt::format("ph={} a={:x} buf={:x}+{:x} w={} gm={}",
                                              after ? "post" : "pre", csb.device_addr,
                                              buffer->Id(), buffer->Offset(csb.device_addr),
                                              written ? 1 : 0,
                                              buffer_cache.IsRegionGpuModified(csb.device_addr,
                                                                               size)
                                                  ? 1
                                                  : 0);
        if (!after) {
            // What the guest holds there when the dispatch is set up.
            guest.resize(size);
            device_memory.ReadBlockUnsafe(csb.device_addr, guest.data(), size);
            glitch_hunt.DumpGuest(call, role, guest, extra);
        }
        glitch_hunt.Dump(call, role, buffer->Handle(), buffer->Offset(csb.device_addr), size,
                         buffer->CurrentState(), extra);
    }
}

void RasterizerD3D12::GlitchBindings(u32 frame, u32 call, const GlitchNote* note, bool compute) {
    const VideoCommon::BufferCacheChannelInfo* const channel = buffer_cache.DiagChannel();
    if (!channel) {
        return;
    }
    std::vector<u8> guest;
    // Reads guest bytes, describes the binding and, when the cache says host holds them
    // unchanged, queues the host copy for comparison.
    const auto binding_line = [&](std::string_view role, const VideoCommon::Binding& binding,
                                  u64 begin, u64 length, std::string_view extra, bool compare,
                                  bool floats, u32 inspect_stride = 0) {
        if (binding.device_addr == 0 || binding.size == 0) {
            glitch_hunt.Line(fmt::format("B f={} n={} r={} null size={} {}", frame, call, role,
                                         binding.size, extra));
            return;
        }
        begin = std::min<u64>(begin, binding.size);
        length = std::min<u64>({length, binding.size - begin, BINDING_CAP});
        const DAddr addr = binding.device_addr + begin;
        guest.resize(length);
        if (length != 0) {
            device_memory.ReadBlockUnsafe(addr, guest.data(), length);
        }
        const bool gpu_mod = length != 0 && buffer_cache.IsRegionGpuModified(addr, length);
        const bool cpu_mod = length != 0 && buffer_cache.IsRegionCpuModified(addr, length);
        const u64 hash = Hash(guest);
        Buffer* const buffer = buffer_cache.DiagBuffer(binding.buffer_id);
        std::string state = "nobuf";
        bool queued = false;
        if (buffer && buffer->Handle()) {
            state = fmt::format("buf={:x}+{:x}", buffer->Id(), buffer->Offset(addr));
            if (compute && compare && !gpu_mod && length != 0) {
                // The dispatch's bindings are set: copy around its state, which stays tracked.
                queued = glitch_hunt.Compare(call, role, buffer->Handle(), buffer->Offset(addr),
                                             guest, hash, buffer->CurrentState());
            } else if (compare && !gpu_mod && length != 0) {
                if (buffer->State() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    state += ":uav"; // written by this work: not comparable
                } else {
                    buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ, false);
                    queued = glitch_hunt.Compare(call, role, buffer->Handle(),
                                                 buffer->Offset(addr), guest, hash);
                }
            }
        }
        // GPU-written vertex streams: guest memory cannot check them, so read what the host has.
        if (inspect_stride != 0 && gpu_mod && length != 0 && buffer && buffer->Handle() &&
            buffer->State() != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ, false);
            if (glitch_hunt.Inspect(call, role, buffer->Handle(), buffer->Offset(addr),
                                    std::min<u64>(length, INSPECT_CAP), inspect_stride,
                                    fmt::format("gva={:x}", addr))) {
                state += ":ins";
            }
        }
        std::string values;
        if (floats && length != 0) {
            const WordStats stats = Stats(guest);
            values = fmt::format(" nan={} max={:.4g}", stats.nonfinite, stats.maxabs);
        }
        glitch_hunt.Line(fmt::format("B f={} n={} r={} a={:x} gva={:x} size={} at={} len={} {} "
                                     "gm={} cm={} h={:016x} cmp={}{} {}",
                                     frame, call, role, binding.device_addr, binding.gpu_addr,
                                     binding.size, begin, length, state, gpu_mod ? 1 : 0,
                                     cpu_mod ? 1 : 0, hash, queued ? 1 : 0, values, extra));
    };

    if (compute) {
        for (u32 index = 0; index < VideoCommon::NUM_COMPUTE_UNIFORM_BUFFERS; ++index) {
            if (((channel->enabled_compute_uniform_buffer_mask >> index) & 1) == 0) {
                continue;
            }
            const u32 shader_size = channel->compute_uniform_buffer_sizes
                                        ? (*channel->compute_uniform_buffer_sizes)[index]
                                        : 0;
            // Host copies of the inputs, compared with guest memory (v11): a stale upload shows
            // as an M line. Misaligned ones are streamed, not read from the cached buffer.
            const auto& ccb = channel->compute_uniform_buffers[index];
            Buffer* const host = buffer_cache.DiagBuffer(ccb.buffer_id);
            const bool cached = host && host->Handle() && host->Offset(ccb.device_addr) % 256 == 0;
            binding_line(fmt::format("ccb{}", index), ccb, 0, shader_size,
                         fmt::format("ssize={}", shader_size), cached, true);
        }
        for (u32 index = 0; index < VideoCommon::NUM_STORAGE_BUFFERS; ++index) {
            if (((channel->enabled_compute_storage_buffers >> index) & 1) == 0) {
                continue;
            }
            const bool written = ((channel->written_compute_storage_buffers >> index) & 1) != 0;
            const auto& csb = channel->compute_storage_buffers[index];
            binding_line(fmt::format("csb{}", index), csb, 0, BINDING_CAP,
                         written ? "w=1" : "w=0", !written && csb.size <= COMPUTE_COMPARE_CAP,
                         false);
        }
        return;
    }

    const auto& regs = maxwell3d->regs;
    const auto& draw = maxwell3d->draw_manager.draw_state;
    const bool indirect = note && note->kind >= 2 && note->kind <= 3 && note->count != 0 &&
                          note->instances == 0;
    const bool indexed = note && (note->kind & 1) != 0;

    // Index buffer and the vertex range it reaches.
    s64 vertex_lo = 0;
    s64 vertex_hi = -1; // inclusive; empty when hi < lo
    bool range_known = false;
    if (indexed) {
        const VideoCommon::Binding& ib = channel->index_buffer;
        const u32 index_size = static_cast<u32>(draw.index_buffer.FormatSizeInBytes());
        if (!draw.inline_index_draw_indexes.empty()) {
            glitch_hunt.Line(fmt::format("B f={} n={} r=ib-inline size={} isz={}", frame, call,
                                         draw.inline_index_draw_indexes.size(), index_size));
        } else if (!indirect && index_size != 0) {
            const u64 begin = u64{draw.index_buffer.first} * index_size;
            const u64 length = u64{draw.index_buffer.count} * index_size;
            std::vector<u8> indices(std::min<u64>(
                {length, INDEX_CAP, ib.size > begin ? ib.size - begin : 0}));
            if (!indices.empty() && ib.device_addr != 0) {
                device_memory.ReadBlockUnsafe(ib.device_addr + begin, indices.data(),
                                              indices.size());
                const bool restart = regs.primitive_restart.enabled != 0;
                const u32 restart_index = regs.primitive_restart.index;
                u32 lo = std::numeric_limits<u32>::max();
                u32 hi = 0;
                u32 restarts = 0;
                for (size_t at = 0; at + index_size <= indices.size(); at += index_size) {
                    u32 value = 0;
                    std::memcpy(&value, indices.data() + at, index_size);
                    if (restart && value == restart_index) {
                        ++restarts;
                        continue;
                    }
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
                if (lo <= hi) {
                    vertex_lo = s64{lo} + s64{draw.base_index};
                    vertex_hi = s64{hi} + s64{draw.base_index};
                    range_known = true;
                }
                glitch_hunt.Line(fmt::format("I f={} n={} min={} max={} restarts={} read={}",
                                             frame, call, lo, hi, restarts, indices.size()));
            }
            binding_line("ib", ib, begin, length,
                         fmt::format("isz={} restart={}", index_size,
                                     regs.primitive_restart.enabled),
                         true, false);
        } else {
            binding_line("ib", ib, 0, UNKNOWN_RANGE, fmt::format("isz={}", index_size), true,
                         false);
        }
    } else if (note && !indirect) {
        vertex_lo = draw.vertex_buffer.first;
        vertex_hi = s64{draw.vertex_buffer.first} + s64{draw.vertex_buffer.count} - 1;
        range_known = vertex_hi >= vertex_lo;
    }

    // Vertex buffers: what the guest stream holds over the range the draw reads, and the view the
    // command list was given for the slot.
    const u32 enabled = buffer_cache.DiagVertexMask();
    const u32 applied = buffer_runtime.AppliedVertexMask();
    for (u32 index = 0; index < VideoCommon::NUM_VERTEX_BUFFERS; ++index) {
        if (((enabled >> index) & 1) == 0) {
            continue;
        }
        const VideoCommon::Binding& vb = channel->vertex_buffers[index];
        const u32 stride = regs.vertex_streams[index].stride;
        const bool instanced = regs.vertex_stream_instances.IsInstancingEnabled(index);
        u64 begin = 0;
        u64 length = UNKNOWN_RANGE;
        if (stride != 0 && instanced && note && !indirect) {
            const u32 divisor = std::max<u32>(regs.vertex_streams[index].frequency, 1);
            begin = u64{note->base_instance} * stride;
            length = (u64{note->instances} / divisor + 1) * stride;
        } else if (stride != 0 && range_known && vertex_lo >= 0) {
            begin = u64(vertex_lo) * stride;
            length = u64(vertex_hi - vertex_lo + 1) * stride;
        }
        const D3D12_VERTEX_BUFFER_VIEW& view = buffer_runtime.LastVertexView(index);
        std::string host = fmt::format("view={:x}:{}:{}", view.BufferLocation, view.SizeInBytes,
                                       view.StrideInBytes);
        if (Buffer* const buffer = buffer_cache.DiagBuffer(vb.buffer_id);
            buffer && buffer->Handle() && vb.device_addr != 0) {
            // The view should start at the binding's place in its buffer.
            const u64 expected = buffer->Address() + buffer->Offset(vb.device_addr);
            if (view.BufferLocation != expected) {
                host += fmt::format(":stale(expect={:x})", expected);
            }
        }
        binding_line(fmt::format("vb{}", index), vb, begin, length,
                     fmt::format("stride={} inst={} set={} {}", stride, instanced ? 1 : 0,
                                 (applied >> index) & 1, host),
                     true, true, std::max<u32>(stride, 4));
    }

    // Uniform buffers: bound size against what the shader reads, and the path the cache took.
    for (size_t stage = 0; stage < VideoCommon::NUM_STAGES; ++stage) {
        const u32 mask = channel->enabled_uniform_buffer_masks[stage];
        for (u32 index = 0; index < VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS; ++index) {
            if (((mask >> index) & 1) == 0) {
                continue;
            }
            const VideoCommon::Binding& cb = channel->uniform_buffers[stage][index];
            const u32 shader_size =
                channel->uniform_buffer_sizes ? (*channel->uniform_buffer_sizes)[stage][index] : 0;
            const u32 size = std::min(cb.size, shader_size);
            Buffer* const buffer = buffer_cache.DiagBuffer(cb.buffer_id);
            const bool has_host = buffer != nullptr;
            const u32 offset = has_host ? buffer->Offset(cb.device_addr) : 0;
            const bool misaligned =
                has_host && (offset % buffer_runtime.GetUniformBufferAlignment()) != 0;
            const bool streamed =
                misaligned ||
                (has_host &&
                 (size <= channel->uniform_buffer_skip_cache_size ||
                  buffer_runtime.UniformNeedsPadding(size)) &&
                 (cb.device_addr == 0 || !buffer_cache.IsRegionGpuModified(cb.device_addr, size)));
            binding_line(fmt::format("cb{}.{}", stage, index), cb, 0, size,
                         fmt::format("ssize={} path={}{}", shader_size, streamed ? 's' : 'c',
                                     cb.size < shader_size ? " short=1" : ""),
                         !streamed, true);
        }
    }

    // Storage buffers: hashed; the read-only ones the GPU wrote (per-instance tables a compute
    // pass fills) are also read back from the host, to compare frames where objects vanish.
    for (size_t stage = 0; stage < VideoCommon::NUM_STAGES; ++stage) {
        const u32 mask = channel->enabled_storage_buffers[stage];
        for (u32 index = 0; index < VideoCommon::NUM_STORAGE_BUFFERS; ++index) {
            if (((mask >> index) & 1) == 0) {
                continue;
            }
            const bool written = ((channel->written_storage_buffers[stage] >> index) & 1) != 0;
            binding_line(fmt::format("sb{}.{}", stage, index), channel->storage_buffers[stage][index],
                         0, BINDING_CAP, written ? "w=1" : "w=0", false, true, written ? 0 : 16);
        }
    }
}

u64 RasterizerD3D12::GlitchTargetSignature() const {
    const auto& regs = maxwell3d->regs;
    const u32 count = std::min<u32>(regs.rt_control.count, NUM_RT);
    u64 signature = 0xcbf29ce484222325ULL;
    const auto mix = [&signature](u64 value) {
        signature = (signature ^ value) * 0x100000001b3ULL;
    };
    mix(count);
    for (u32 index = 0; index < count; ++index) {
        const auto& rt = regs.rt[index];
        mix(regs.rt_control.Map(index));
        mix(rt.Address());
        mix(static_cast<u64>(rt.format) | u64{rt.width} << 16 | u64{rt.height} << 40);
    }
    mix(regs.zeta_enable ? regs.zeta.Address() : 0);
    return signature;
}

namespace {
/// Draws of one indirect call whose arguments the capture reads back.
constexpr u64 INDIRECT_ARGS_DRAWS = 64;

u64 IndirectArgsBytes(const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams& params) {
    const u64 words = params.is_indexed ? 5 : 4;
    const u64 stride = params.stride != 0 ? params.stride : words * sizeof(u32);
    const u64 draws = std::min<u64>(std::max<u64>(params.max_draw_counts, 1),
                                    INDIRECT_ARGS_DRAWS);
    return (draws - 1) * stride + words * sizeof(u32);
}
} // namespace

RasterizerD3D12::GlitchIndirect RasterizerD3D12::GlitchIndirectBefore(
    const IndirectParams& params) {
    GlitchIndirect out;
    if (const std::optional<DAddr> addr = gpu_memory->GpuToCpuAddress(params.indirect_start_address)) {
        const u64 size = IndirectArgsBytes(params);
        out.mapped = true;
        out.cpu_modified = buffer_cache.IsRegionCpuModified(*addr, size);
        out.gpu_modified = buffer_cache.IsRegionGpuModified(*addr, size);
    }
    return out;
}

void RasterizerD3D12::GlitchIndirectArgs(const IndirectParams& params,
                                         const GlitchIndirect& before, Buffer* buffer,
                                         u32 offset) {
    const u32 frame = glitch_hunt.Frame();
    const u32 call = glitch_hunt.PeekCall();
    const u64 words = params.is_indexed ? 5 : 4;
    const u64 stride = params.stride != 0 ? params.stride : words * sizeof(u32);
    u64 size = IndirectArgsBytes(params);
    // What guest memory holds, unflushed: stale when only the GPU wrote the arguments.
    std::vector<u32> guest(size / sizeof(u32));
    gpu_memory->ReadBlockUnsafe(params.indirect_start_address, guest.data(),
                                guest.size() * sizeof(u32));
    std::string guest_text;
    for (const u32 word : guest) {
        guest_text += fmt::format("{}{}", guest_text.empty() ? "" : ",", word);
    }
    u32 guest_count = 0;
    if (params.include_count) {
        gpu_memory->ReadBlockUnsafe(params.count_start_address, &guest_count, sizeof(guest_count));
    }
    const std::optional<DAddr> dev = gpu_memory->GpuToCpuAddress(params.indirect_start_address);
    const bool gpu_after = dev && buffer_cache.IsRegionGpuModified(*dev, size);
    const u64 room = buffer->SizeBytes() > offset ? buffer->SizeBytes() - offset : 0;
    glitch_args_words = guest;
    glitch_args_call = call;
    glitch_args_stride = static_cast<u32>(stride / sizeof(u32));
    glitch_args_count = static_cast<u32>(words);
    glitch_hunt.Line(fmt::format(
        "A f={} n={} gva={:x} dev={:x} host={:x}+{:x} draws={} stride={} words={} count={} "
        "cgva={:x} mapped={} cpu_mod={} gpu_mod={} gpu_after={} room={} guest_count={} guest={}",
        frame, call, params.indirect_start_address, dev.value_or(0), buffer->CpuAddr(), offset,
        params.max_draw_counts, stride, words, params.include_count ? 1 : 0,
        params.include_count ? params.count_start_address : 0, before.mapped ? 1 : 0,
        before.cpu_modified ? 1 : 0, before.gpu_modified ? 1 : 0, gpu_after ? 1 : 0, room,
        guest_count, guest_text));
    size = std::min(size, room) & ~u64{3};
    if (size != 0 && buffer->Handle()) {
        // The words ExecuteIndirect reads: copied at the same point of the command list.
        glitch_hunt.InspectWords(call, "args", size, sizeof(u32), "",
                                 [&](ID3D12Resource* dst, u64 at) {
                                     scheduler.CommandList()->CopyBufferRegion(
                                         dst, at, buffer->Handle(), offset, size);
                                 });
    }
    if (params.include_count) {
        const auto [count_buffer, count_src] = buffer_cache.GetDrawIndirectCount();
        if (count_buffer && count_buffer->Handle()) {
            count_buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
            glitch_hunt.InspectWords(call, "count", sizeof(u32), sizeof(u32), "",
                                     [&](ID3D12Resource* dst, u64 at) {
                                         scheduler.CommandList()->CopyBufferRegion(
                                             dst, at, count_buffer->Handle(), count_src,
                                             sizeof(u32));
                                     });
        }
    }
}

void RasterizerD3D12::GlitchPassNote(const Framebuffer* framebuffer, u32 call, bool clear) {
    if (glitch_pass.active && glitch_pass.framebuffer != framebuffer) {
        // The framebuffer changed while the registers did not: read after this call.
        GlitchPassEnd("late");
    }
    if (!glitch_pass.active) {
        glitch_pass.active = true;
        glitch_pass.framebuffer = framebuffer;
        glitch_pass.images = framebuffer->Images();
        glitch_pass.signature = GlitchTargetSignature();
        glitch_pass.first_call = call;
        for (u32 index = 0; index < NUM_RT; ++index) {
            const Image* const image = framebuffer->ColorImage(index);
            if (!image) {
                continue;
            }
            const auto base = framebuffer->ColorBase(index);
            glitch_pass.ids[index] = framebuffer->ColorImageId(index);
            glitch_pass.resources[index] = framebuffer->ColorOnCopy(index)
                                               ? image->DiagnosticReinterpreted()
                                               : image->Handle();
            glitch_pass.formats[index] = framebuffer->ColorFormat(index);
            glitch_pass.subresources[index] = image->Subresource(base.level, base.layer);
        }
    }
    glitch_pass.last_call = call;
    ++(clear ? glitch_pass.clears : glitch_pass.draws);
}

void RasterizerD3D12::GlitchPassCheck(std::string_view why, bool force) {
    if (!glitch_pass.active) {
        return;
    }
    if (force || GlitchTargetSignature() != glitch_pass.signature) {
        GlitchPassEnd(why);
    }
}

void RasterizerD3D12::GlitchPassEnd(std::string_view why) {
    GlitchPass pass = std::move(glitch_pass);
    glitch_pass = {};
    if (!glitch_hunt.Capturing()) {
        return;
    }
    std::scoped_lock lock{texture_cache.mutex};
    for (u32 index = 0; index < NUM_RT; ++index) {
        ID3D12Resource* const resource = pass.resources[index].Get();
        if (!resource) {
            continue;
        }
        const std::string head = fmt::format(
            "n={}-{} draws={} clears={} fb={:x} rt={} img={} res={:x} why={}", pass.first_call,
            pass.last_call, pass.draws, pass.clears,
            reinterpret_cast<std::uintptr_t>(pass.framebuffer) & 0xffffff, index,
            pass.ids[index].index, reinterpret_cast<std::uintptr_t>(resource) & 0xffffff, why);
        // The slot may hold another image by now: it must still track the held resource.
        Image* image = pass.images && pass.ids[index] != ImageId{} ? &(*pass.images)[pass.ids[index]]
                                                                   : nullptr;
        const std::optional<D3D12_RESOURCE_STATES> previous =
            image ? image->DiagnosticState(resource) : std::nullopt;
        if (!previous) {
            glitch_hunt.Line(fmt::format("V f={} {} skip=gone", glitch_hunt.Frame(), head));
            continue;
        }
        glitch_hunt.ReadTarget(head, resource, pass.subresources[index], pass.formats[index],
                               [&](bool to_copy) {
                                   image->DiagnosticTransition(
                                       resource,
                                       to_copy ? D3D12_RESOURCE_STATE_COPY_SOURCE : *previous);
                               });
    }
}

void RasterizerD3D12::DiagnosticsFrameEnd(double interval_ms) {
    scheduler.RefreshDiagToggles();
    if (VideoCommon::ReadbackTrace::FrameWait()) [[unlikely]] {
        scheduler.Finish();
    }
    const u64 lost_total = LostVertexSlotDraws();
    const u64 lost = lost_total - glitch_lost_vertex_draws;
    glitch_lost_vertex_draws = lost_total;
    GlitchPassCheck("end", true);
    if (!glitch_hunt.Capturing()) {
        glitch_hunt.FrameEnd(interval_ms, {});
        return;
    }
    const auto& cond = conditional_rendering.LastDecision();
    glitch_hunt.FrameEnd(interval_ms,
                         fmt::format("lost_vb_draws={} lost_total={} cond_serial={} "
                                     "force_visible={}",
                                     lost, lost_total, cond.serial,
                                     VideoCommon::force_occlusion_visible.load() ? 1 : 0));
}

} // namespace D3D12
