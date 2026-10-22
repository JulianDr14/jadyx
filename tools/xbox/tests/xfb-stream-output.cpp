// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Transform feedback through D3D12 stream output: a test bench of 33 cases (sanity, functional,
// integration, regression, performance and exploratory) on the production pieces
// (shader recompiler, MakeTransformFeedbackVaryings, spirv_to_dxil, RootSignatureCache,
// StreamOutput, Buffer) and a real D3D12 device with the debug layer.
//
// The expected data never comes from the implementation. It comes from a CPU model of what the
// specifications require of a capture (Vulkan VK_EXT_transform_feedback and the drawing chapter,
// the D3D11/D3D12 stream output rules, Maxwell's register semantics as the Vulkan backend reads
// them):
//  - vertices are written in primitive order; strips and fans are split into separate primitives,
//    each keeping its orientation (an odd strip triangle may start at any of its vertices, the
//    APIs differ there, but its winding is fixed)
//  - a primitive that does not fit in every bound buffer is written to none of them
//  - words of a buffer no varying covers (holes) are left as they were
//  - a captured output the shader never writes is undefined, so it is not checked
//  - the position captured is the one the shader wrote, before any host transform
//  - capture appends while the registers stay the same; it restarts at start_offset when they
//    change or capture is turned off and on
//
// Usage: xfb-stream-output.exe [category] [--seed N]

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12sdklayers.h>
#include <eden_spirv_to_dxil.h>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_stream_output.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"
#include "video_core/transform_feedback.h"

using namespace D3D12;
namespace IR = Shader::IR;
using Maxwell3D = Tegra::Engines::Maxwell3D;
using Maxwell = Maxwell3D::Regs;

namespace {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            throw Failure(fmt::format("line {}: {}: {}", __LINE__, #cond,                          \
                                      fmt::format(__VA_ARGS__)));                                  \
        }                                                                                          \
    } while (0)

/// Reads a byte count once (reading it finishes the GPU work) and reports the value read.
#define CHECK_BYTE_COUNT(bench, stream, expected)                                                  \
    do {                                                                                           \
        const std::optional<u32> got_count = (bench).ByteCount(stream);                            \
        const u64 want_count = (expected);                                                         \
        CHECK(got_count == want_count, "stream {} byte count {} expected {}", stream,              \
              got_count ? fmt::to_string(*got_count) : std::string("none"), want_count);          \
    } while (0)

constexpr u32 SENTINEL = 0xCDCDCDCDu;

// Maxwell stream output attribute ids (IR::Attribute values).
constexpr u32 HOLE = 0; // not an output: the word is a gap
constexpr u32 LAYER = 25;
constexpr u32 VIEWPORT = 26;
constexpr u32 POINT_SIZE = 27;
constexpr u32 Pos(u32 c) {
    return 28 + c;
}
constexpr u32 Gen(u32 g, u32 c) {
    return 32 + g * 4 + c;
}
constexpr u32 Clip(u32 c) {
    return 176 + c;
}

std::vector<u32> Vector(u32 base, u32 first, u32 count) {
    std::vector<u32> out;
    for (u32 c = first; c < first + count; ++c) {
        out.push_back(base + c);
    }
    return out;
}
std::vector<u32> Cat(std::initializer_list<std::vector<u32>> parts) {
    std::vector<u32> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Inputs and the shader model

struct Vertex {
    std::array<float, 4> pos;
    std::array<float, 4> in1;
};
static_assert(sizeof(Vertex) == 32);

/// Every vertex is distinct (in1 identifies it), with exact float values.
Vertex MakeVertex(u32 v, float w = 1.0f) {
    const float base = static_cast<float>(v * 4);
    return {{static_cast<float>(v % 8) * 0.125f - 0.5f, static_cast<float>(v % 4) * 0.25f - 0.375f,
             0.25f + static_cast<float>(v % 3) * 0.125f, w},
            {base + 1, base + 2, base + 3, base + 4}};
}
std::vector<Vertex> MakeVertices(u32 count, float w = 1.0f) {
    std::vector<Vertex> out(count);
    for (u32 v = 0; v < count; ++v) {
        out[v] = MakeVertex(v, w);
    }
    return out;
}

/// What the guest shaders do. The vertex shader writes position = input 0 and generic g =
/// input 1 + 10 (g + 1); the geometry shader (gs_copies > 0) emits each input triangle
/// gs_copies times, adding 100 (k + 1) to generic 0 in copy k.
struct ShaderSpec {
    u32 vs_generics = 1;  ///< bit g: the vertex shader writes generic g
    u32 vs_partial = 0;   ///< bit g: only its x and y
    bool vs_point_size = false;
    u32 vs_clip_distances = 0;
    u32 gs_copies = 0;
    bool gs_layer = false; ///< the geometry shader writes Layer 5 and ViewportIndex 3
    u32 fs_loads = 0;
    u32 fs_noperspective = 0;
    bool convert_depth = false;
    bool storage = false; ///< the storage buffer fallback instead of stream output
};

struct Expect {
    enum Kind : u8 { Hole, Undefined, Value } kind;
    u32 bits;
};

Expect ValueOf(float value) {
    return {Expect::Value, std::bit_cast<u32>(value)};
}

/// The word the specifications require for attribute attr of vertex in, in GS copy k.
Expect Model(const ShaderSpec& spec, u32 attr, const Vertex& in, u32 k) {
    const bool gs = spec.gs_copies != 0;
    if (attr >= Pos(0) && attr <= Pos(3)) {
        return ValueOf(in.pos[attr - Pos(0)]);
    }
    if (attr >= Gen(0, 0) && attr <= Gen(31, 3)) {
        const u32 g = (attr - Gen(0, 0)) / 4;
        const u32 c = attr % 4;
        if (((spec.vs_generics >> g) & 1) == 0 || (((spec.vs_partial >> g) & 1) != 0 && c >= 2)) {
            return {Expect::Undefined, 0};
        }
        float value = in.in1[c] + 10.0f * static_cast<float>(g + 1);
        if (gs && g == 0) {
            value += 100.0f * static_cast<float>(k + 1);
        }
        return ValueOf(value);
    }
    if (attr == POINT_SIZE) {
        return spec.vs_point_size && !gs ? ValueOf(3.0f) : Expect{Expect::Undefined, 0};
    }
    if (attr == LAYER || attr == VIEWPORT) {
        // Integer outputs: captured as their 32-bit pattern.
        return spec.gs_layer && gs ? Expect{Expect::Value, attr == LAYER ? 5u : 3u}
                                   : Expect{Expect::Undefined, 0};
    }
    if (attr >= Clip(0) && attr <= Clip(7)) {
        const u32 c = attr - Clip(0);
        return c < spec.vs_clip_distances && !gs ? ValueOf(in.in1[0] + static_cast<float>(c))
                                                 : Expect{Expect::Undefined, 0};
    }
    if (attr < 24) {
        return {Expect::Hole, 0};
    }
    return {Expect::Undefined, 0};
}

// ---------------------------------------------------------------------------------------------
// Primitive assembly as the drawing chapter defines it

struct Primitive {
    std::vector<u32> v; ///< vertex indices, in the order of the even/first-vertex convention
    bool rotatable{};   ///< an odd strip triangle: any rotation keeps its winding
};

constexpr u32 RESTART = ~0u;

std::vector<Primitive> Assemble(D3D_PRIMITIVE_TOPOLOGY topology, const std::vector<u32>& order) {
    std::vector<Primitive> out;
    std::vector<u32> run;
    const auto flush = [&] {
        switch (topology) {
        case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
            for (u32 v : run) out.push_back({{v}});
            break;
        case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
            for (size_t i = 0; i + 1 < run.size(); i += 2) out.push_back({{run[i], run[i + 1]}});
            break;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
            for (size_t i = 0; i + 1 < run.size(); ++i) out.push_back({{run[i], run[i + 1]}});
            break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
            for (size_t i = 0; i + 2 < run.size(); i += 3) {
                out.push_back({{run[i], run[i + 1], run[i + 2]}});
            }
            break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
            for (size_t i = 0; i + 2 < run.size(); ++i) {
                if (i % 2 == 0) {
                    out.push_back({{run[i], run[i + 1], run[i + 2]}});
                } else {
                    out.push_back({{run[i + 1], run[i], run[i + 2]}, true});
                }
            }
            break;
        default:
            throw Failure("unsupported topology in the model");
        }
        run.clear();
    };
    for (u32 v : order) {
        if (v == RESTART) {
            flush();
        } else {
            run.push_back(v);
        }
    }
    flush();
    return out;
}

std::vector<u32> Iota(u32 count) {
    std::vector<u32> out(count);
    for (u32 i = 0; i < count; ++i) out[i] = i;
    return out;
}

// ---------------------------------------------------------------------------------------------
// The bench

struct BufferLayout {
    u32 stream = 0;
    std::vector<u32> attrs; ///< one attribute id per 32-bit word
    u32 Stride() const {
        return static_cast<u32>(attrs.size() * 4);
    }
};
using Layouts = std::array<BufferLayout, 4>;

struct PipelineConfig {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    bool rasterize = false;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    bool capture = true;
    bool flatten = false;
    std::optional<u32> force_lower_mask; ///< replaces the production NoPerspective rule
};

struct Pipeline {
    ComPtr<ID3D12PipelineState> pso;
    const PipelineLayout* layout{};
    std::unique_ptr<eden_spirv_to_dxil_stream_output> so;
    std::vector<D3D12_SO_DECLARATION_ENTRY> declaration;
    std::array<UINT, 4> strides{};
    bool native{};
    bool rasterize{};
    double compile_ms{};
};

struct Target {
    std::unique_ptr<Buffer> buffer;
    GPUVAddr address{};
    u32 size{};         ///< regs size: bytes from start_offset
    u32 start_offset{};
    bool enable{true};
};

struct DrawCall {
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<Vertex> vertices;
    std::vector<u32> indices; ///< empty: not indexed
    bool index32 = false;
    u32 yz_flip_mask = 0;
};

u32 g_assert_failures = 0;
void CountAssert(const char*) noexcept {
    ++g_assert_failures;
}

class Bench {
public:
    Bench()
        : scheduler{device}, staging{device, scheduler}, transfer{device, scheduler},
          runtime{device, scheduler, staging, transfer, FakeDeviceMemory()},
          root_signatures{device}, stream_output{device, scheduler, compiler},
          engine{std::make_unique<Maxwell3D>(FakeMemoryManager())} {
        ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "info queue");
        profile = Shader::Profile{
            .supported_spirv = 0x00010300,
            .unified_descriptor_binding = true,
            .descriptor_arrays_use_count = true,
            .support_descriptor_aliasing = false,
            .support_float_controls = true,
            .support_fp32_denorm_flush = true,
            .force_fp32_denorm_flush = true,
            .support_demote_to_helper_invocation = true,
            .support_derivative_control = true,
            .support_geometry_shader_passthrough = false,
            .support_native_ndc = false,
            .support_multi_viewport = true,
            .support_geometry_streams = false,
            .need_declared_frag_colors = true,
            .has_broken_loop_safety_counter = true,
            .min_ssbo_alignment = 16,
            .max_user_clip_distances = 8,
        };
        const D3D12_DESCRIPTOR_HEAP_DESC heap{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                              .NumDescriptors = 1};
        ThrowIfFailed(device.Get()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap)), "rtv");
        const D3D12_HEAP_PROPERTIES props{.Type = D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                       .Width = RT_SIZE, .Height = RT_SIZE, .DepthOrArraySize = 1,
                                       .MipLevels = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                                       .SampleDesc = {1, 0},
                                       .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        const D3D12_CLEAR_VALUE clear{.Format = DXGI_FORMAT_R32G32B32A32_FLOAT};
        ThrowIfFailed(device.Get()->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                            &clear, IID_PPV_ARGS(&render_target)),
                      "render target");
        device.Get()->CreateRenderTargetView(render_target.Get(), nullptr,
                                             rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    Device device;
    Scheduler scheduler;
    ShaderCompiler compiler;
    StagingBufferPool staging;
    TransferBufferPool transfer;
    BufferCacheRuntime runtime;
    RootSignatureCache root_signatures;
    StreamOutput stream_output;
    std::unique_ptr<Maxwell3D> engine;
    Shader::Profile profile;
    ComPtr<ID3D12InfoQueue> messages;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12Resource> render_target;
    std::vector<ComPtr<ID3D12Resource>> keep;
    std::array<Target, 4> targets;
    std::array<BufferCacheRuntime::TransformFeedbackBinding, 4> bindings{};
    Layouts layouts;
    u64 next_cpu_addr = 0x1000'0000;
    static constexpr u32 RT_SIZE = 16;

    Maxwell& Regs() {
        return engine->regs;
    }

    /// Between tests: capture off, every buffer unbound, the GPU idle.
    void Reset() {
        scheduler.Finish();
        keep.clear();
        targets = {};
        bindings = {};
        layouts = {};
        std::memset(&Regs().transform_feedback, 0, sizeof(Regs().transform_feedback));
        Regs().transform_feedback_enabled = 0;
        stream_output.Disable();
    }

    std::unique_ptr<Buffer> NewBuffer(u64 size) {
        auto buffer = std::make_unique<Buffer>(runtime, next_cpu_addr, size);
        next_cpu_addr += Common::AlignUp(size, 0x1000) + 0x1000;
        return buffer;
    }

    ID3D12Resource* UploadBytes(const void* data, u64 size) {
        auto upload = CreateCommittedBuffer(device.Get(), std::max<u64>(size, 4),
                                            D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            D3D12_RESOURCE_FLAG_NONE, "test upload");
        void* mapped{};
        const D3D12_RANGE none{};
        ThrowIfFailed(upload->Map(0, &none, &mapped), "map upload");
        std::memcpy(mapped, data, size);
        upload->Unmap(0, nullptr);
        keep.push_back(upload);
        return upload.Get();
    }

    void Fill(Buffer& buffer, u32 word) {
        const std::vector<u32> words(buffer.SizeBytes() / 4, word);
        ID3D12Resource* const upload = UploadBytes(words.data(), words.size() * 4);
        buffer.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        scheduler.CommandList()->CopyBufferRegion(buffer.Handle(), 0, upload, 0,
                                                  words.size() * 4);
    }

    std::vector<u32> ReadResource(ID3D12Resource* resource, u64 offset, u64 size) {
        auto readback = CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_READBACK,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              D3D12_RESOURCE_FLAG_NONE, "test readback");
        scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, resource, offset, size);
        scheduler.Finish();
        keep.clear();
        std::vector<u32> out(size / 4);
        void* mapped{};
        const D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
        ThrowIfFailed(readback->Map(0, &range, &mapped), "map readback");
        std::memcpy(out.data(), mapped, size);
        const D3D12_RANGE none{};
        readback->Unmap(0, &none);
        return out;
    }

    std::vector<u32> Read(Buffer& buffer) {
        buffer.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE, false);
        return ReadResource(buffer.Handle(), 0, buffer.SizeBytes());
    }

    /// What a StreamingByteCount report of stream would write (RasterizerD3D12::QueryByteCount).
    std::optional<u32> ByteCount(u32 stream) {
        const std::optional<u32> slot = stream_output.ByteCountSlot(Regs(), stream);
        if (!slot) {
            return std::nullopt;
        }
        auto dst = NewBuffer(16);
        dst->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        stream_output.CopyByteCount(scheduler.CommandList(), *slot, dst->Handle(), 0);
        return Read(*dst)[0];
    }

    /// Sets the transform feedback registers for the layouts and targets, and the bindings the
    /// buffer cache makes from them (BufferCache::BindHostTransformFeedbackBuffers, then
    /// BufferCacheRuntime::BindTransformFeedbackBuffers).
    void SetTargets() {
        auto& tf = Regs().transform_feedback;
        Regs().transform_feedback_enabled = 1;
        for (u32 b = 0; b < 4; ++b) {
            const Target& target = targets[b];
            auto& regs = tf.buffers[b];
            regs.enable = target.buffer && target.enable ? 1 : 0;
            regs.address_high = static_cast<u32>(target.address >> 32);
            regs.address_low = static_cast<u32>(target.address);
            regs.size = static_cast<s32>(target.size);
            regs.start_offset = static_cast<s32>(target.start_offset);
            tf.controls[b].stream = layouts[b].stream;
            tf.controls[b].varying_count = static_cast<u32>(layouts[b].attrs.size());
            tf.controls[b].stride = layouts[b].Stride();
            bindings[b] = {};
            const bool has_layout = tf.controls[b].varying_count != 0 || tf.controls[b].stride != 0;
            if (!has_layout || regs.enable == 0 || target.size == 0) {
                continue;
            }
            const u64 offset = target.start_offset;
            if (offset % 4 != 0 || offset >= target.buffer->SizeBytes()) {
                continue;
            }
            bindings[b] = {
                .buffer = target.buffer.get(),
                .address = target.buffer->Address() + offset,
                .size = static_cast<u32>(
                    std::min<u64>(target.size, target.buffer->SizeBytes() - offset)),
            };
        }
    }

    /// A target for buffer b: buffer_bytes of guest memory at a fresh address, filled with the
    /// sentinel, capturing size bytes from start_offset.
    void AddTarget(u32 b, u32 buffer_bytes, u32 start_offset = 0, std::optional<u32> size = {}) {
        Target& target = targets[b];
        target.buffer = NewBuffer(buffer_bytes);
        target.address = 0x2'0000'0000ull + b * 0x100'0000ull + (next_cpu_addr & 0xFF'F000);
        target.start_offset = start_offset;
        target.size = size.value_or(buffer_bytes - start_offset);
        Fill(*target.buffer, SENTINEL);
    }

    Pipeline Build(const ShaderSpec& spec, const Layouts& layout_set,
                   const PipelineConfig& config = {});
    void Draw(const Pipeline& pipeline, const DrawCall& call);
    void SetRuntimeData(CommandStream* cmd, const Pipeline& pipeline, u32 yz_flip_mask);
    void MessagesCheck(const std::string& where);

