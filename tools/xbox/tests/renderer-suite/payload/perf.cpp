// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The performance layer's workload: a fixed number of frames, each the same, with the shape a game's
// frame has for the D3D12 backend. About 5000 draws, each with its own constants, a state change
// every few draws and four textures. Every frame is identical, so what suite.py compares between runs
// comes from the emulator and not from the content. Its numbers come from the emulator's own perf
// windows (gpu_profile=1); this side only says when it has finished.

#include "harness.h"

#include <cstring>

namespace {

constexpr uint32_t Width = 1280, Height = 720;
constexpr uint32_t Swapchain = 3;
constexpr uint32_t DrawsPerFrame = 5000;
constexpr uint32_t Columns = 100, Rows = 50;
constexpr uint32_t StateCount = 16;
constexpr uint32_t TextureCount = 4;
constexpr uint32_t CommandBytes = 8 * 1024 * 1024;

struct Constants {
    float offset[2];
    float scale[2];
    float color[4];
};

constexpr std::array<DkVtxAttribState, 3> Attribs{{
    {0, 0, offsetof(Vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, offsetof(Vertex, color), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
}};
constexpr std::array<DkVtxBufferState, 1> Buffers{{{sizeof(Vertex), 0}}};

} // namespace

void RunPerformanceLoop(uint32_t frames) {
    dk::UniqueDevice device = dk::DeviceMaker{}.create();
    dk::UniqueQueue queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();
    CMemPool images{device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 64 * 1024 * 1024};
    CMemPool code{device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                  1 * 1024 * 1024};
    CMemPool data{device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 64 * 1024 * 1024};

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

    CShader vertex_shader, fragment_shader;
    vertex_shader.load(code, "romfs:/shaders/basic_vsh.dksh");
    fragment_shader.load(code, "romfs:/shaders/two_textures_fsh.dksh");

    // Four small textures, each a different gray.
    CMemPool::Handle image_descriptors = data.allocate(TextureCount * sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    CMemPool::Handle sampler_descriptors = data.allocate(sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
    auto* image_descriptor_cpu = static_cast<dk::ImageDescriptor*>(image_descriptors.getCpuAddr());
    dk::Image textures[TextureCount];
    CMemPool::Handle staging = data.allocate(16 * 16 * 4 * TextureCount, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);

    CMemPool::Handle setup_memory = data.allocate(64 * 1024);
    dk::UniqueCmdBuf setup = dk::CmdBufMaker{device}.create();
    setup.addMemory(setup_memory.getMemBlock(), setup_memory.getOffset(), setup_memory.getSize());
    for (uint32_t i = 0; i < TextureCount; ++i) {
        dk::ImageLayout texture_layout;
        dk::ImageLayoutMaker{device}.setFormat(DkImageFormat_RGBA8_Unorm).setDimensions(16, 16).initialize(texture_layout);
        CMemPool::Handle memory = images.allocate(texture_layout.getSize(), texture_layout.getAlignment());
        textures[i].initialize(texture_layout, memory.getMemBlock(), memory.getOffset());
        uint8_t* texels = static_cast<uint8_t*>(staging.getCpuAddr()) + i * 16 * 16 * 4;
        std::memset(texels, static_cast<int>(60 + i * 60), 16 * 16 * 4);
        dk::ImageView view{textures[i]};
        setup.copyBufferToImage({staging.getGpuAddr() + i * 16 * 16 * 4, 0, 0}, view, {0, 0, 0, 16, 16, 1});
        image_descriptor_cpu[i].initialize(view);
    }
    dk::Sampler sampler;
    static_cast<dk::SamplerDescriptor*>(sampler_descriptors.getCpuAddr())->initialize(sampler);
    queue.submitCommands(setup.finishList());
    queue.waitIdle();

    // A quad at the origin, moved by constants that the vertex shader never sees: the quads are in
    // different places because each has its own vertices, as a game's would.
    CMemPool::Handle vertices = data.allocate(sizeof(Vertex) * 4 * DrawsPerFrame, alignof(Vertex));
    auto* vertex_cpu = static_cast<Vertex*>(vertices.getCpuAddr());
    for (uint32_t i = 0; i < DrawsPerFrame; ++i) {
        const float x = (i % Columns) * 2.0f / Columns - 1.0f, y = 1.0f - ((i / Columns) % Rows) * 2.0f / Rows;
        const float w = 2.0f / Columns, h = 2.0f / Rows;
        const float z = (i % 8) / 8.0f;
        const Vertex quad[4] = {{{x, y, z}, {1, 1, 1, 1}, {0, 0}},
                                {{x, y - h, z}, {1, 1, 1, 1}, {0, 1}},
                                {{x + w, y, z}, {1, 1, 1, 1}, {1, 0}},
                                {{x + w, y - h, z}, {1, 1, 1, 1}, {1, 1}}};
        std::memcpy(&vertex_cpu[i * 4], quad, sizeof(quad));
    }
    CMemPool::Handle constants = data.allocate(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);

    CMemPool::Handle command_memory = data.allocate(CommandBytes * Swapchain);
    dk::UniqueCmdBuf cmd[Swapchain];
    for (uint32_t i = 0; i < Swapchain; ++i) {
        cmd[i] = dk::CmdBufMaker{device}.create();
        cmd[i].addMemory(command_memory.getMemBlock(), command_memory.getOffset() + i * CommandBytes, CommandBytes);
    }

    dk::ColorState blend_on, blend_off;
    blend_on.setBlendEnable(0, true);
    dk::BlendState blend;
    blend.setFactors(DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_Zero);
    dk::RasterizerState rasterizer;
    rasterizer.setCullMode(DkFace_None);

    Report("PERF_START %u", frames);
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
        list.bindColorWriteState(dk::ColorWriteState{});
        list.bindMultisampleState(dk::MultisampleState{});
        list.bindVtxAttribState(Attribs);
        list.bindVtxBufferState(Buffers);
        list.bindVtxBuffer(0, vertices.getGpuAddr(), vertices.getSize());
        list.bindImageDescriptorSet(image_descriptors.getGpuAddr(), TextureCount);
        list.bindSamplerDescriptorSet(sampler_descriptors.getGpuAddr(), 1);
        list.bindShaders(DkStageFlag_GraphicsMask, {vertex_shader, fragment_shader});
        list.bindBlendStates(0, blend);
        list.bindUniformBuffer(DkStage_Fragment, 0, constants.getGpuAddr(), constants.getSize());
        for (uint32_t i = 0; i < DrawsPerFrame; ++i) {
            const uint32_t state = i % StateCount;
            if (i % 4 == 0) {
                dk::DepthStencilState depth;
                depth.setDepthTestEnable(false).setDepthWriteEnable(false);
                list.bindColorState((state & 1) ? blend_on : blend_off);
                list.bindDepthStencilState(depth);
            }
            if (i % 8 == 0) {
                list.bindTextures(DkStage_Fragment, 0,
                                  {dkMakeTextureHandle((i / 8) % TextureCount, 0),
                                   dkMakeTextureHandle(((i / 8) + 1) % TextureCount, 0)});
            }
            const Constants values{{0, 0}, {1, 1}, {1, 1, 1, 1}};
            list.pushConstants(constants.getGpuAddr(), constants.getSize(), 0, sizeof(values), &values);
            list.draw(DkPrimitive_TriangleStrip, 4, 1, i * 4, 0);
        }
        queue.submitCommands(list.finishList());
        queue.presentImage(swapchain, slot);
        queue.waitIdle();
    }
    Report("PERF_DONE");
    queue.waitIdle();
}
