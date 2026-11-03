// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "harness.h"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace {

constexpr uint32_t ArenaSize = 24 * 1024 * 1024;
constexpr uint32_t CommandSize = 2 * 1024 * 1024;
constexpr uint32_t ReadbackSize = 2 * 1024 * 1024;
constexpr uint32_t MaxDescriptors = 64;
constexpr uint32_t ImagePoolSize = 256 * 1024 * 1024;
// Images of finished scenes keep their memory until this much is held, so a scene's images never
// land where the scene before left one of another format: the texture cache would match the old
// image there, and a scene would test what the previous one drew. format_alias_reuse tests that
// on purpose.
constexpr uint32_t RetiredImageLimit = 192 * 1024 * 1024;

constexpr std::array<DkVtxAttribState, 3> VertexAttribs{{
    {0, 0, offsetof(Vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, offsetof(Vertex, color), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
}};
constexpr std::array<DkVtxBufferState, 1> VertexBuffers{{{sizeof(Vertex), 0}}};

constexpr FormatInfo Formats[] = {
    {DkImageFormat_R8_Unorm, "R8_Unorm", 1, 1, 8, Kind::Unorm},
    {DkImageFormat_R8_Snorm, "R8_Snorm", 1, 1, 8, Kind::Snorm},
    {DkImageFormat_R8_Uint, "R8_Uint", 1, 1, 8, Kind::Uint},
    {DkImageFormat_R8_Sint, "R8_Sint", 1, 1, 8, Kind::Sint},
    {DkImageFormat_R16_Float, "R16_Float", 2, 1, 16, Kind::Float},
    {DkImageFormat_R16_Unorm, "R16_Unorm", 2, 1, 16, Kind::Unorm},
    {DkImageFormat_R16_Snorm, "R16_Snorm", 2, 1, 16, Kind::Snorm},
    {DkImageFormat_R16_Uint, "R16_Uint", 2, 1, 16, Kind::Uint},
    {DkImageFormat_R16_Sint, "R16_Sint", 2, 1, 16, Kind::Sint},
    {DkImageFormat_R32_Float, "R32_Float", 4, 1, 32, Kind::Float},
    {DkImageFormat_R32_Uint, "R32_Uint", 4, 1, 32, Kind::Uint},
    {DkImageFormat_R32_Sint, "R32_Sint", 4, 1, 32, Kind::Sint},
    {DkImageFormat_RG8_Unorm, "RG8_Unorm", 2, 2, 8, Kind::Unorm},
    {DkImageFormat_RG8_Snorm, "RG8_Snorm", 2, 2, 8, Kind::Snorm},
    {DkImageFormat_RG8_Uint, "RG8_Uint", 2, 2, 8, Kind::Uint},
    {DkImageFormat_RG8_Sint, "RG8_Sint", 2, 2, 8, Kind::Sint},
    {DkImageFormat_RG16_Float, "RG16_Float", 4, 2, 16, Kind::Float},
    {DkImageFormat_RG16_Unorm, "RG16_Unorm", 4, 2, 16, Kind::Unorm},
    {DkImageFormat_RG16_Snorm, "RG16_Snorm", 4, 2, 16, Kind::Snorm},
    {DkImageFormat_RG16_Uint, "RG16_Uint", 4, 2, 16, Kind::Uint},
    {DkImageFormat_RG16_Sint, "RG16_Sint", 4, 2, 16, Kind::Sint},
    {DkImageFormat_RG32_Float, "RG32_Float", 8, 2, 32, Kind::Float},
    {DkImageFormat_RG32_Uint, "RG32_Uint", 8, 2, 32, Kind::Uint},
    {DkImageFormat_RG32_Sint, "RG32_Sint", 8, 2, 32, Kind::Sint},
    {DkImageFormat_RGBA8_Unorm, "RGBA8_Unorm", 4, 4, 8, Kind::Unorm},
    {DkImageFormat_RGBA8_Snorm, "RGBA8_Snorm", 4, 4, 8, Kind::Snorm},
    {DkImageFormat_RGBA8_Uint, "RGBA8_Uint", 4, 4, 8, Kind::Uint},
    {DkImageFormat_RGBA8_Sint, "RGBA8_Sint", 4, 4, 8, Kind::Sint},
    {DkImageFormat_RGBA16_Float, "RGBA16_Float", 8, 4, 16, Kind::Float},
    {DkImageFormat_RGBA16_Unorm, "RGBA16_Unorm", 8, 4, 16, Kind::Unorm},
    {DkImageFormat_RGBA16_Snorm, "RGBA16_Snorm", 8, 4, 16, Kind::Snorm},
    {DkImageFormat_RGBA16_Uint, "RGBA16_Uint", 8, 4, 16, Kind::Uint},
    {DkImageFormat_RGBA16_Sint, "RGBA16_Sint", 8, 4, 16, Kind::Sint},
    {DkImageFormat_RGBA32_Float, "RGBA32_Float", 16, 4, 32, Kind::Float},
    {DkImageFormat_RGBA32_Uint, "RGBA32_Uint", 16, 4, 32, Kind::Uint},
    {DkImageFormat_RGBA32_Sint, "RGBA32_Sint", 16, 4, 32, Kind::Sint},
    {DkImageFormat_RGBA8_Unorm_sRGB, "RGBA8_sRGB", 4, 4, 8, Kind::Unorm},
    {DkImageFormat_BGRA8_Unorm, "BGRA8_Unorm", 4, 0, 8, Kind::Raw},
    {DkImageFormat_BGRA8_Unorm_sRGB, "BGRA8_sRGB", 4, 0, 8, Kind::Raw},
    {DkImageFormat_RGB10A2_Unorm, "RGB10A2_Unorm", 4, 0, 10, Kind::Raw},
    {DkImageFormat_RGB10A2_Uint, "RGB10A2_Uint", 4, 0, 10, Kind::Raw},
    {DkImageFormat_RG11B10_Float, "RG11B10_Float", 4, 0, 11, Kind::Raw},
    {DkImageFormat_RGB565_Unorm, "RGB565_Unorm", 2, 0, 5, Kind::Raw},
    {DkImageFormat_BGR565_Unorm, "BGR565_Unorm", 2, 0, 5, Kind::Raw},
    {DkImageFormat_RGB5A1_Unorm, "RGB5A1_Unorm", 2, 0, 5, Kind::Raw},
    {DkImageFormat_BGR5A1_Unorm, "BGR5A1_Unorm", 2, 0, 5, Kind::Raw},
    {DkImageFormat_RGBA4_Unorm, "RGBA4_Unorm", 2, 0, 4, Kind::Raw},
    {DkImageFormat_RGBX8_Unorm, "RGBX8_Unorm", 4, 0, 8, Kind::Raw},
    {DkImageFormat_BGRX8_Unorm, "BGRX8_Unorm", 4, 0, 8, Kind::Raw},
    {DkImageFormat_E5BGR9_Float, "E5BGR9_Float", 4, 0, 9, Kind::Raw},
};

double HalfToDouble(uint16_t half) {
    const int sign = (half >> 15) & 1;
    const int exponent = (half >> 10) & 0x1F;
    const int mantissa = half & 0x3FF;
    double value;
    if (exponent == 0) {
        value = std::ldexp(mantissa, -24);
    } else if (exponent == 31) {
        value = mantissa ? NAN : HUGE_VAL;
    } else {
        value = std::ldexp(mantissa + 1024, exponent - 25);
    }
    return sign ? -value : value;
}

double DecodeChannel(const uint8_t* data, const FormatInfo& info) {
    uint16_t u16;
    uint32_t u32;
    std::memcpy(&u16, data, info.bits >= 16 ? 2 : 0);
    std::memcpy(&u32, data, info.bits == 32 ? 4 : 0);
    switch (info.kind) {
    case Kind::Unorm:
        return info.bits == 8 ? data[0] / 255.0 : u16 / 65535.0;
    case Kind::Snorm:
        if (info.bits == 8) return std::fmax(static_cast<int8_t>(data[0]) / 127.0, -1.0);
        return std::fmax(static_cast<int16_t>(u16) / 32767.0, -1.0);
    case Kind::Uint:
        if (info.bits == 8) return data[0];
        return info.bits == 16 ? u16 : u32;
    case Kind::Sint:
        if (info.bits == 8) return static_cast<int8_t>(data[0]);
        return info.bits == 16 ? static_cast<int16_t>(u16) : static_cast<int32_t>(u32);
    case Kind::Float: {
        if (info.bits == 16) return HalfToDouble(u16);
        float value;
        std::memcpy(&value, &u32, 4);
        return value;
    }
    case Kind::Raw:
        break;
    }
    return 0.0;
}

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
    Report("DEKO_ERROR %s %d %s", context ? context : "", static_cast<int>(result), message ? message : "");
    if (result != DkResult_Success) {
        Report("ABORT");
        // Ends the emulator's run now; the host then skips this scene and runs the rest.
        static const char done[] = "JADYX_GFX_DONE";
        svcOutputDebugString(done, sizeof(done) - 1);
        svcExitProcess();
    }
}

} // namespace