private:
    static Tegra::MaxwellDeviceMemoryManager& FakeDeviceMemory() {
        // BufferCacheRuntime only keeps the reference; nothing here reaches guest memory.
        alignas(std::max_align_t) static std::byte storage[4096]{};
        return *reinterpret_cast<Tegra::MaxwellDeviceMemoryManager*>(storage);
    }
    static Tegra::MemoryManager& FakeMemoryManager() {
        alignas(std::max_align_t) static std::byte storage[4096]{};
        return *reinterpret_cast<Tegra::MemoryManager*>(storage);
    }
};

void Bench::MessagesCheck(const std::string& where) {
    std::string errors;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<std::byte> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        ThrowIfFailed(messages->GetMessage(i, message, &size), "message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            errors += fmt::format("\n    [{}] {}", static_cast<int>(message->Severity),
                                  message->pDescription);
        }
    }
    messages->ClearStoredMessages();
    const HRESULT removed = device.Get()->GetDeviceRemovedReason();
    if (removed != S_OK) {
        throw Failure(fmt::format("{}: device removed 0x{:08X}", where, static_cast<u32>(removed)));
    }
    if (!errors.empty()) {
        throw Failure(fmt::format("{}: debug layer:{}", where, errors));
    }
}

// --- shaders ---------------------------------------------------------------------------------

struct ProgramBuilder {
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;

    IR::Block* Begin(IR::Program& p, Shader::Stage stage) {
        p.stage = stage;
        auto* block = block_pool.Create(inst_pool);
        p.blocks.push_back(block);
        p.post_order_blocks.push_back(block);
        IR::AbstractSyntaxNode node;
        node.type = IR::AbstractSyntaxNode::Type::Block;
        node.data.block = block;
        p.syntax_list.push_back(node);
        p.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        return block;
    }

    static void CollectInfo(IR::Program& p) {
        for (const auto& inst : p.blocks.front()->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetAttribute:
                p.info.loads.Set(inst.Arg(0).Attribute());
                break;
            case IR::Opcode::SetAttribute:
                p.info.stores.Set(inst.Arg(0).Attribute());
                break;
            case IR::Opcode::SetFragColor:
                p.info.stores_frag_color[inst.Arg(0).U32()] = true;
                break;
            default:
                break;
            }
        }
    }
};

IR::Attribute Attr(u32 id) {
    return static_cast<IR::Attribute>(id);
}

void BuildVertex(ProgramBuilder& builder, IR::Program& p, const ShaderSpec& spec) {
    IR::IREmitter ir{*builder.Begin(p, Shader::Stage::VertexB)};
    ir.Prologue();
    for (u32 c = 0; c < 4; ++c) {
        ir.SetAttribute(Attr(Pos(c)), ir.GetAttribute(Attr(Gen(0, c))), ir.Imm32(0u));
    }
    for (u32 g = 0; g < 32; ++g) {
        if (((spec.vs_generics >> g) & 1) == 0) continue;
        const u32 components = ((spec.vs_partial >> g) & 1) != 0 ? 2 : 4;
        for (u32 c = 0; c < components; ++c) {
            const IR::F32 value{ir.FPAdd(ir.GetAttribute(Attr(Gen(1, c))),
                                         ir.Imm32(10.0f * static_cast<float>(g + 1)))};
            ir.SetAttribute(Attr(Gen(g, c)), value, ir.Imm32(0u));
        }
    }
    if (spec.vs_point_size) {
        ir.SetAttribute(Attr(POINT_SIZE), ir.Imm32(3.0f), ir.Imm32(0u));
    }
    for (u32 c = 0; c < spec.vs_clip_distances; ++c) {
        const IR::F32 value{ir.FPAdd(ir.GetAttribute(Attr(Gen(1, 0))),
                                     ir.Imm32(static_cast<float>(c)))};
        ir.SetAttribute(Attr(Clip(c)), value, ir.Imm32(0u));
    }
    ir.Epilogue();
    ProgramBuilder::CollectInfo(p);
}

