// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The guest side of the renderer suite (tools/xbox/tests/renderer-suite): a homebrew built on the
// deko3d samples' framework, so each scene goes through everything a game goes through: the Maxwell
// engines, the shader recompiler, the caches and D3D12.
//
// A scene draws into an offscreen target, reads it back through guest memory and states what it must
// contain. What the scene states comes from the GPU's documented behaviour, not from a previous
// run, so a reference picture can never bless a bug. suite.py also keeps each picture to notice
// changes no probe looks at.
#pragma once

#include "SampleFramework/CMemPool.h"
#include "SampleFramework/CShader.h"

#include <array>
#include <cstdarg>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// A vertex of the scenes' common format, as basic_vsh reads it.
struct Vertex {
    float pos[3];
    float color[4];
    float uv[2];
};

struct Color {
    double r, g, b, a;
};

constexpr Color Black{0, 0, 0, 1};
constexpr Color White{1, 1, 1, 1};
constexpr Color Red{1, 0, 0, 1};
constexpr Color Green{0, 1, 0, 1};
constexpr Color Blue{0, 0, 1, 1};

/// How the bytes of a format read: what the probes and the host's comparison need.
enum class Kind : uint8_t { Unorm, Snorm, Uint, Sint, Float, Raw };

struct FormatInfo {
    DkImageFormat format;
    const char* name;
    uint8_t bytes;    ///< Per pixel.
    uint8_t channels; ///< 0 for packed formats, which only the sampled round trip probes.
    uint8_t bits;     ///< Per channel.
    Kind kind;
};

const FormatInfo& InfoOf(DkImageFormat format);

struct Image {
    dk::Image image;
    DkMemBlock block;
    uint32_t offset;
    DkImageFormat format;
    uint32_t width, height, depth, mips;
};

struct Slice {
    void* cpu;
    DkGpuAddr gpu;
    uint32_t size;
};

constexpr uint32_t TargetSize = 32;

class Gpu {
public:
    explicit Gpu(uint32_t device_flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft);
    ~Gpu();

    dk::UniqueDevice device;
    dk::UniqueQueue queue;
    dk::UniqueCmdBuf cmd;
    /// The depth buffer BeginDepth bound, for scenes that sample it.
    Image* current_depth = nullptr;

    // Scene lifetime: everything allocated goes when the next scene begins.
    Slice Alloc(uint32_t size, uint32_t align = 16);
    Image& MakeImage(DkImageFormat format, uint32_t width, uint32_t height, uint32_t depth = 1,
                     uint32_t mips = 1, DkImageType type = DkImageType_2D,
                     uint32_t flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine,
                     DkMsMode ms = DkMsMode_1x, uint32_t pitch_stride = 0);
    /// A 2D target of `format` over the memory of `base`, as a game reusing memory gets.
    Image& Alias(const Image& base, DkImageFormat format);
    /// Copies texels into a level and layer of an image.
    void Upload(const Image& image, const void* data, uint32_t size, uint32_t w, uint32_t h,
                uint32_t level = 0, uint32_t layer = 0, uint32_t depth = 1);
    /// `name`.dksh from the romfs, kept for the whole run.
    const dk::Shader* Shader(const char* name);

    /// A new target with the default state: everything off, the whole target as viewport and
    /// scissor, basic_vsh + color_fsh, the common vertex format.
    Image& Begin(DkImageFormat format = DkImageFormat_RGBA8_Unorm, uint32_t size = TargetSize);
    /// The same with a depth buffer bound too.
    Image& BeginDepth(DkImageFormat depth_format, DkImageFormat format = DkImageFormat_RGBA8_Unorm);
    /// Makes `target` current and binds it, for scenes drawing into several.
    void UseTarget(Image& target);
    /// Puts the default state back on the current target, keeping what it has drawn.
    void DefaultState();
    /// What was drawn so far becomes visible to textures, shaders and copies.
    void Barrier();
    void Shaders(const char* vertex, const char* fragment);
    void ClearColor(const Color& color, uint32_t target = 0);

    /// Vertices into scene memory and bound as buffer 0.
    void BindVertices(const Vertex* vertices, uint32_t count);
    uint32_t Width() const;
    uint32_t Height() const;
    /// A rectangle in pixels of the current target; uv goes 0..1 over it.
    void Rect(float x0, float y0, float x1, float y1, const Color& color, float z = 0.0f);
    /// The whole target.
    void Full(const Color& color, float z = 0.0f);
    /// Clip-space position of a target pixel.
    float NdcX(float x) const;
    float NdcY(float y) const;