const FormatInfo& InfoOf(DkImageFormat format) {
    for (const FormatInfo& info : Formats) {
        if (info.format == format) return info;
    }
    Report("BAD_FORMAT %d", static_cast<int>(format));
    return Formats[0];
}

// Each line is its own open: the host sees everything up to a crash.
void Report(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (FILE* file = fopen("sdmc:/renderer_suite/results.txt", "a")) {
        fprintf(file, "%s\n", line);
        fclose(file);
    }
    svcOutputDebugString(line, length < static_cast<int>(sizeof(line)) ? length : sizeof(line) - 1);
}

Gpu::Gpu(uint32_t device_flags) {
    device = dk::DeviceMaker{}.setFlags(device_flags).setCbDebug(DebugCallback).create();
    queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute).create();
    pool_images.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, ImagePoolSize);
    pool_code.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                      4 * 1024 * 1024);
    pool_data.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 64 * 1024 * 1024);
    arena = pool_data->allocate(ArenaSize, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    readback = pool_data->allocate(ReadbackSize, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    image_descriptors = pool_data->allocate(MaxDescriptors * sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    sampler_descriptors =
        pool_data->allocate(MaxDescriptors * sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
    cmd = dk::CmdBufMaker{device}.create();
    cmd_memory = pool_data->allocate(CommandSize);
    cmd.addMemory(cmd_memory.getMemBlock(), cmd_memory.getOffset(), cmd_memory.getSize());
}

Gpu::~Gpu() {
    queue.waitIdle();
    cmd.clear();
    for (CMemPool::Handle& memory : image_memory) memory.destroy();
    image_memory.clear();
    for (CMemPool::Handle& memory : retired_memory) memory.destroy();
    retired_memory.clear();
    shaders.clear();
    readback.destroy();
    arena.destroy();
    image_descriptors.destroy();
    sampler_descriptors.destroy();
    cmd_memory.destroy();
}

Slice Gpu::Alloc(uint32_t size, uint32_t align) {
    uint32_t offset = (arena_used + align - 1) & ~(align - 1);
    if (offset + size > ArenaSize) {
        Fail("arena out of memory (%u bytes)", size);
        offset = 0;
    }
    arena_used = offset + size;
    return {static_cast<uint8_t*>(arena.getCpuAddr()) + offset, arena.getGpuAddr() + offset, size};
}

Image& Gpu::MakeImage(DkImageFormat format, uint32_t width, uint32_t height, uint32_t depth, uint32_t mips,
                      DkImageType type, uint32_t flags, DkMsMode ms, uint32_t pitch_stride) {
    dk::ImageLayout layout;
    dk::ImageLayoutMaker maker{device};
    maker.setType(type).setFlags(flags).setFormat(format).setMipLevels(mips).setMsMode(ms);
    if (pitch_stride != 0) maker.setPitchStride(pitch_stride);
    if (type == DkImageType_1D) {
        maker.setDimensions(width);
    } else if (type == DkImageType_2D || type == DkImageType_2DMS || type == DkImageType_Cubemap) {
        maker.setDimensions(width, height);
    } else {
        maker.setDimensions(width, height, depth);
    }
    maker.initialize(layout);
    image_memory.push_back(pool_images->allocate(layout.getSize(), layout.getAlignment()));
    Image& image = images.emplace_back();
    image.image.initialize(layout, image_memory.back().getMemBlock(), image_memory.back().getOffset());
    image.block = image_memory.back().getMemBlock();
    image.offset = image_memory.back().getOffset();
    image.format = format;
    image.width = width;
    image.height = height;
    image.depth = depth;
    image.mips = mips;
    return image;
}

Image& Gpu::Alias(const Image& base, DkImageFormat format) {
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
        .setFormat(format)
        .setDimensions(base.width, base.height)
        .initialize(layout);
    Image& image = images.emplace_back();
    image.image.initialize(layout, base.block, base.offset);
    image.block = base.block;
    image.offset = base.offset;
    image.format = format;
    image.width = base.width;
    image.height = base.height;
    image.depth = 1;
    image.mips = 1;
    return image;
}

void Gpu::Upload(const Image& image, const void* data, uint32_t size, uint32_t w, uint32_t h, uint32_t level,
                 uint32_t layer, uint32_t depth) {
    Slice staging = Alloc(size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    std::memcpy(staging.cpu, data, size);
    dk::ImageView view{image.image};
    view.setMipLevels(level, 1);
    // A 3D image has slices, not layers, and deko3d refuses a layer range on it.
    if (depth == 1) view.setLayers(layer, 1);
    cmd.copyBufferToImage({staging.gpu, 0, 0}, view, {0, 0, 0, w, h, depth});
    Barrier();
}

const dk::Shader* Gpu::Shader(const char* name) {
    auto found = shaders.find(name);
    if (found == shaders.end()) {
        auto shader = std::make_unique<CShader>();
        char path[128];
        snprintf(path, sizeof(path), "romfs:/shaders/%s.dksh", name);
        if (!shader->load(*pool_code, path)) {
            Fail("shader %s did not load", name);
        }
        found = shaders.emplace(name, std::move(shader)).first;
    }
    return *found->second;
}

void Gpu::RecordDefaultState() {
    const uint32_t width = current_target->width;
    const uint32_t height = current_target->height;
    cmd.setViewports(0, {{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f}});
    cmd.setScissors(0, {{0, 0, width, height}});
    const DkViewportSwizzle identity{DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
    cmd.setViewportSwizzles(0, identity);
    dk::RasterizerState rasterizer;
    rasterizer.setCullMode(DkFace_None);
    cmd.bindRasterizerState(rasterizer);
    cmd.bindColorState(dk::ColorState{});
    cmd.bindColorWriteState(dk::ColorWriteState{});
    dk::DepthStencilState depth_stencil;
    depth_stencil.setDepthTestEnable(false).setDepthWriteEnable(false);
    cmd.bindDepthStencilState(depth_stencil);
    cmd.bindMultisampleState(dk::MultisampleState{});
    cmd.setSampleMask(0xFFFFFFFF);
    cmd.setDepthBounds(false, 0.0f, 1.0f);
    cmd.setPrimitiveRestart(false, 0xFFFFFFFF);
    cmd.bindVtxAttribState(VertexAttribs);
    cmd.bindVtxBufferState(VertexBuffers);
    Shaders("basic_vsh", "color_fsh");
    cmd.bindImageDescriptorSet(image_descriptors.getGpuAddr(), MaxDescriptors);
    cmd.bindSamplerDescriptorSet(sampler_descriptors.getGpuAddr(), MaxDescriptors);
}

void Gpu::DefaultState() {
    RecordDefaultState();
}

void Gpu::Barrier() {
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
}

uint32_t Gpu::Width() const {
    return current_target->width;
}

uint32_t Gpu::Height() const {
    return current_target->height;
}

bool Gpu::PatchCommands(const uint32_t* from, const uint32_t* to, uint32_t count) {
    uint32_t* const words = static_cast<uint32_t*>(cmd_memory.getCpuAddr());
    const uint32_t total = cmd_memory.getSize() / sizeof(uint32_t);
    bool found = false;
    for (uint32_t i = 0; i + count <= total; ++i) {
        if (memcmp(words + i, from, count * sizeof(uint32_t)) == 0) {
            memcpy(words + i, to, count * sizeof(uint32_t));
            found = true;
        }
    }
    return found;
}

Image& Gpu::Begin(DkImageFormat format, uint32_t size) {
    Image& target = MakeImage(format, size, size);
    UseTarget(target);
    return target;
}

void Gpu::UseTarget(Image& target) {
    current_target = &target;
    read_image = nullptr;
    dk::ImageView view{target.image};
    cmd.bindRenderTargets(&view);
    RecordDefaultState();
}

Image& Gpu::BeginDepth(DkImageFormat depth_format, DkImageFormat format) {
    Image& target = MakeImage(format, TargetSize, TargetSize);
    Image& depth = MakeImage(depth_format, TargetSize, TargetSize, 1, 1, DkImageType_2D, DkImageFlags_UsageRender);
    current_target = &target;
    current_depth = &depth;
    read_image = nullptr;
    dk::ImageView view{target.image};
    dk::ImageView depth_view{depth.image};
    cmd.bindRenderTargets(&view, &depth_view);
    RecordDefaultState();
    return target;
}

void Gpu::Shaders(const char* vertex, const char* fragment) {
    cmd.bindShaders(DkStageFlag_GraphicsMask, {Shader(vertex), Shader(fragment)});
}

void Gpu::ClearColor(const Color& color, uint32_t target) {
    cmd.clearColor(target, DkColorMask_RGBA, static_cast<float>(color.r), static_cast<float>(color.g),
                   static_cast<float>(color.b), static_cast<float>(color.a));
}

void Gpu::BindVertices(const Vertex* vertices, uint32_t count) {
    Slice slice = Alloc(count * sizeof(Vertex));
    std::memcpy(slice.cpu, vertices, slice.size);
    cmd.bindVtxBuffer(0, slice.gpu, slice.size);
}

float Gpu::NdcX(float x) const {
    return 2.0f * x / current_target->width - 1.0f;
}

float Gpu::NdcY(float y) const {
    return 1.0f - 2.0f * y / current_target->height;
}

void Gpu::Rect(float x0, float y0, float x1, float y1, const Color& color, float z) {
    const float r = static_cast<float>(color.r), g = static_cast<float>(color.g);
    const float b = static_cast<float>(color.b), a = static_cast<float>(color.a);
    const float left = NdcX(x0), top = NdcY(y0), right = NdcX(x1), bottom = NdcY(y1);
    const Vertex vertices[4] = {
        {{left, top, z}, {r, g, b, a}, {0, 0}},
        {{left, bottom, z}, {r, g, b, a}, {0, 1}},
        {{right, top, z}, {r, g, b, a}, {1, 0}},
        {{right, bottom, z}, {r, g, b, a}, {1, 1}},
    };
    BindVertices(vertices, 4);
    cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

void Gpu::Full(const Color& color, float z) {
    Rect(0, 0, current_target->width, current_target->height, color, z);
}

void Gpu::Uniform(DkStage stage, uint32_t id, const void* data, uint32_t size) {
    Slice slice = Alloc(size, DK_UNIFORM_BUF_ALIGNMENT);
    std::memcpy(slice.cpu, data, size);
    cmd.bindUniformBuffer(stage, id, slice.gpu, size);
}

uint32_t Gpu::SetImage(const dk::ImageView& view, bool load_store) {
    dk::ImageDescriptor descriptor;
    descriptor.initialize(view, load_store);
    const uint32_t id = next_image++;
    cmd.pushData(image_descriptors.getGpuAddr() + id * sizeof(DkImageDescriptor), &descriptor, sizeof(descriptor));
    cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    return id;
}

uint32_t Gpu::SetSampler(const dk::Sampler& sampler) {
    dk::SamplerDescriptor descriptor;
    descriptor.initialize(sampler);
    const uint32_t id = next_sampler++;
    cmd.pushData(sampler_descriptors.getGpuAddr() + id * sizeof(DkSamplerDescriptor), &descriptor, sizeof(descriptor));
    cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    return id;
}

DkResHandle Gpu::Texture(const Image& image, const dk::Sampler& sampler) {
    dk::ImageView view{image.image};
    return Texture(view, sampler);
}

DkResHandle Gpu::Texture(const dk::ImageView& view, const dk::Sampler& sampler) {
    const uint32_t image_id = SetImage(view);
    return dkMakeTextureHandle(image_id, SetSampler(sampler));
}

DkResHandle Gpu::TextureAs(const Image& image, const dk::Sampler& sampler, uint32_t tic_word0) {
    dk::ImageView view{image.image};
    dk::ImageDescriptor descriptor;
    descriptor.initialize(view, false);
    std::memcpy(static_cast<void*>(&descriptor), &tic_word0, sizeof(tic_word0));
    const uint32_t id = next_image++;
    cmd.pushData(image_descriptors.getGpuAddr() + id * sizeof(DkImageDescriptor), &descriptor, sizeof(descriptor));
    cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    return dkMakeTextureHandle(id, SetSampler(sampler));
}

void Gpu::Submit() {
    queue.submitCommands(cmd.finishList());
    queue.waitIdle();
    cmd.clear();
}

void Gpu::Finish(const Image* target) {
    read_image = target ? target : current_target;
    const FormatInfo& info = InfoOf(read_image->format);
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    dk::ImageView view{read_image->image};
    cmd.copyImageToBuffer(view, {0, 0, 0, read_image->width, read_image->height, 1}, {readback.getGpuAddr(), 0, 0});
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
    Submit();

    char path[128];
    snprintf(path, sizeof(path), "sdmc:/renderer_suite/%s.bin", scene.c_str());
    if (FILE* file = fopen(path, "wb")) {
        const uint32_t header[4] = {read_image->width, read_image->height, info.bytes,
                                    (static_cast<uint32_t>(info.kind) << 8) | info.bits};
        fwrite(header, sizeof(header), 1, file);
        fwrite(readback.getCpuAddr(), info.bytes * read_image->width * read_image->height, 1, file);
        fclose(file);
    }
}

const uint8_t* Gpu::ReadRaw(uint32_t x, uint32_t y) const {
    const FormatInfo& info = InfoOf(read_image->format);
    return static_cast<const uint8_t*>(readback.getCpuAddr()) + (y * read_image->width + x) * info.bytes;
}

Color Gpu::Read(uint32_t x, uint32_t y) const {
    const FormatInfo& info = InfoOf(read_image->format);
    const uint8_t* data = ReadRaw(x, y);
    const uint32_t step = info.bits / 8;
    double channels[4] = {0, 0, 0, 1};
    for (uint32_t i = 0; i < info.channels; ++i) {
        channels[i] = DecodeChannel(data + i * step, info);
    }
    return {channels[0], channels[1], channels[2], channels[3]};
}

void Gpu::Expect(uint32_t x, uint32_t y, const Color& color, double tolerance) {
    const Color got = Read(x, y);
    const bool same = std::fabs(got.r - color.r) <= tolerance && std::fabs(got.g - color.g) <= tolerance &&
                      std::fabs(got.b - color.b) <= tolerance && std::fabs(got.a - color.a) <= tolerance;
    if (!same) {
        Fail("pixel %u,%u is %.3f %.3f %.3f %.3f, must be %.3f %.3f %.3f %.3f", x, y, got.r, got.g, got.b, got.a,
             color.r, color.g, color.b, color.a);
    }
}

void Gpu::ExpectUint(uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    Expect(x, y, {static_cast<double>(r), static_cast<double>(g), static_cast<double>(b), static_cast<double>(a)}, 0.0);
}

void Gpu::Check(const char* what, double got, double expected, double tolerance) {
    if (!(std::fabs(got - expected) <= tolerance)) {
        Fail("%s is %.6f, must be %.6f", what, got, expected);
    }
}

void Gpu::Fail(const char* format, ...) {
    char line[384];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ++failures;
    if (failures <= 8) Report("PROBE %s %s", scene.c_str(), line);
}

void Gpu::BeginScene(const char* name) {
    scene = name;
    failures = 0;
    arena_used = 0;
    next_image = 0;
    next_sampler = 0;
    images.clear();
    for (CMemPool::Handle& memory : image_memory) {
        retired_bytes += memory.getSize();
        retired_memory.push_back(memory);
    }
    image_memory.clear();
    if (retired_bytes > RetiredImageLimit) {
        for (CMemPool::Handle& memory : retired_memory) memory.destroy();
        retired_memory.clear();
        retired_bytes = 0;
    }
    current_target = nullptr;
    current_depth = nullptr;
    read_image = nullptr;
    Report("START %s", name);
}

bool Gpu::EndScene() {
    Report("END %s %s", scene.c_str(), failures == 0 ? "pass" : "fail");
    return failures == 0;
}