void BuildGeometry(ProgramBuilder& builder, IR::Program& p, const ShaderSpec& spec) {
    IR::IREmitter ir{*builder.Begin(p, Shader::Stage::Geometry)};
    p.invocations = 1;
    p.output_vertices = 3 * spec.gs_copies;
    p.output_topology = Shader::OutputTopology::TriangleStrip;
    ir.Prologue();
    for (u32 k = 0; k < spec.gs_copies; ++k) {
        for (u32 j = 0; j < 3; ++j) {
            const IR::U32 vertex = ir.Imm32(j);
            for (u32 c = 0; c < 4; ++c) {
                ir.SetAttribute(Attr(Pos(c)), ir.GetAttribute(Attr(Pos(c)), vertex), ir.Imm32(0u));
            }
            for (u32 g = 0; g < 32; ++g) {
                if (((spec.vs_generics >> g) & 1) == 0) continue;
                const u32 components = ((spec.vs_partial >> g) & 1) != 0 ? 2 : 4;
                for (u32 c = 0; c < components; ++c) {
                    IR::F32 value = ir.GetAttribute(Attr(Gen(g, c)), vertex);
                    if (g == 0) {
                        value = IR::F32{ir.FPAdd(value,
                                                 ir.Imm32(100.0f * static_cast<float>(k + 1)))};
                    }
                    ir.SetAttribute(Attr(Gen(g, c)), value, ir.Imm32(0u));
                }
            }
            if (spec.gs_layer) {
                ir.SetAttribute(Attr(LAYER), ir.Imm32(std::bit_cast<float>(5u)), ir.Imm32(0u));
                ir.SetAttribute(Attr(VIEWPORT), ir.Imm32(std::bit_cast<float>(3u)), ir.Imm32(0u));
            }
            ir.EmitVertex(ir.Imm32(0u));
        }
        ir.EndPrimitive(ir.Imm32(0u));
    }
    ir.Epilogue();
    ProgramBuilder::CollectInfo(p);
}

void BuildFragment(ProgramBuilder& builder, IR::Program& p, const ShaderSpec& spec) {
    IR::IREmitter ir{*builder.Begin(p, Shader::Stage::Fragment)};
    ir.Prologue();
    const u32 first = spec.fs_loads != 0 ? static_cast<u32>(std::countr_zero(spec.fs_loads)) : 32;
    for (u32 c = 0; c < 4; ++c) {
        IR::F32 value = ir.Imm32(0.25f);
        for (u32 g = 0; g < 32; ++g) {
            if (((spec.fs_loads >> g) & 1) == 0) continue;
            const IR::F32 loaded = ir.GetAttribute(Attr(Gen(g, c)));
            value = g == first ? loaded : IR::F32{ir.FPAdd(value, IR::F32{ir.FPAdd(loaded, ir.Imm32(0.0f))})};
            if (g != first) {
                // Only the first loaded generic reaches the color; the others are read and
                // dropped, to keep the input declared.
                value = IR::F32{ir.FPAdd(value, IR::F32{ir.FPAdd(loaded, ir.FPNeg(loaded))})};
            }
        }
        ir.SetFragColor(0, c, value);
    }
    ir.Epilogue();
    ProgramBuilder::CollectInfo(p);
    for (u32 g = 0; g < 32; ++g) {
        if ((spec.fs_noperspective >> g) & 1) {
            p.info.interpolation[g] = Shader::Interpolation::NoPerspective;
        }
    }
}

/// The layout rule the Series driver needs (DefineTransformFeedbackCopies): every output stream
/// output reads is alone in its register, from component 0. Checked on the SPIR-V the last stage
/// declares and on the stream output declaration built from it.
void CheckCaptureLayout(std::span<const u32> spirv,
                        std::span<const D3D12_SO_DECLARATION_ENTRY> declaration) {
    static constexpr u32 OP_DECORATE = 71;
    static constexpr u32 OP_VARIABLE = 59;
    static constexpr u32 STORAGE_OUTPUT = 3;
    static constexpr u32 DECORATION_LOCATION = 30;
    static constexpr u32 DECORATION_COMPONENT = 31;
    static constexpr u32 DECORATION_XFB_BUFFER = 36;
    struct Output {
        bool output{};
        std::optional<u32> location;
        bool component{};
        bool captured{};
    };
    std::unordered_map<u32, Output> ids;
    for (size_t word = 5; word < spirv.size();) {
        const u32 count = spirv[word] >> 16;
        const u32 opcode = spirv[word] & 0xFFFF;
        CHECK(count != 0 && word + count <= spirv.size(), "malformed SPIR-V at word {}", word);
        if (opcode == OP_VARIABLE && count >= 4 && spirv[word + 3] == STORAGE_OUTPUT) {
            ids[spirv[word + 2]].output = true;
        } else if (opcode == OP_DECORATE && count >= 3) {
            Output& id = ids[spirv[word + 1]];
            switch (spirv[word + 2]) {
            case DECORATION_LOCATION:
                id.location = spirv[word + 3];
                break;
            case DECORATION_COMPONENT:
                id.component = true;
                break;
            case DECORATION_XFB_BUFFER:
                id.captured = true;
                break;
            }
        }
        word += count;
    }
    std::array<u32, 32> per_location{};
    for (const auto& [id, output] : ids) {
        if (output.output && output.location && *output.location < per_location.size()) {
            ++per_location[*output.location];
        }
    }
    for (const auto& [id, output] : ids) {
        if (!output.output || !output.captured || !output.location) continue;
        CHECK(!output.component, "captured output %{} at location {} takes a component",
              id, *output.location);
        CHECK(per_location[*output.location] == 1,
              "captured output %{} shares location {} with {} other output(s)", id,
              *output.location, per_location[*output.location] - 1);
    }
    std::array<u32, 32> per_register{};
    for (const D3D12_SO_DECLARATION_ENTRY& entry : declaration) {
        if (!entry.SemanticName || std::string_view{entry.SemanticName} != "TEXCOORD") continue;
        CHECK(entry.StartComponent == 0, "TEXCOORD{} captured from component {}",
              entry.SemanticIndex, entry.StartComponent);
        CHECK(entry.SemanticIndex < per_register.size() && ++per_register[entry.SemanticIndex] == 1,
              "TEXCOORD{} captured twice", entry.SemanticIndex);
    }
}

Pipeline Bench::Build(const ShaderSpec& spec, const Layouts& layout_set,
                      const PipelineConfig& config) {
    const auto start = std::chrono::steady_clock::now();
    // The key as the pipeline cache refreshes it, from the layout registers and the current
    // buffer enables (SetTargets).
    auto regs = std::make_unique<Maxwell>(Regs());
    for (u32 b = 0; b < 4; ++b) {
        const BufferLayout& layout = layout_set[b];
        auto& control = regs->transform_feedback.controls[b];
        control.stream = layout.stream;
        control.varying_count = static_cast<u32>(layout.attrs.size());
        control.stride = layout.Stride();
        regs->stream_out_layout[b] = {};
        for (size_t w = 0; w < layout.attrs.size(); ++w) {
            u32 raw{};
            std::memcpy(&raw, &regs->stream_out_layout[b][w / 4], sizeof(raw));
            raw |= (layout.attrs[w] & 0xFF) << ((w % 4) * 8);
            std::memcpy(&regs->stream_out_layout[b][w / 4], &raw, sizeof(raw));
        }
    }
    VideoCommon::TransformFeedbackState state{};
    state.Refresh(*regs);
    const auto xfb_varyings = VideoCommon::MakeTransformFeedbackVaryings(state);
    const bool native = config.capture && !spec.storage && compiler.SupportsStreamOutput();

    ProgramBuilder builder;
    IR::Program vs, gs, fs;
    BuildVertex(builder, vs, spec);
    const bool has_gs = spec.gs_copies != 0;
    if (has_gs) BuildGeometry(builder, gs, spec);
    BuildFragment(builder, fs, spec);
    IR::Program& last = has_gs ? gs : vs;

    // The production NoPerspective rule (PipelineCache::CreateGraphicsPipeline): only direct
    // VS -> FS pipelines, and never a varying stream output captures.
    u32 lower_mask = 0;
    if (!has_gs) {
        for (u32 g = 0; g < 32; ++g) {
            const bool captured = config.capture && xfb_varyings.CapturesGeneric(g);
            if (fs.info.interpolation[g] == Shader::Interpolation::NoPerspective &&
                fs.info.loads.Generic(g) && vs.info.stores.Generic(g) && !(native && captured)) {
                lower_mask |= 1u << g;
            }
        }
    }
    if (config.force_lower_mask) lower_mask = *config.force_lower_mask;

    const auto runtime_for = [&](IR::Program& program, const IR::Program* previous) {
        Shader::RuntimeInfo info{};
        if (previous) {
            info.previous_stage_stores = previous->info.stores;
        } else {
            info.previous_stage_stores.mask.set();
        }
        info.lower_noperspective_mask = lower_mask;
        if (program.stage == Shader::Stage::Geometry) {
            info.input_topology = Shader::InputTopology::Triangles;
        }
        if (&program == &last) {
            info.convert_depth_mode = spec.convert_depth;
            if (config.capture) {
                xfb_varyings.Apply(info);
                info.xfb_storage = !native;
                info.xfb_copy_outputs = native;
            }
        }
        return info;
    };
    Shader::Backend::Bindings binding;
    std::vector<u32> vs_spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_for(vs, nullptr),
                                                                   vs, binding);
    std::vector<u32> gs_spirv;
    if (has_gs) gs_spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_for(gs, &vs), gs, binding);
    std::vector<u32> fs_spirv =
        Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_for(fs, &last), fs, binding);

    std::array<const Shader::Info*, 5> infos{&vs.info, nullptr, nullptr,
                                             has_gs ? &gs.info : nullptr, &fs.info};
    Pipeline out;
    out.native = native;
    out.rasterize = config.rasterize;
    out.layout = &root_signatures.Get(infos, false, native);
    std::vector<ShaderCompiler::PipelineStage> stages{{vs_spirv, DXIL_SPIRV_SHADER_VERTEX}};
    if (has_gs) stages.push_back({gs_spirv, DXIL_SPIRV_SHADER_GEOMETRY});
    stages.push_back({fs_spirv, DXIL_SPIRV_SHADER_FRAGMENT});
    ShaderCompiler::PipelineOptions options{
        .yz_flip = DXIL_SPIRV_YZ_FLIP_CONDITIONAL,
        .first_vertex_and_base_instance = DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA,
        .flatten_vertex = config.flatten,
    };
    if (native) {
        out.so = std::make_unique<eden_spirv_to_dxil_stream_output>();
        options.stream_output = out.so.get();
    }
    const auto compiled = compiler.CompilePipeline(stages, options);
    CHECK(compiled.size() == stages.size(), "{} compiled stages", compiled.size());

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = out.layout->Handle();
    desc.VS = {compiled[0].dxil.data(), compiled[0].dxil.size()};
    if (has_gs) desc.GS = {compiled[1].dxil.data(), compiled[1].dxil.size()};
    const auto& ps = compiled.back();
    desc.PS = {ps.dxil.data(), ps.dxil.size()};
    if (native) {
        out.declaration = MakeStreamOutputDeclaration(*out.so);
        for (u32 b = 0; b < 4; ++b) out.strides[b] = layout_set[b].Stride();
        desc.StreamOutput = DescribeStreamOutput(out.declaration, out.strides, config.rasterize);
        CheckCaptureLayout(has_gs ? gs_spirv : vs_spirv, out.declaration);
    }
    if (!config.rasterize) desc.PS = {};
    static constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 2> elements{{
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};
    desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    desc.IBStripCutValue = config.cut;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.PrimitiveTopologyType = config.type;
    if (config.rasterize) {
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    const HRESULT hr = device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out.pso));
    if (FAILED(hr)) {
        MessagesCheck("CreateGraphicsPipelineState");
        throw Failure(fmt::format("CreateGraphicsPipelineState 0x{:08X}", static_cast<u32>(hr)));
    }
    out.compile_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start).count();
    return out;
}

