// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The tessellation workload: every frame the same indexed draws of quad patches at fixed levels over
// the whole screen, or (flat) the triangles that tessellation makes, as indexed triangle lists. Both
// runs rasterize the same triangles with the same colors, so what one costs over the other is the
// tessellation. Its numbers come from the emulator's perf windows (gpu_profile=1).

#include "harness.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t Width = 1280, Height = 720;
constexpr uint32_t Swapchain = 3;
constexpr uint32_t CommandBytes = 8 * 1024 * 1024;

constexpr std::array<DkVtxAttribState, 3> Attribs{{
    {0, 0, offsetof(Vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, offsetof(Vertex, color), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
}};
constexpr std::array<DkVtxBufferState, 1> Buffers{{{sizeof(Vertex), 0}}};

uint32_t Align(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

void RunTessPerformanceLoop(uint32_t frames, const TessPerformance& config) {
    const uint32_t level = config.level;
    const uint32_t total = config.draws * config.patches;
    // Patches on a grid with the screen's aspect.
    const uint32_t columns = static_cast<uint32_t>(std::ceil(std::sqrt(total * 16.0 / 9.0)));
    const uint32_t rows = (total + columns - 1) / columns;
    const float w = 2.0f / columns, h = 2.0f / rows;

    const uint32_t patch_vertices = config.flat ? (level + 1) * (level + 1) : 4;
    const uint32_t patch_indices = config.flat ? level * level * 6 : 4;
    const uint32_t draw_vertices = config.patches * patch_vertices;
    const uint32_t draw_indices = config.patches * patch_indices;
    const uint32_t vertex_bytes = Align(total * patch_vertices * sizeof(Vertex), 256);
    const uint32_t index_bytes = Align(draw_indices * sizeof(uint16_t), 256);

    dk::UniqueDevice device = dk::DeviceMaker{}.create();
    dk::UniqueQueue queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();
    CMemPool images{device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16 * 1024 * 1024};
    CMemPool code{device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                  1 * 1024 * 1024};
    CMemPool data{device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                  Align(vertex_bytes + index_bytes + CommandBytes * Swapchain + 1024 * 1024, 64 * 1024)};

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(Width, Height)
        .initialize(layout);
    dk::Image targets[Swapchain];
    const DkImage* target_list[Swapchain];
    CMemPool::Handle target_memory[Swapchain];
    for (uint32_t i = 0; i < Swapchain; ++i) {
        target_memory[i] = images.allocate(layout.getSize(), layout.getAlignment());
        targets[i].initialize(layout, target_memory[i].getMemBlock(), target_memory[i].getOffset());
        target_list[i] = &targets[i];
    }
    dk::UniqueSwapchain swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), target_list, Swapchain}.create();

    CShader vertex_shader, eval_shader, fragment_shader;
    vertex_shader.load(code, config.flat ? "romfs:/shaders/basic_vsh.dksh" : "romfs:/shaders/pos_only_vsh.dksh");
    if (!config.flat) eval_shader.load(code, "romfs:/shaders/tess_quads_pos_tesh.dksh");
    fragment_shader.load(code, "romfs:/shaders/color_fsh.dksh");

    // Each patch's corners, or its grid of level + 1 by level + 1 vertices colored (u, v, 0, 1) as the
    // evaluation shader colors its domain points.
    CMemPool::Handle vertices = data.allocate(vertex_bytes, 256);
    auto* vertex_cpu = static_cast<Vertex*>(vertices.getCpuAddr());
    for (uint32_t patch = 0; patch < total; ++patch) {
        const float x0 = -1.0f + (patch % columns) * w, y0 = 1.0f - (patch / columns) * h;
        Vertex* out = vertex_cpu + patch * patch_vertices;
        const auto at = [&](float u, float v) {
            return Vertex{{x0 + u * w, y0 - v * h, 0.0f}, {u, v, 0.0f, 1.0f}, {u, v}};
        };
        if (config.flat) {
            for (uint32_t j = 0; j <= level; ++j) {
                for (uint32_t i = 0; i <= level; ++i) {
                    *out++ = at(static_cast<float>(i) / level, static_cast<float>(j) / level);
                }
            }
        } else {
            out[0] = at(0, 0);
            out[1] = at(0, 1);
            out[2] = at(1, 0);
            out[3] = at(1, 1);
        }
    }
    // One draw's indices, relative to its first vertex; every draw uses them.
    CMemPool::Handle indices = data.allocate(index_bytes, 256);
    auto* index_cpu = static_cast<uint16_t*>(indices.getCpuAddr());
    for (uint32_t patch = 0; patch < config.patches; ++patch) {
        const uint32_t base = patch * patch_vertices;
        if (!config.flat) {
            for (uint32_t i = 0; i < 4; ++i) *index_cpu++ = static_cast<uint16_t>(base + i);
            continue;
        }
        for (uint32_t j = 0; j < level; ++j) {
            for (uint32_t i = 0; i < level; ++i) {
                const uint32_t a = base + j * (level + 1) + i, b = a + 1, c = a + level + 1, d = c + 1;
                const uint16_t quad[6] = {static_cast<uint16_t>(a), static_cast<uint16_t>(c), static_cast<uint16_t>(b),
                                          static_cast<uint16_t>(b), static_cast<uint16_t>(c), static_cast<uint16_t>(d)};
                std::memcpy(index_cpu, quad, sizeof(quad));
                index_cpu += 6;
            }
        }
    }

    CMemPool::Handle command_memory = data.allocate(CommandBytes * Swapchain);
    dk::UniqueCmdBuf cmd[Swapchain];
    for (uint32_t i = 0; i < Swapchain; ++i) {
        cmd[i] = dk::CmdBufMaker{device}.create();
        cmd[i].addMemory(command_memory.getMemBlock(), command_memory.getOffset() + i * CommandBytes, CommandBytes);
    }

    dk::RasterizerState rasterizer;
    rasterizer.setCullMode(DkFace_None);
    dk::DepthStencilState depth;
    depth.setDepthTestEnable(false).setDepthWriteEnable(false);
    const float levels = static_cast<float>(level);

    Report("PERF_START %u tess draws %u patches %u level %u flat %u", frames, config.draws, config.patches, level,
           config.flat ? 1U : 0U);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const int slot = queue.acquireImage(swapchain);
        dk::UniqueCmdBuf& list = cmd[slot];
        list.clear();
        dk::ImageView target{targets[slot]};
        list.bindRenderTargets(&target);
        list.setViewports(0, {{0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 0.0f, 1.0f}});
        list.setScissors(0, {{0, 0, Width, Height}});
        list.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
        list.bindRasterizerState(rasterizer);
        list.bindColorState(dk::ColorState{});
        list.bindColorWriteState(dk::ColorWriteState{});
        list.bindDepthStencilState(depth);
        list.bindMultisampleState(dk::MultisampleState{});
        list.bindVtxAttribState(Attribs);
        list.bindVtxBufferState(Buffers);
        list.bindVtxBuffer(0, vertices.getGpuAddr(), vertices.getSize());
        list.bindIdxBuffer(DkIdxFormat_Uint16, indices.getGpuAddr());
        if (config.flat) {
            list.bindShaders(DkStageFlag_GraphicsMask, {vertex_shader, fragment_shader});
        } else {
            list.bindShaders(DkStageFlag_GraphicsMask, {vertex_shader, eval_shader, fragment_shader});
            list.setPatchSize(4);
            list.setTessOuterLevels(levels, levels, levels, levels);
            list.setTessInnerLevels(levels, levels);
        }
        const DkPrimitive primitive = config.flat ? DkPrimitive_Triangles : DkPrimitive_Patches;
        for (uint32_t draw = 0; draw < config.draws; ++draw) {
            list.drawIndexed(primitive, draw_indices, 1, 0, draw * draw_vertices, 0);
        }
        queue.submitCommands(list.finishList());
        queue.presentImage(swapchain, slot);
        queue.waitIdle();
    }
    Report("PERF_DONE");
    queue.waitIdle();
}
