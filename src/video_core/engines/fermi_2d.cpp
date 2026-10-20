// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <utility>
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/sw_blitter/blitter.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

namespace Tegra::Engines {

using namespace Texture;

Fermi2D::Fermi2D(MemoryManager& memory_manager_) : memory_manager{memory_manager_} {
    sw_blitter = std::make_unique<Blitter::SoftwareBlitEngine>(memory_manager);
    // Nvidia's OpenGL driver seems to assume these values
    regs.src.depth = 1;
    regs.dst.depth = 1;

    execution_mask.reset();
    execution_mask[FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1] = true;
}

Fermi2D::~Fermi2D() {
    if (unsupported_count != 0) {
        LOG_WARNING(HW_GPU, "Fermi2D diagnostic summary: noncopy={} distinct_snapshots={} capacity=32 operation_writes={}",
                    unsupported_count, unsupported_state_count, operation_write_count);
    }
}

void Fermi2D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void Fermi2D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid Fermi2D register, increase the size of the Regs structure");
    regs.reg_array[method] = method_argument;
    if (method == FERMI2D_REG_INDEX(operation)) {
        ++operation_write_count;
    }

    if (method == FERMI2D_REG_INDEX(pixels_from_memory.src_y0) + 1) {
        Blit();
    }
}

void Fermi2D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    for (u32 i = 0; i < amount; ++i) {
        CallMethod(system, method, base_start[i], methods_pending - i <= 1);
    }
}

void Fermi2D::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
        if (method == FERMI2D_REG_INDEX(operation)) {
            ++operation_write_count;
        }
    }
    method_sink.clear();
}

void Fermi2D::Blit() {
    LOG_DEBUG(HW_GPU, "called. source address={:#x}, destination address={:#x}", regs.src.Address(), regs.dst.Address());

    const bool noncopy = regs.operation != Operation::SrcCopy;
    bool diagnostic = false;
    if (noncopy) {
        ++unsupported_count;
        std::array<u32, 18> state{};
        std::copy_n(regs.reg_array.begin() + 0x280 / 4, 14, state.begin());
        state[14] = static_cast<u32>(regs.src.format);
        state[15] = static_cast<u32>(regs.dst.format);
        state[16] = regs.reg_array[0x88c / 4];
        state[17] = (static_cast<u32>(regs.src.linear) << 16) |
                    static_cast<u32>(regs.dst.linear);
        const auto end = unsupported_states.begin() + unsupported_state_count;
        if (unsupported_state_count < unsupported_states.size() &&
            std::find(unsupported_states.begin(), end, state) == end) {
            unsupported_states[unsupported_state_count++] = state;
            diagnostic = true;
        }
    }
    UNIMPLEMENTED_IF_MSG(noncopy, "Operation is not copy: operation={} rop={:#x}",
                         static_cast<u32>(regs.operation), regs.reg_array[0x2a0 / 4]);
    if (diagnostic) {
        constexpr std::array names{"SrcCopyAnd", "ROPAnd", "Blend", "SrcCopy", "ROP",
                                   "SrcCopyPremult", "BlendPremult"};
        const auto operation = static_cast<u32>(regs.operation);
        LOG_WARNING(HW_GPU, "Fermi2D diagnostic: snapshot={} occurrence={} operation={} ({}) operation_writes={} raw_register_offsets=bytes values=hex; behavior unchanged",
                    unsupported_state_count, unsupported_count, operation,
                    operation < names.size() ? names[operation] : "Unknown", operation_write_count);
        // Exact raw state keeps enum domains, fixed-point coordinates and control bits intact.
        for (const auto range : {std::pair{0x200u, 0x258u}, std::pair{0x260u, 0x2b8u},
                                 std::pair{0x2e8u, 0x300u}, std::pair{0x880u, 0x8e0u}}) {
            for (u32 offset = range.first; offset < range.second; offset += 4) {
                LOG_WARNING(HW_GPU, "Fermi2D registers: snapshot={} offset={:#05x} value={:#010x}",
                            unsupported_state_count, offset, regs.reg_array[offset / 4]);
            }
        }
    }
    UNIMPLEMENTED_IF_MSG(regs.src.layer != 0, "Source layer is not zero");
    UNIMPLEMENTED_IF_MSG(regs.dst.layer != 0, "Destination layer is not zero");
    UNIMPLEMENTED_IF_MSG(regs.src.depth != 1, "Source depth is not one");
    UNIMPLEMENTED_IF_MSG(regs.clip_enable != 0, "Clipped blit enabled");

    const auto& args = regs.pixels_from_memory;
    constexpr s64 null_derivative = 1ULL << 32;
    Surface src = regs.src;
    const auto bytes_per_pixel = BytesPerBlock(PixelFormatFromRenderTargetFormat(src.format));
    const bool delegate_to_gpu = src.width > 512 && src.height > 512 && bytes_per_pixel <= 8 &&
                                 src.format != regs.dst.format;

    auto srcX = args.src_x0;
    auto srcY = args.src_y0;
    if (args.sample_mode.origin == Origin::Corner) {
        srcX -= (args.du_dx >> 33) << 32;
        srcY -= (args.dv_dy >> 33) << 32;
    }

    Config config{
        .operation = regs.operation,
        .filter = args.sample_mode.filter,
        .must_accelerate =
            args.du_dx != null_derivative || args.dv_dy != null_derivative || delegate_to_gpu,
        .dst_x0 = args.dst_x0,
        .dst_y0 = args.dst_y0,
        .dst_x1 = args.dst_x0 + args.dst_width,
        .dst_y1 = args.dst_y0 + args.dst_height,
        .src_x0 = static_cast<s32>(srcX >> 32),
        .src_y0 = static_cast<s32>(srcY >> 32),
        .src_x1 = static_cast<s32>((srcX + args.du_dx * args.dst_width) >> 32),
        .src_y1 = static_cast<s32>((srcY + args.dv_dy * args.dst_height) >> 32),
    };

    const auto need_align_to_pitch =
        src.linear == Tegra::Engines::Fermi2D::MemoryLayout::Pitch &&
        static_cast<s32>(src.width) == config.src_x1 &&
        config.src_x1 > static_cast<s32>(src.pitch / bytes_per_pixel) && config.src_x0 > 0;
    if (need_align_to_pitch) {
        auto address = src.Address() + config.src_x0 * bytes_per_pixel;
        src.addr_upper = static_cast<u32>(address >> 32);
        src.addr_lower = static_cast<u32>(address);
        src.width -= config.src_x0;
        config.src_x1 -= config.src_x0;
        config.src_x0 = 0;
    }

    memory_manager.FlushCaching();
    const auto started = diagnostic ? std::chrono::steady_clock::now() :
                                     std::chrono::steady_clock::time_point{};
    const bool accelerated = rasterizer->AccelerateSurfaceCopy(src, regs.dst, config);
    if (!accelerated) {
        sw_blitter->Blit(src, regs.dst, config);
    }
    if (diagnostic) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        LOG_WARNING(HW_GPU, "Fermi2D result: snapshot={} accelerated={} host_elapsed_us={} src={:#x} dst={:#x} src_rect=({},{})-({},{}) dst_rect=({},{})-({},{}) pitch_adjusted={}; elapsed excludes FlushCaching and is not GPU time",
                    unsupported_state_count, accelerated, elapsed, src.Address(), regs.dst.Address(),
                    config.src_x0, config.src_y0, config.src_x1, config.src_y1,
                    config.dst_x0, config.dst_y0, config.dst_x1, config.dst_y1, need_align_to_pitch);
    }
}

} // namespace Tegra::Engines