void Bench::SetRuntimeData(CommandStream* cmd, const Pipeline& pipeline, u32 yz_flip_mask) {
    dxil_spirv_vertex_runtime_data data{};
    data.yz_flip_mask = yz_flip_mask;
    data.viewport_width = RT_SIZE;
    data.viewport_height = RT_SIZE;
    std::array<u32, sizeof(data) / 4> words{};
    std::memcpy(words.data(), &data, sizeof(data));
    const UINT count = std::min<UINT>(pipeline.layout->RuntimeDataWords(),
                                      static_cast<UINT>(words.size()));
    if (count != 0) {
        cmd->SetGraphicsRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX, count,
                                           words.data(), 0);
    }
}

/// One draw as RasterizerD3D12 records it: capture off disables the stream output, and binding
/// the targets is the last thing before the draw.
void Bench::Draw(const Pipeline& pipeline, const DrawCall& call) {
    if (Regs().transform_feedback_enabled == 0) {
        stream_output.Disable();
    }
    ID3D12Resource* const vb = UploadBytes(call.vertices.data(), call.vertices.size() * 32);
    ID3D12Resource* ib = nullptr;
    if (!call.indices.empty()) {
        if (call.index32) {
            ib = UploadBytes(call.indices.data(), call.indices.size() * 4);
        } else {
            std::vector<u16> narrow(call.indices.begin(), call.indices.end());
            ib = UploadBytes(narrow.data(), narrow.size() * 2);
        }
    }
    CommandStream* const cmd = scheduler.CommandList();
    cmd->SetGraphicsRootSignature(pipeline.layout->Handle());
    SetRuntimeData(cmd, pipeline, call.yz_flip_mask);
    scheduler.SetPipelineState(pipeline.pso.Get());
    cmd->IASetPrimitiveTopology(call.topology);
    const D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(),
                                       static_cast<UINT>(call.vertices.size() * 32), 32};
    cmd->IASetVertexBuffers(0, 1, &vbv);
    if (ib) {
        const D3D12_INDEX_BUFFER_VIEW ibv{
            ib->GetGPUVirtualAddress(),
            static_cast<UINT>(call.indices.size() * (call.index32 ? 4 : 2)),
            call.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
        cmd->IASetIndexBuffer(&ibv);
    }
    if (pipeline.rasterize) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, RT_SIZE, RT_SIZE, 0, 1};
        const D3D12_RECT scissor{0, 0, RT_SIZE, RT_SIZE};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
    }
    if (pipeline.layout->StreamOutput() && Regs().transform_feedback_enabled != 0) {
        stream_output.Bind(cmd, Regs(), bindings);
    }
    if (ib) {
        cmd->DrawIndexedInstanced(static_cast<UINT>(call.indices.size()), 1, 0, 0, 0);
    } else {
        cmd->DrawInstanced(static_cast<UINT>(call.vertices.size()), 1, 0, 0);
    }
}

// --- expected captures -----------------------------------------------------------------------

/// The CPU side of a capture: what each buffer must hold after a series of draws.
class Expectation {
public:
    Expectation(Bench& bench_, const ShaderSpec& spec_) : bench{bench_}, spec{spec_} {
        for (u32 b = 0; b < 4; ++b) {
            const Target& target = bench.targets[b];
            if (target.buffer) {
                images[b].assign(target.buffer->SizeBytes() / 4, Expect{Expect::Hole, 0});
            }
        }
    }

    /// Capture restarts at start_offset (registers changed, or capture turned off and on).
    void Restart() {
        written = {};
    }

    /// The vertices of one draw, primitives in order; a geometry shader emits copies of each.
    void Capture(const std::vector<Primitive>& primitives, const std::vector<Vertex>& vertices) {
        const u32 copies = std::max(spec.gs_copies, 1u);
        // The caller's vertices may not outlive the draw: keep a copy for Verify.
        draws.push_back(std::make_unique<const std::vector<Vertex>>(vertices));
        for (const Primitive& primitive : primitives) {
            const u64 vertex_count = primitive.v.size() * copies;
            // A primitive that does not fit in every bound buffer is written to none of them;
            // an unbound buffer (null view) discards without overflowing.
            bool fits = true;
            for (u32 b = 0; b < 4; ++b) {
                if (!Bound(b)) continue;
                const u64 need = vertex_count * bench.layouts[b].Stride();
                fits &= written[b] + need <= bench.bindings[b].size;
            }
            if (!fits) {
                ++dropped_primitives;
                continue;
            }
            Group group;
            group.vertices = draws.back().get();
            group.candidates.push_back(primitive.v);
            if (primitive.rotatable) {
                const auto& v = primitive.v;
                group.candidates.push_back({v[1], v[2], v[0]});
                group.candidates.push_back({v[2], v[0], v[1]});
            }
            group.copies = copies;
            for (u32 b = 0; b < 4; ++b) {
                group.offsets[b] = bench.targets[b].start_offset + written[b];
                if (Bound(b)) written[b] += vertex_count * bench.layouts[b].Stride();
            }
            groups.push_back(std::move(group));
        }
    }

    u64 Written(u32 b) const {
        return written[b];
    }
    u32 dropped_primitives = 0;

    /// Reads every target and compares it with the model, word by word.
    void Verify() {
        std::array<std::vector<u32>, 4> actual;
        for (u32 b = 0; b < 4; ++b) {
            if (bench.targets[b].buffer) actual[b] = bench.Read(*bench.targets[b].buffer);
        }
        for (const Group& group : groups) {
            // An odd strip triangle may start at any vertex: take the rotation the data shows.
            size_t chosen = 0;
            for (size_t candidate = 0; candidate < group.candidates.size(); ++candidate) {
                if (Matches(group, group.candidates[candidate], actual)) {
                    chosen = candidate;
                    break;
                }
            }
            Place(group, group.candidates[chosen]);
        }
        std::string report;
        u32 mismatches = 0;
        for (u32 b = 0; b < 4; ++b) {
            for (size_t w = 0; w < images[b].size(); ++w) {
                const Expect& e = images[b][w];
                const u32 got = actual[b][w];
                const bool ok = e.kind == Expect::Undefined ||
                                (e.kind == Expect::Hole ? got == SENTINEL : got == e.bits);
                if (!ok && ++mismatches <= 8) {
                    report += fmt::format(
                        "\n    buffer {} byte {}: expected {} got 0x{:08X} ({})", b, w * 4,
                        e.kind == Expect::Hole
                            ? std::string("untouched")
                            : fmt::format("0x{:08X} ({})", e.bits, std::bit_cast<float>(e.bits)),
                        got, std::bit_cast<float>(got));
                }
            }
        }
        CHECK(mismatches == 0, "{} words differ from the model:{}", mismatches, report);
    }

private:
    struct Group {
        const std::vector<Vertex>* vertices{};
        std::vector<std::vector<u32>> candidates;
        u32 copies{};
        std::array<u64, 4> offsets{};
    };

    bool Bound(u32 b) const {
        return bench.bindings[b].size != 0 && bench.layouts[b].Stride() != 0;
    }

    template <typename Fn>
    void ForEachWord(const Group& group, const std::vector<u32>& order, Fn&& fn) const {
        for (u32 b = 0; b < 4; ++b) {
            if (!Bound(b)) continue;
            const BufferLayout& layout = bench.layouts[b];
            u64 offset = group.offsets[b];
            for (u32 k = 0; k < group.copies; ++k) {
                for (u32 v : order) {
                    for (size_t w = 0; w < layout.attrs.size(); ++w) {
                        fn(b, (offset + w * 4) / 4,
                           Model(spec, layout.attrs[w], (*group.vertices)[v], k));
                    }
                    offset += layout.Stride();
                }
            }
        }
    }

    bool Matches(const Group& group, const std::vector<u32>& order,
                 const std::array<std::vector<u32>, 4>& actual) const {
        bool ok = true;
        ForEachWord(group, order, [&](u32 b, u64 word, const Expect& e) {
            if (e.kind == Expect::Value && actual[b][word] != e.bits) ok = false;
        });
        return ok;
    }

    void Place(const Group& group, const std::vector<u32>& order) {
        ForEachWord(group, order, [&](u32 b, u64 word, const Expect& e) {
            CHECK(word < images[b].size(), "capture past buffer {} end", b);
            images[b][word] = e;
        });
    }

    Bench& bench;
    ShaderSpec spec;
    std::array<std::vector<Expect>, 4> images;
    std::array<u64, 4> written{};
    std::vector<std::unique_ptr<const std::vector<Vertex>>> draws;
    std::vector<Group> groups;
};

/// One capture end to end: pipeline, targets, draw, compare with the model and the byte count.
struct Capture {
    ShaderSpec spec;
    Layouts layouts;
    PipelineConfig config;
    DrawCall call;
    std::array<u32, 4> start_offsets{};
    std::array<std::optional<u32>, 4> sizes{};
    std::array<bool, 4> enables{true, true, true, true};
};