    void Uniform(DkStage stage, uint32_t id, const void* data, uint32_t size);
    uint32_t SetImage(const dk::ImageView& view, bool load_store = false);
    uint32_t SetSampler(const dk::Sampler& sampler);
    /// A texture of `image` with `sampler`, ready for bindTextures.
    DkResHandle Texture(const Image& image, const dk::Sampler& sampler);
    /// Sampled through `view`, whose format or type may differ from the image's own.
    DkResHandle Texture(const dk::ImageView& view, const dk::Sampler& sampler);
    /// The same, with the first word of the texture descriptor replaced: for the formats the guest
    /// has and deko3d does not name.
    DkResHandle TextureAs(const Image& image, const dk::Sampler& sampler, uint32_t tic_word0);

    /// Runs what is recorded and reads the target (`target` when given) back to guest memory.
    void Finish(const Image* target = nullptr);
    /// Runs what is recorded, for scenes that read their own memory.
    void Submit();
    /// A pixel of the last Finish, with the format's own decoding.
    Color Read(uint32_t x, uint32_t y) const;
    const uint8_t* ReadRaw(uint32_t x, uint32_t y) const;
    /// Says whether a read pixel is what it must be. `tolerance` over 0..1 values.
    void Expect(uint32_t x, uint32_t y, const Color& color, double tolerance = 2.5 / 255.0);
    void ExpectUint(uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
    /// Says whether a value computed in the guest is what it must be.
    void Check(const char* what, double got, double expected, double tolerance = 0.0);
    void Fail(const char* format, ...) __attribute__((format(printf, 2, 3)));
    /// Rewrites `count` consecutive words of what is recorded, for GPU state deko3d has no call for.
    /// False if they are not there.
    bool PatchCommands(const uint32_t* from, const uint32_t* to, uint32_t count);

    // Called by the runner.
    void BeginScene(const char* name);
    bool EndScene();

private:
    void RecordDefaultState();

    std::optional<CMemPool> pool_images;
    std::optional<CMemPool> pool_code;
    std::optional<CMemPool> pool_data;
    CMemPool::Handle arena;
    uint32_t arena_used = 0;
    CMemPool::Handle cmd_memory;
    CMemPool::Handle image_descriptors;
    CMemPool::Handle sampler_descriptors;
    CMemPool::Handle readback;
    uint32_t next_image = 0;
    uint32_t next_sampler = 0;

    std::deque<Image> images;
    std::vector<CMemPool::Handle> image_memory;
    /// Memory of earlier scenes' images, held so new images get new addresses.
    std::vector<CMemPool::Handle> retired_memory;
    uint32_t retired_bytes = 0;
    std::map<std::string, std::unique_ptr<CShader>> shaders;

    Image* current_target = nullptr;
    const Image* read_image = nullptr;
    std::string scene;
    int failures = 0;
};

using SceneFn = std::function<void(Gpu&)>;

struct SceneEntry {
    std::string name;
    SceneFn run;
};

/// What each scenes_*.cpp adds to the run.
void RegisterClears(std::vector<SceneEntry>& scenes);
void RegisterBlend(std::vector<SceneEntry>& scenes);
void RegisterDepthStencil(std::vector<SceneEntry>& scenes);
void RegisterRaster(std::vector<SceneEntry>& scenes);
void RegisterGeometry(std::vector<SceneEntry>& scenes);
void RegisterTextures(std::vector<SceneEntry>& scenes);
void RegisterShaders(std::vector<SceneEntry>& scenes);
void RegisterTransfers(std::vector<SceneEntry>& scenes);

/// Scenes on the OpenGL conventions (lower-left origin, depth -1..1), which need their own device.
void RegisterGlConventions(std::vector<SceneEntry>& scenes);

/// The draw-heavy loop of the performance layer, on the swapchain.
void RunPerformanceLoop(uint32_t frames);

/// The tessellation loop: draws of patches of quads at a fixed level, or (flat) the same triangles
/// drawn without tessellation, so the two runs differ only by what tessellating costs.
struct TessPerformance {
    uint32_t draws;
    uint32_t patches;
    uint32_t level;
    bool flat;
};
void RunTessPerformanceLoop(uint32_t frames, const TessPerformance& config);

/// One line for the host, in the results file and in the emulator's log.
void Report(const char* format, ...) __attribute__((format(printf, 1, 2)));