void RunCapture(Bench& bench, const Capture& c) {
    bench.layouts = c.layouts;
    for (u32 b = 0; b < 4; ++b) {
        if (c.layouts[b].attrs.empty()) continue;
        const u32 size = c.sizes[b].value_or(64 * 1024);
        bench.AddTarget(b, Common::AlignUp(c.start_offsets[b] + size + 64, 256), c.start_offsets[b],
                        size);
        bench.targets[b].enable = c.enables[b];
    }
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(c.spec, c.layouts, c.config);
    CHECK(pipeline.so->missing_outputs == 0 && pipeline.so->dropped_entries == 0,
          "spirv_to_dxil: {} missing outputs, {} dropped entries", pipeline.so->missing_outputs,
          pipeline.so->dropped_entries);
    bench.Draw(pipeline, c.call);
    Expectation expect{bench, c.spec};
    expect.Capture(Assemble(c.call.topology,
                            c.call.indices.empty() ? Iota(static_cast<u32>(c.call.vertices.size()))
                                                   : c.call.indices),
                   c.call.vertices);
    expect.Verify();
    for (u32 b = 0; b < 4; ++b) {
        if (bench.bindings[b].size == 0 || bench.Regs().transform_feedback.controls[b].stream != 0) {
            continue;
        }
        const std::optional<u32> count = bench.ByteCount(0);
        CHECK(count.has_value(), "no byte count for stream 0");
        CHECK(*count == expect.Written(b), "byte count {} expected {} (buffer {})", *count,
              expect.Written(b), b);
        break;
    }
}

std::vector<u32> WithRestarts(const std::vector<u32>& order, u32 restart_value) {
    std::vector<u32> out;
    for (u32 v : order) out.push_back(v == RESTART ? restart_value : v);
    return out;
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// =============================================================================================
// Sanity

void SanityCompilerCaps(Bench& bench) {
    CHECK(bench.compiler.IsAvailable(), "spirv_to_dxil missing");
    CHECK(bench.compiler.CanLinkStages(), "spirv_to_dxil cannot link stages");
    CHECK(bench.compiler.SupportsStreamOutput(),
          "spirv_to_dxil without eden_spirv_to_dxil_pipeline_v4: native capture is off");
}

void SanityRootSignatures(Bench& bench) {
    ProgramBuilder builder;
    IR::Program vs, fs;
    BuildVertex(builder, vs, {});
    BuildFragment(builder, fs, {});
    const std::array<const Shader::Info*, 5> infos{&vs.info, nullptr, nullptr, nullptr, &fs.info};
    const PipelineLayout& plain = bench.root_signatures.Get(infos, false, false);
    const PipelineLayout& so = bench.root_signatures.Get(infos, false, true);
    CHECK(!plain.StreamOutput() && so.StreamOutput(), "stream output flag {} / {}",
          plain.StreamOutput(), so.StreamOutput());
    CHECK(plain.Handle() != so.Handle(),
          "a stream output pipeline shares the root signature without ALLOW_STREAM_OUTPUT");
    CHECK(&bench.root_signatures.Get(infos, false, true) == &so, "layout not cached");
    CHECK(so.DrawSignature(false) != nullptr && so.DrawSignature(true) != nullptr,
          "missing ExecuteIndirect signatures");
}

void SanityPipelineStates(Bench& bench) {
    const Layouts layouts{BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}};
    // The cache keys pipelines on the live registers: buffer 0 has to be enabled to capture.
    bench.layouts = layouts;
    bench.AddTarget(0, 4096, 0, 1024);
    bench.SetTargets();
    for (const bool rasterize : {false, true}) {
        const Pipeline pipeline = bench.Build({.fs_loads = 1}, layouts, {.rasterize = rasterize});
        CHECK(pipeline.native, "not native");
        CHECK(pipeline.so->entry_count != 0, "empty declaration");
        CHECK(pipeline.so->missing_outputs == 0 && pipeline.so->dropped_entries == 0,
              "missing {} dropped {}", pipeline.so->missing_outputs, pipeline.so->dropped_entries);
        u32 components = 0;
        for (const auto& entry : pipeline.declaration) {
            CHECK(entry.OutputSlot == 0, "entry for slot {}", entry.OutputSlot);
            components += entry.ComponentCount;
        }
        CHECK(components == 8, "the declaration covers {} components, the layout 8", components);
    }
}

void SanitySinglePoint(Bench& bench) {
    RunCapture(bench, {.layouts = {BufferLayout{0, Vector(Pos(0), 0, 4)}},
                       .config = {.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT},
                       .call = {.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST,
                                .vertices = MakeVertices(1)}});
}

void SanityStreamOutputState(Bench& bench) {
    for (u32 stream = 0; stream < 4; ++stream) {
        CHECK(!bench.stream_output.ByteCountSlot(bench.Regs(), stream).has_value(),
              "stream {} has a byte count before any capture", stream);
    }
    bench.layouts = {BufferLayout{0, Vector(Pos(0), 0, 4)}};
    bench.AddTarget(0, 1024);
    bench.SetTargets();
    CHECK(!bench.stream_output.ByteCountSlot(bench.Regs(), 0).has_value(),
          "a byte count before the first capture");
    const Pipeline pipeline = bench.Build({}, bench.layouts);
    bench.Draw(pipeline, {.vertices = MakeVertices(3)});
    CHECK(bench.stream_output.ByteCountSlot(bench.Regs(), 0) == 0u, "stream 0 not on buffer 0");
    CHECK(!bench.stream_output.ByteCountSlot(bench.Regs(), 1).has_value(),
          "stream 1 has a byte count without a buffer");
    CHECK_BYTE_COUNT(bench, 0, 48u);
}

// =============================================================================================
// Functional

void FunctionalTriangleList(Bench& bench) {
    RunCapture(bench, {.spec = {.vs_generics = 0b111, .vs_partial = 0b100},
                       .layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4),
                                                        Vector(Gen(1, 0), 0, 3),
                                                        Vector(Gen(2, 0), 0, 2)})}},
                       .call = {.vertices = MakeVertices(12)}});
}

void FunctionalIndexedList(Bench& bench) {
    // Repeated and out-of-order indices: capture follows the index stream, one vertex per index.
    const std::vector<u32> indices{5, 0, 3, 3, 0, 5, 7, 7, 7, 1, 2, 6, 4, 4, 2};
    for (const bool index32 : {false, true}) {
        bench.Reset();
        RunCapture(bench, {.spec = {.vs_generics = 1},
                           .layouts = {BufferLayout{0, Cat({Vector(Gen(0, 0), 0, 4),
                                                            Vector(Pos(0), 0, 4)})}},
                           .call = {.vertices = MakeVertices(8), .indices = indices,
                                    .index32 = index32}});
    }
}

void FunctionalStrips(Bench& bench) {
    RunCapture(bench, {.spec = {.vs_generics = 1},
                       .layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}},
                       .call = {.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                                .vertices = MakeVertices(9)}});
    bench.Reset();
    RunCapture(bench, {.spec = {.vs_generics = 1},
                       .layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)}},
                       .config = {.type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
                       .call = {.topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
                                .vertices = MakeVertices(7)}});
}

void FunctionalGeometryShader(Bench& bench) {
    RunCapture(bench, {.spec = {.vs_generics = 0b11, .gs_copies = 3},
                       .layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4),
                                                        Vector(Gen(1, 0), 0, 4)})}},
                       .call = {.vertices = MakeVertices(6)}});
    bench.Reset();
    // Strip input: the geometry shader sees each assembled triangle.
    RunCapture(bench, {.spec = {.vs_generics = 1, .gs_copies = 2},
                       .layouts = {BufferLayout{0, Cat({Vector(Gen(0, 0), 0, 4), Vector(Pos(0), 0, 2)})}},
                       .call = {.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                                .vertices = MakeVertices(6)}});
}

void FunctionalMultipleBuffers(Bench& bench) {
    // Three buffers, different strides and start offsets, components out of declaration order,
    // holes in the middle and at the end, and a captured generic the shader never writes
    // (undefined, but its neighbours must land where the layout says).
    RunCapture(bench,
               {.spec = {.vs_generics = 0b1011},
                .layouts = {BufferLayout{0, Cat({Vector(Gen(3, 0), 0, 4), {HOLE},
                                                 Vector(Pos(0), 0, 4)})},
                            BufferLayout{0, Cat({Vector(Gen(1, 0), 0, 2), {HOLE, HOLE},
                                                 Vector(Gen(0, 0), 0, 4), Vector(Gen(2, 0), 0, 4)})},
                            BufferLayout{0, Cat({Vector(Pos(0), 2, 2), {HOLE}})}},
                .call = {.vertices = MakeVertices(9)},
                .start_offsets = {0, 64, 12}});
}

// =============================================================================================
// Integration

void IntegrationAppend(Bench& bench) {
    // Draws with the same registers append; the byte count covers all of them.
    const ShaderSpec spec{.vs_generics = 1};
    bench.layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}};
    bench.AddTarget(0, 4096, 32);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    Expectation expect{bench, spec};
    const auto first = MakeVertices(6);
    std::vector<Vertex> second(9);
    for (u32 v = 0; v < 9; ++v) second[v] = MakeVertex(100 + v);
    bench.Draw(pipeline, {.vertices = first});
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(6)), first);
    bench.Draw(pipeline, {.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, .vertices = second});
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, Iota(9)), second);
    expect.Verify();
    CHECK_BYTE_COUNT(bench, 0, expect.Written(0));
}

void IntegrationRestart(Bench& bench) {
    // Changing a buffer register, or turning capture off and on, starts the buffer over at
    // start_offset; the registers alone do not.
    const ShaderSpec spec{.vs_generics = 1};
    bench.layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)}};
    bench.AddTarget(0, 4096);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    const auto a = MakeVertices(3);
    std::vector<Vertex> b(3), c(3), d(3);
    for (u32 v = 0; v < 3; ++v) {
        b[v] = MakeVertex(10 + v);
        c[v] = MakeVertex(20 + v);
        d[v] = MakeVertex(30 + v);
    }
    const auto triangle = Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(3));

    bench.Draw(pipeline, {.vertices = a});
    bench.Draw(pipeline, {.vertices = b});
    CHECK_BYTE_COUNT(bench, 0, 96u);

    // Off then on with the same registers: starts over at 0, over the first triangle.
    bench.Regs().transform_feedback_enabled = 0;
    bench.Draw(pipeline, {.vertices = a}); // not captured
    bench.Regs().transform_feedback_enabled = 1;
    bench.Draw(pipeline, {.vertices = c});
    {
        Expectation expect{bench, spec};
        expect.Capture(triangle, c);
        expect.Capture(triangle, b); // the second triangle of the first pass is still there
        expect.Verify();
        CHECK_BYTE_COUNT(bench, 0, 48u);
    }

    // A new start_offset: starts over there.
    bench.targets[0].start_offset = 256;
    bench.targets[0].size = 1024;
    bench.SetTargets();
    bench.Draw(pipeline, {.vertices = d});
    CHECK_BYTE_COUNT(bench, 0, 48u);
    const auto words = bench.Read(*bench.targets[0].buffer);
    CHECK(words[256 / 4] == std::bit_cast<u32>(d[0].in1[0] + 10.0f), "start_offset not honored");
    CHECK(words[48 / 4] == std::bit_cast<u32>(b[0].in1[0] + 10.0f),
          "the earlier capture was overwritten");
}

void IntegrationByteCountStreams(Bench& bench) {
    // The byte count of a stream is the first enabled buffer of that stream. A vertex shader
    // only feeds stream 0, so a stream 1 buffer gets nothing.
    const ShaderSpec spec{.vs_generics = 0b11};
    bench.layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)},
                     BufferLayout{0, Cat({Vector(Gen(1, 0), 0, 4), Vector(Pos(0), 0, 4)})},
                     BufferLayout{1, Vector(Gen(0, 0), 0, 2)}};
    bench.AddTarget(0, 4096);
    bench.AddTarget(1, 4096);
    bench.AddTarget(2, 4096);
    bench.targets[0].enable = false;
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    const auto vertices = MakeVertices(6);
    bench.Draw(pipeline, {.vertices = vertices});
    CHECK(bench.stream_output.ByteCountSlot(bench.Regs(), 0) == 1u,
          "stream 0 must report buffer 1 (buffer 0 is disabled)");
    CHECK_BYTE_COUNT(bench, 0, 6u * 32u);
    const auto stream1 = bench.ByteCount(1);
    CHECK(stream1.value_or(0) == 0, "stream 1 count {} with nothing emitted to it", *stream1);
    const auto untouched = bench.Read(*bench.targets[2].buffer);
    CHECK(std::all_of(untouched.begin(), untouched.end(), [](u32 w) { return w == SENTINEL; }),
          "a vertex shader wrote the stream 1 buffer");
    Expectation expect{bench, spec};
    bench.bindings[2] = {}; // stream 1: modelled as capturing nothing
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(6)), vertices);
    expect.Verify();
}

void IntegrationDrawAuto(Bench& bench) {
    // Capture, report the byte count to guest memory, then draw from it: the captured buffer is
    // the vertex buffer of a DrawAuto, captured again. Layout = input layout (32 bytes), so the
    // second capture is the first with generic 0 increased by 10 once more.
    const ShaderSpec spec{.vs_generics = 1};
    const Layouts layouts{BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}};
    bench.layouts = layouts;
    bench.AddTarget(0, 4096);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, layouts);
    const auto vertices = MakeVertices(15);
    bench.Draw(pipeline, {.vertices = vertices});

    auto report = bench.NewBuffer(256);
    report->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    CommandStream* cmd = bench.scheduler.CommandList();
    const auto slot = bench.stream_output.ByteCountSlot(bench.Regs(), 0);
    CHECK(slot.has_value(), "no byte count slot");
    bench.stream_output.CopyByteCount(cmd, *slot, report->Handle(), 0);

    // Second target at another guest address: its counter starts at 0.
    Target first = std::move(bench.targets[0]);
    bench.AddTarget(0, 4096);
    bench.SetTargets();
    first.buffer->Transition(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
    report->Transition(D3D12_RESOURCE_STATE_GENERIC_READ, false);
    cmd = bench.scheduler.CommandList();
    constexpr u32 yz_flip_mask = 0x0001'0001;
    ID3D12Resource* const record =
        bench.stream_output.BuildDrawAutoRecord(cmd, report->Address(), 32, yz_flip_mask);
    cmd->SetGraphicsRootSignature(pipeline.layout->Handle());
    bench.SetRuntimeData(cmd, pipeline, 0);
    bench.scheduler.SetPipelineState(pipeline.pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vbv{first.buffer->Address(),
                                       static_cast<UINT>(first.buffer->SizeBytes()), 32};
    cmd->IASetVertexBuffers(0, 1, &vbv);
    bench.stream_output.Bind(cmd, bench.Regs(), bench.bindings);
    ID3D12CommandSignature* const signature = pipeline.layout->DrawSignature(false);
    CHECK(signature != nullptr, "no draw signature");
    cmd->ExecuteIndirect(signature, 1, record, 0, nullptr, 0);

    TransitionResource(cmd, record, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto words = bench.ReadResource(record, 0, INDIRECT_DRAW_WORDS * 4);
    const std::array<u32, INDIRECT_DRAW_WORDS> expected_record{0, 0, 0, yz_flip_mask, 0, 15, 1, 0, 0};
    CHECK(std::equal(words.begin(), words.end(), expected_record.begin()),
          "DrawAuto record {}", fmt::join(words, ","));

    std::vector<Vertex> redrawn = vertices;
    for (Vertex& v : redrawn) {
        for (u32 c = 0; c < 4; ++c) v.in1[c] += 10.0f;
    }
    Expectation expect{bench, spec};
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(15)), redrawn);
    expect.Verify();
    CHECK_BYTE_COUNT(bench, 0, 15u * 32u);
}

void IntegrationRenderAndCapture(Bench& bench) {
    // Rasterization on: the capture is complete and the fragment shader still draws (stream 0
    // goes to the rasterizer).
    const ShaderSpec spec{.vs_generics = 1, .fs_loads = 1};
    std::vector<Vertex> vertices{{{-1, -1, 0.5f, 1}, {7, 8, 9, 10}},
                                 {{-1, 3, 0.5f, 1}, {7, 8, 9, 10}},
                                 {{3, -1, 0.5f, 1}, {7, 8, 9, 10}}};
    bench.layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}};
    bench.AddTarget(0, 1024);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts, {.rasterize = true});
    const float clear[4]{0, 0, 0, 0};
    bench.scheduler.CommandList()->ClearRenderTargetView(
        bench.rtv_heap->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
    bench.Draw(pipeline, {.vertices = vertices});
    Expectation expect{bench, spec};
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(3)), vertices);
    expect.Verify();

    auto readback = CreateCommittedBuffer(bench.device.Get(), 256 * Bench::RT_SIZE,
                                          D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_FLAG_NONE, "rt readback");
    CommandStream* const cmd = bench.scheduler.CommandList();
    TransitionResource(cmd, bench.render_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, Bench::RT_SIZE, Bench::RT_SIZE, 1, 256}}};
    const D3D12_TEXTURE_COPY_LOCATION src{.pResource = bench.render_target.Get(),
                                          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                          .SubresourceIndex = 0};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    TransitionResource(cmd, bench.render_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    bench.scheduler.Finish();
    float* texels{};
    ThrowIfFailed(readback->Map(0, nullptr, reinterpret_cast<void**>(&texels)), "map rt");
    const float* center = texels + (Bench::RT_SIZE / 2) * 64 + (Bench::RT_SIZE / 2) * 4;
    const std::array<float, 4> pixel{center[0], center[1], center[2], center[3]};
    readback->Unmap(0, nullptr);
    for (u32 c = 0; c < 4; ++c) {
        const float expected = vertices[0].in1[c] + 10.0f;
        CHECK(std::abs(pixel[c] - expected) < 1e-3f, "pixel {} = {} expected {}", c, pixel[c],
              expected);
    }
}

// =============================================================================================
// Regression

void RegressionGuestPosition(Bench& bench) {
    // The host converts depth to [0, 1] and flips Y/Z for the viewport after the guest shader;
    // capture must keep the position the guest wrote.
    for (const bool gs : {false, true}) {
        bench.Reset();
        RunCapture(bench, {.spec = {.vs_generics = 1, .gs_copies = gs ? 1u : 0u, .convert_depth = true},
                           .layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 1)})}},
                           .call = {.vertices = MakeVertices(6, 2.0f), .yz_flip_mask = 0x0001'0001}});
    }
}

void RegressionNoPerspective(Bench& bench) {
    // A NoPerspective varying the fragment shader reads, with w = 2: the lowering would export
    // a * w. Captured, it keeps native interpolation and the capture reads a itself.
    RunCapture(bench, {.spec = {.vs_generics = 0b11, .fs_loads = 0b11, .fs_noperspective = 0b11},
                       .layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)}},
                       .config = {.rasterize = true},
                       .call = {.vertices = MakeVertices(6, 2.0f)}});
    // The lowering must refuse a captured varying instead of capturing a * w.
    bool refused = false;
    try {
        bench.Reset();
        RunCapture(bench, {.spec = {.vs_generics = 1, .fs_loads = 1, .fs_noperspective = 1},
                           .layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)}},
                           .config = {.rasterize = true, .force_lower_mask = 1u},
                           .call = {.vertices = MakeVertices(3, 2.0f)}});
    } catch (const Shader::NotImplementedException&) {
        refused = true;
    }
    CHECK(refused, "the NoPerspective lowering accepted a captured varying");
}

void RegressionAcrossSubmits(Bench& bench) {
    // The counters and the buffers' states live across command lists: append through a Flush
    // and a Finish (resource states decay to COMMON between lists).
    const ShaderSpec spec{.vs_generics = 1};
    bench.layouts = {BufferLayout{0, Cat({Vector(Gen(0, 0), 0, 4), Vector(Pos(0), 0, 4)})}};
    bench.AddTarget(0, 8192, 16);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    Expectation expect{bench, spec};
    for (u32 draw = 0; draw < 6; ++draw) {
        std::vector<Vertex> vertices(6);
        for (u32 v = 0; v < 6; ++v) vertices[v] = MakeVertex(draw * 10 + v);
        bench.Draw(pipeline, {.vertices = vertices});
        expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(6)), vertices);
        if (draw % 3 == 0) bench.scheduler.Flush();
        if (draw % 3 == 1) bench.scheduler.Finish();
        if (draw == 4) CHECK_BYTE_COUNT(bench, 0, expect.Written(0));
    }
    expect.Verify();
    CHECK_BYTE_COUNT(bench, 0, expect.Written(0));
}

void RegressionOverflow(Bench& bench) {
    // Buffer 1 has room for 2.5 triangles: only whole primitives are written, and when one does
    // not fit in buffer 1 it is written to no buffer at all.
    const ShaderSpec spec{.vs_generics = 0b11};
    bench.layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})},
                     BufferLayout{0, Vector(Gen(1, 0), 0, 4)}};
    bench.AddTarget(0, 4096);
    bench.AddTarget(1, 1024, 32, 2 * 48 + 24);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    const auto vertices = MakeVertices(12);
    bench.Draw(pipeline, {.vertices = vertices});
    Expectation expect{bench, spec};
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(12)), vertices);
    CHECK(expect.dropped_primitives == 2, "model dropped {}", expect.dropped_primitives);
    expect.Verify();
    CHECK_BYTE_COUNT(bench, 0, 6u * 32u);
    // Appending to a full buffer writes nothing more.
    bench.Draw(pipeline, {.vertices = MakeVertices(3)});
    expect.Verify();
}

void RegressionUnreadCaptures(Bench& bench) {
    // The fragment shader reads none of the captured varyings (linking may not drop them), one
    // captured generic is never written by the shader (declared for stream output anyway).
    RunCapture(bench, {.spec = {.vs_generics = 0b100101, .fs_loads = 0b100000},
                       .layouts = {BufferLayout{0, Cat({Vector(Gen(0, 0), 0, 4), Vector(Gen(1, 0), 0, 4),
                                                        Vector(Gen(2, 0), 0, 4), Vector(Pos(0), 0, 4)})}},
                       .config = {.rasterize = true},
                       .call = {.vertices = MakeVertices(9)}});
}

void RegressionDriverFaultLayout(Bench& bench) {
    // The vertex-only pass the Series driver faulted compiling (0xC0000005): xyz of generic 0 in
    // buffer 0 and x of generics 1 and 2 in buffers 1 and 2, nothing rasterized. Packed, the
    // capture of generic 1 shared a register with generic 0; each must now be alone in its own.
    RunCapture(bench, {.spec = {.vs_generics = 0b111},
                       .layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 3)},
                                   BufferLayout{0, Vector(Gen(1, 0), 0, 1)},
                                   BufferLayout{0, Vector(Gen(2, 0), 0, 1)}},
                       .call = {.vertices = MakeVertices(9)}});
}

void RegressionCopiesAcrossVectors(Bench& bench) {
    // Consecutive words of different vectors share a copy, a hole or a fifth word starts the
    // next, a partial generic is copied without its neighbours, and a repeated capture (generic
    // 0 z in two buffers) gets a copy of its own.
    RunCapture(bench, {.spec = {.vs_generics = 0b1111, .vs_partial = 0b1000, .fs_loads = 0b1111},
                       .layouts = {BufferLayout{0, Cat({Vector(Gen(0, 0), 2, 2),
                                                        Vector(Gen(1, 0), 0, 2), Vector(Pos(0), 0, 4),
                                                        {HOLE}, Vector(Gen(1, 0), 3, 1),
                                                        Vector(Gen(3, 0), 0, 2)})},
                                   BufferLayout{0, Cat({Vector(Gen(2, 0), 1, 3), {HOLE},
                                                        Vector(Gen(0, 0), 2, 1)})}},
                       .config = {.rasterize = true},
                       .call = {.vertices = MakeVertices(9)}});
}

void RegressionFullRegisters(Bench& bench) {
    // 30 generics stored, all captured whole in one buffer with the position: no free location
    // for 31 copies, so the whole generics carry their captures themselves, still one output per
    // register, and the position takes the location left.
    BufferLayout layout{0, Vector(Pos(0), 0, 4)};
    for (u32 g = 0; g < 30; ++g) {
        const auto part = Vector(Gen(g, 0), 0, 4);
        layout.attrs.insert(layout.attrs.end(), part.begin(), part.end());
    }
    RunCapture(bench, {.spec = {.vs_generics = (1u << 30) - 1, .fs_loads = (1u << 30) - 1},
                       .layouts = {layout},
                       .config = {.rasterize = true},
                       .call = {.vertices = MakeVertices(6)}});
}

// =============================================================================================
// Performance

void PerformanceCompile(Bench& bench) {
    // Budget: stream output may not make pipeline builds much slower.
    const Layouts layouts{BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4),
                                               Vector(Gen(1, 0), 0, 4)})}};
    const ShaderSpec spec{.vs_generics = 0b111, .fs_loads = 0b100};
    std::vector<double> with, without;
    for (u32 i = 0; i < 7; ++i) {
        with.push_back(bench.Build(spec, layouts).compile_ms);
        without.push_back(bench.Build(spec, layouts, {.capture = false}).compile_ms);
    }
    const double a = Median(with), b = Median(without);
    std::cout << fmt::format("      compile median: {:.2f} ms with stream output, {:.2f} ms without\n", a, b);
    CHECK(a < 3.0 * b + 5.0, "stream output build {:.2f} ms vs {:.2f} ms", a, b);
    CHECK(a < 1000.0, "stream output build {:.2f} ms", a);
}

void PerformanceBind(Bench& bench) {
    // Budget: Bind runs every capturing draw; with unchanged registers it must stay cheap and
    // must not clear the counters again.
    const ShaderSpec spec{.vs_generics = 1};
    bench.layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)}, BufferLayout{0, Vector(Pos(0), 0, 4)}};
    bench.AddTarget(0, 4096);
    bench.AddTarget(1, 4096);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    bench.Draw(pipeline, {.vertices = MakeVertices(3)});
    std::vector<double> samples;
    for (u32 batch = 0; batch < 20; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 500; ++i) {
            bench.stream_output.Bind(bench.scheduler.CommandList(), bench.Regs(), bench.bindings);
        }
        samples.push_back(std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - start).count() / 500);
    }
    const double per_call = Median(samples);
    std::cout << fmt::format("      Bind: {:.3f} us per call (median of 20 x 500)\n", per_call);
    CHECK(per_call < 10.0, "Bind costs {:.3f} us", per_call);
    // 10k binds without draws do not move the counters: the next draw appends after the first.
    bench.Draw(pipeline, {.vertices = MakeVertices(3)});
    CHECK_BYTE_COUNT(bench, 0, 96u);
}

void PerformanceThroughput(Bench& bench) {
    // ~1M vertices captured in one draw, all of them checked against the model.
    constexpr u32 count = 1'000'002;
    const ShaderSpec spec{.vs_generics = 1};
    bench.layouts = {BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}};
    bench.AddTarget(0, count * 32 + 256);
    bench.SetTargets();
    const Pipeline pipeline = bench.Build(spec, bench.layouts);
    const auto vertices = MakeVertices(count);
    bench.scheduler.Finish();
    std::vector<double> runs;
    Expectation expect{bench, spec};
    for (u32 run = 0; run < 3; ++run) {
        if (run != 0) {
            bench.Regs().transform_feedback_enabled = 0;
            bench.stream_output.Disable();
            bench.Regs().transform_feedback_enabled = 1;
        }
        bench.Draw(pipeline, {.vertices = vertices});
        const auto start = std::chrono::steady_clock::now();
        bench.scheduler.Finish();
        runs.push_back(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start).count());
    }
    expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Iota(count)), vertices);
    expect.Verify();
    const double ms = Median(runs);
    std::cout << fmt::format("      {} vertices, {} MB captured: {:.2f} ms ({:.0f} Mvertices/s)\n",
                             count, count * 32 / (1024 * 1024), ms, count / ms / 1000.0);
    CHECK(ms < 500.0, "1M vertex capture took {:.2f} ms", ms);
    CHECK_BYTE_COUNT(bench, 0, count * 32u);
}

void PerformanceDrawAutoRecord(Bench& bench) {
    // The first record builds the compute pipeline; afterwards a record must be cheap.
    auto source = bench.NewBuffer(256);
    const u32 value = 32 * 99;
    ID3D12Resource* const upload = bench.UploadBytes(&value, 4);
    source->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    bench.scheduler.CommandList()->CopyBufferRegion(source->Handle(), 0, upload, 0, 4);
    source->Transition(D3D12_RESOURCE_STATE_GENERIC_READ, false);
    const auto first_start = std::chrono::steady_clock::now();
    ID3D12Resource* record = bench.stream_output.BuildDrawAutoRecord(
        bench.scheduler.CommandList(), source->Address(), 32, 0);
    const double first = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - first_start).count();
    std::vector<double> samples;
    for (u32 batch = 0; batch < 10; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 100; ++i) {
            record = bench.stream_output.BuildDrawAutoRecord(bench.scheduler.CommandList(),
                                                             source->Address(), 16, batch);
        }
        samples.push_back(std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - start).count() / 100);
    }
    const double steady = Median(samples);
    std::cout << fmt::format("      DrawAuto record: first {:.2f} ms, then {:.3f} us\n", first, steady);
    CHECK(steady < 20.0, "record costs {:.3f} us", steady);
    TransitionResource(bench.scheduler.CommandList(), record, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto words = bench.ReadResource(record, 0, INDIRECT_DRAW_WORDS * 4);
    const std::array<u32, INDIRECT_DRAW_WORDS> expected{0, 0, 0, 9, 0, 32 * 99 / 16, 1, 0, 0};
    CHECK(std::equal(words.begin(), words.end(), expected.begin()), "last record {}",
          fmt::join(words, ","));
}

void PerformanceWidestLayout(Bench& bench) {
    // 30 generics and the position over four buffers: 124 of the 128 components one stream can
    // capture, 32 output registers with the position copy. Must build in budget and be exact.
    Layouts layouts;
    layouts[0].attrs = Vector(Pos(0), 0, 4);
    for (u32 g = 0; g < 30; ++g) {
        auto& attrs = layouts[g < 7 ? 0 : g < 15 ? 1 : g < 23 ? 2 : 3].attrs;
        const auto part = Vector(Gen(g, 0), 0, 4);
        attrs.insert(attrs.end(), part.begin(), part.end());
    }
    const auto start = std::chrono::steady_clock::now();
    RunCapture(bench, {.spec = {.vs_generics = (1u << 30) - 1}, .layouts = layouts,
                       .call = {.vertices = MakeVertices(6)}});
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start).count();
    std::cout << fmt::format("      widest layout: {:.2f} ms build + capture + check\n", ms);
    CHECK(ms < 3000.0, "{:.2f} ms", ms);
}

// =============================================================================================
// Exploratory

u32 g_seed = 0x7F3D'0001;

void ExploratoryFuzz(Bench& bench) {
    // Random layouts, offsets, sizes, topologies and shaders, checked against the model.
    std::mt19937 rng{g_seed};
    const auto pick = [&](u32 n) { return static_cast<u32>(rng() % n); };
    for (u32 iteration = 0; iteration < 30; ++iteration) {
        bench.Reset();
        Capture c;
        c.spec.vs_generics = 0;
        // Generic vectors 0..11 and the position, each captured at most once.
        std::vector<u32> vectors(13);
        for (u32 i = 0; i < 13; ++i) vectors[i] = i;
        std::shuffle(vectors.begin(), vectors.end(), rng);
        const u32 buffers = 1 + pick(4);
        const u32 used = 1 + pick(8);
        for (u32 i = 0; i < used; ++i) {
            const u32 vec = vectors[i];
            const u32 base = vec == 12 ? Pos(0) : Gen(vec, 0);
            const u32 first = pick(4) == 0 ? pick(4) : 0;
            const u32 count = 1 + pick(4 - first);
            auto& attrs = c.layouts[pick(buffers)].attrs;
            if (pick(5) == 0) attrs.push_back(HOLE);
            const auto part = Vector(base, first, count);
            attrs.insert(attrs.end(), part.begin(), part.end());
            if (vec != 12 && pick(8) != 0) c.spec.vs_generics |= 1u << vec;
            if (vec != 12 && pick(6) == 0) c.spec.vs_partial |= 1u << vec;
        }
        for (u32 b = 0; b < 4; ++b) {
            if (c.layouts[b].attrs.empty()) continue;
            if (pick(4) == 0) c.layouts[b].attrs.push_back(HOLE);
            c.start_offsets[b] = pick(17) * 4;
            if (pick(4) == 0) c.sizes[b] = c.layouts[b].Stride() * (3 + pick(20));
        }
        const u32 count = 3 + pick(30);
        c.call.vertices = MakeVertices(count, pick(2) ? 1.0f : 0.5f);
        switch (pick(4)) {
        case 0:
            break;
        case 1:
            c.call.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case 2:
            for (u32 i = 0; i < count; ++i) c.call.indices.push_back(pick(count));
            break;
        default:
            c.call.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            for (u32 i = 0; i < count; ++i) c.call.indices.push_back(pick(count));
            c.call.index32 = pick(2) != 0;
            break;
        }
        try {
            RunCapture(bench, c);
        } catch (const std::exception& e) {
            std::string layout;
            for (u32 b = 0; b < 4; ++b) {
                layout += fmt::format(" [{}: {} @{} size {}]", b, fmt::join(c.layouts[b].attrs, ","),
                                      c.start_offsets[b], c.sizes[b].value_or(0));
            }
            throw Failure(fmt::format("seed {:#x} iteration {}: generics {:#x} partial {:#x} "
                                      "topology {} indexed {}{}: {}",
                                      g_seed, iteration, c.spec.vs_generics, c.spec.vs_partial,
                                      static_cast<int>(c.call.topology), !c.call.indices.empty(),
                                      layout, e.what()));
        }
    }
}

void ExploratoryPrimitiveRestart(Bench& bench) {
    const std::vector<u32> order{0, 1, 2, 3, 4, RESTART, 5, 6, RESTART, 7, 8, 9, 10, 11, 12, RESTART, 13};
    for (const bool index32 : {false, true}) {
        bench.Reset();
        Capture c{.spec = {.vs_generics = 1},
                  .layouts = {BufferLayout{0, Cat({Vector(Gen(0, 0), 0, 4), Vector(Pos(0), 0, 4)})}},
                  .config = {.cut = index32 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                            : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF},
                  .call = {.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                           .vertices = MakeVertices(14),
                           .indices = WithRestarts(order, index32 ? 0xFFFF'FFFFu : 0xFFFFu),
                           .index32 = index32}};
        bench.layouts = c.layouts;
        bench.AddTarget(0, 4096);
        bench.SetTargets();
        const Pipeline pipeline = bench.Build(c.spec, c.layouts, c.config);
        bench.Draw(pipeline, c.call);
        Expectation expect{bench, c.spec};
        expect.Capture(Assemble(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, order), c.call.vertices);
        expect.Verify();
        CHECK_BYTE_COUNT(bench, 0, expect.Written(0));
    }
}

void ExploratoryBufferEdges(Bench& bench) {
    // A disabled buffer between two enabled ones: the others are complete and it is untouched.
    RunCapture(bench, {.spec = {.vs_generics = 0b111},
                       .layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)},
                                   BufferLayout{0, Vector(Gen(1, 0), 0, 4)},
                                   BufferLayout{0, Cat({Vector(Gen(2, 0), 0, 4), Vector(Pos(0), 0, 4)})}},
                       .call = {.vertices = MakeVertices(9)},
                       .enables = {true, false, true, true}});
    const auto middle = bench.Read(*bench.targets[1].buffer);
    CHECK(std::all_of(middle.begin(), middle.end(), [](u32 w) { return w == SENTINEL; }),
          "the disabled buffer was written");
    // The same varying captured into two buffers, which Maxwell allows.
    bench.Reset();
    RunCapture(bench, {.spec = {.vs_generics = 1},
                       .layouts = {BufferLayout{0, Vector(Gen(0, 0), 0, 4)},
                                   BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}},
                       .call = {.vertices = MakeVertices(6)}});
}

void ExploratoryBuiltins(Bench& bench) {
    // Built-in outputs: point size and clip distances from a vertex shader, layer and viewport
    // index from a geometry shader.
    RunCapture(bench, {.spec = {.vs_generics = 1, .vs_point_size = true, .vs_clip_distances = 2},
                       .layouts = {BufferLayout{0, Cat({{POINT_SIZE}, Vector(Clip(0), 0, 2),
                                                        Vector(Gen(0, 0), 0, 1)})}},
                       .call = {.vertices = MakeVertices(6)}});
    bench.Reset();
    RunCapture(bench, {.spec = {.vs_generics = 1, .gs_copies = 1, .gs_layer = true},
                       .layouts = {BufferLayout{0, Cat({{LAYER, VIEWPORT}, Vector(Gen(0, 0), 0, 2)})}},
                       .call = {.vertices = MakeVertices(6)}});
}

void ExploratoryTopologies(Bench& bench) {
    const Layouts layouts{BufferLayout{0, Cat({Vector(Pos(0), 0, 4), Vector(Gen(0, 0), 0, 4)})}};
    const std::array<std::pair<D3D_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>, 4> cases{{
        {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT},
        {D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
        {D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
        {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
    }};
    for (const auto& [topology, type] : cases) {
        // An incomplete trailing primitive is not recorded: 11 vertices.
        bench.Reset();
        RunCapture(bench, {.spec = {.vs_generics = 1}, .layouts = layouts, .config = {.type = type},
                           .call = {.topology = topology, .vertices = MakeVertices(11)}});
    }
}

// =============================================================================================

struct Test {
    const char* category;
    const char* name;
    void (*run)(Bench&);
};

constexpr std::array TESTS{
    Test{"sanity", "compiler capabilities", SanityCompilerCaps},
    Test{"sanity", "stream output root signatures", SanityRootSignatures},
    Test{"sanity", "pipeline states with and without rasterization", SanityPipelineStates},
    Test{"sanity", "one point captured", SanitySinglePoint},
    Test{"sanity", "byte count slots before and after a capture", SanityStreamOutputState},
    Test{"functional", "triangle list with partial varyings", FunctionalTriangleList},
    Test{"functional", "indexed list in index order (16 and 32 bit)", FunctionalIndexedList},
    Test{"functional", "triangle and line strips split into primitives", FunctionalStrips},
    Test{"functional", "geometry shader amplification", FunctionalGeometryShader},
    Test{"functional", "three buffers: strides, offsets, holes", FunctionalMultipleBuffers},
    Test{"integration", "draws append, byte count covers them", IntegrationAppend},
    Test{"integration", "restart on register change and re-enable", IntegrationRestart},
    Test{"integration", "byte count per stream", IntegrationByteCountStreams},
    Test{"integration", "DrawAuto from a captured buffer", IntegrationDrawAuto},
    Test{"integration", "capture while rasterizing", IntegrationRenderAndCapture},
    Test{"regression", "guest position before depth conversion and flips", RegressionGuestPosition},
    Test{"regression", "captured NoPerspective varying", RegressionNoPerspective},
    Test{"regression", "append across command lists", RegressionAcrossSubmits},
    Test{"regression", "overflow writes whole primitives to every buffer or none", RegressionOverflow},
    Test{"regression", "captures the fragment shader does not read", RegressionUnreadCaptures},
    Test{"regression", "the layout the Series driver faulted on", RegressionDriverFaultLayout},
    Test{"regression", "copies across vectors, holes and repeats", RegressionCopiesAcrossVectors},
    Test{"regression", "every register taken: whole generics capture in place", RegressionFullRegisters},
    Test{"performance", "pipeline build cost", PerformanceCompile},
    Test{"performance", "Bind cost", PerformanceBind},
    Test{"performance", "1M vertex capture", PerformanceThroughput},
    Test{"performance", "DrawAuto record cost", PerformanceDrawAutoRecord},
    Test{"performance", "widest layout", PerformanceWidestLayout},
    Test{"exploratory", "randomized layouts", ExploratoryFuzz},
    Test{"exploratory", "primitive restart", ExploratoryPrimitiveRestart},
    Test{"exploratory", "disabled and duplicated buffers", ExploratoryBufferEdges},
    Test{"exploratory", "built-in outputs", ExploratoryBuiltins},
    Test{"exploratory", "points, lines, incomplete primitives", ExploratoryTopologies},
};

} // Anonymous namespace

int main(int argc, char** argv) try {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            g_seed = static_cast<u32>(std::stoul(argv[++i], nullptr, 0));
        } else {
            filter = arg;
        }
    }
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    SetAssertHook(CountAssert);
    Bench bench;
    bench.messages->ClearStoredMessages();
    u32 passed = 0, failed = 0;
    for (const Test& test : TESTS) {
        if (!filter.empty() && filter != test.category) continue;
        bench.Reset();
        bench.messages->ClearStoredMessages();
        const u32 asserts = g_assert_failures;
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        try {
            test.run(bench);
            bench.scheduler.Finish();
            bench.MessagesCheck("after the test");
            if (g_assert_failures != asserts) {
                error = fmt::format("{} assertion(s) failed in the emulator code (see the log)",
                                    g_assert_failures - asserts);
            }
        } catch (const std::exception& e) {
            error = e.what();
            try {
                bench.scheduler.Finish();
                bench.messages->ClearStoredMessages();
            } catch (...) {
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        if (error.empty()) {
            ++passed;
            std::cout << fmt::format("PASS [{}] {} ({:.0f} ms)\n", test.category, test.name, ms);
        } else {
            ++failed;
            std::cout << fmt::format("FAIL [{}] {} ({:.0f} ms)\n    {}\n", test.category,
                                     test.name, ms, error);
        }
        std::cout.flush();
    }
    std::cout << fmt::format("{} passed, {} failed (seed {:#x})\n", passed, failed, g_seed);
    return failed == 0 ? 0 : 1;
} catch (const std::exception& e) {
    std::cerr << "bench setup: " << e.what() << '\n';
    return 2;
}
