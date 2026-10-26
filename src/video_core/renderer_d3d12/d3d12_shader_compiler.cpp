// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

// First: dxcapi.h relies on the Windows/COM declarations d3d12_device.h pulls in.
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

#include <dxcapi.h>
#include <fmt/format.h>

#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_dxil_disk_cache.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_stats.h"
#include "video_core/frame_trace.h"

namespace D3D12 {

namespace {

/// Shader model the translator may use: the Xbox Series UWP runtime reports 6.4.
constexpr dxil_shader_model SHADER_MODEL = SHADER_MODEL_6_4;
/// Validator rules the translator targets; dxil.dll only has to be at least this new.
constexpr dxil_validator_version VALIDATOR_VERSION = DXIL_VALIDATOR_1_4;

/// Minimal IDxcBlob over caller-owned memory, so the validator can sign a buffer in place.
class BorrowedBlob final : public IDxcBlob {
public:
    BorrowedBlob(void* data_, size_t size_) : data{data_}, size{size_} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcBlob)) {
            *out = static_cast<IDxcBlob*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // Lives on the caller's stack for the duration of one Validate call.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override {
        return data;
    }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override {
        return size;
    }

private:
    void* data;
    size_t size;
};

std::atomic<DxilSignMode> sign_mode{DxilSignMode::Validator};
std::atomic<u64> digests_matched{};
std::atomic<u64> digests_mismatched{};

/// Container header: "DXBC", the digest, the version and the size the digest covers.
constexpr size_t DXIL_DIGEST_OFFSET = 4;
constexpr size_t DXIL_DIGEST_SIZE = 16;
constexpr size_t DXIL_HASHED_OFFSET = DXIL_DIGEST_OFFSET + DXIL_DIGEST_SIZE;
constexpr size_t DXIL_SIZE_OFFSET = 24;

/// One MD5 block (RFC 1321).
void Md5Block(std::array<u32, 4>& state, const u32* x) {
    static constexpr std::array<u32, 64> k{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static constexpr std::array<u32, 16> s{7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (u32 i = 0; i < 64; ++i) {
        u32 f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        f += a + k[i] + x[g];
        a = d;
        d = c;
        c = b;
        b += std::rotl(f, static_cast<int>(s[(i / 16) * 4 + (i & 3)]));
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

/// The digest DXC's validator writes (ComputeHashRetail in DxilHash.cpp): MD5 with the bit count
/// moved to the first word of the last block and 1 | size * 2 in its last word.
std::array<u32, 4> DxilDigest(const u8* data, u32 size) {
    std::array<u32, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const u32 tail = size & 63;
    const u32 full = size - tail;
    std::array<u32, 16> block;
    for (u32 offset = 0; offset < full; offset += 64) {
        std::memcpy(block.data(), data + offset, 64);
        Md5Block(state, block.data());
    }
    auto* const bytes = reinterpret_cast<u8*>(block.data());
    if (tail < 56) {
        block.fill(0);
        block[0] = size << 3;
        std::memcpy(bytes + 4, data + full, tail);
        bytes[4 + tail] = 0x80;
    } else {
        block.fill(0);
        std::memcpy(bytes, data + full, tail);
        if (tail < 64) {
            bytes[tail] = 0x80;
        }
        Md5Block(state, block.data());
        block.fill(0);
        block[0] = size << 3;
    }
    block[15] = 1 | (size << 1);
    Md5Block(state, block.data());
    return state;
}

void WriteDxilDigest(std::vector<u8>& dxil) {
    if (dxil.size() < DXIL_SIZE_OFFSET + sizeof(u32)) {
        throw std::runtime_error("D3D12: DXIL container too small to sign");
    }
    u32 size;
    std::memcpy(&size, dxil.data() + DXIL_SIZE_OFFSET, sizeof(size));
    if (size > dxil.size() || size < DXIL_HASHED_OFFSET) {
        throw std::runtime_error("D3D12: DXIL container size out of range");
    }
    const std::array<u32, 4> digest =
        DxilDigest(dxil.data() + DXIL_HASHED_OFFSET, size - DXIL_HASHED_OFFSET);
    std::memcpy(dxil.data() + DXIL_DIGEST_OFFSET, digest.data(), DXIL_DIGEST_SIZE);
}

void CountDigest(const std::vector<u8>& signed_dxil, const std::array<u8, DXIL_DIGEST_SIZE>& own) {
    if (std::memcmp(signed_dxil.data() + DXIL_DIGEST_OFFSET, own.data(), DXIL_DIGEST_SIZE) == 0) {
        digests_matched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (digests_mismatched.fetch_add(1, std::memory_order_relaxed) < 8) {
        u32 size;
        std::memcpy(&size, signed_dxil.data() + DXIL_SIZE_OFFSET, sizeof(size));
        LOG_ERROR(Render, "D3D12: own DXIL digest differs from the validator's (size {}, tail {})",
                  size, (size - DXIL_HASHED_OFFSET) & 63);
    }
}

/// Bumped when MakeConf() or the record layout below changes what a cached pipeline means.
constexpr u32 DXIL_CACHE_FORMAT = 1;

/// The translator's identity: a rebuilt spirv_to_dxil.dll misses the old records.
u64 HashLoadedModule(const wchar_t* name) {
    const HMODULE module = GetModuleHandleW(name);
    std::array<wchar_t, MAX_PATH> path{};
    if (!module || GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())) == 0) {
        return 0;
    }
    std::ifstream file(path.data(), std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    return Common::CityHash64(bytes.data(), bytes.size());
}

template <typename T>
void Append(std::vector<u8>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool Take(std::span<const u8>& in, T& value) {
    if (in.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}

/// Record: stage count, then per stage its size, both metadata flags and the signed DXIL, then
/// the stream output declaration when the pipeline asked for one.
std::vector<u8> EncodeStages(std::span<const ShaderCompiler::CompiledStage> stages,
                             const eden_spirv_to_dxil_stream_output* stream_output) {
    std::vector<u8> out;
    Append(out, static_cast<u32>(stages.size()));
    for (const auto& stage : stages) {
        Append(out, static_cast<u32>(stage.dxil.size()));
        Append(out, static_cast<u8>(stage.metadata.requires_runtime_data));
        Append(out, static_cast<u8>(stage.metadata.needs_draw_sysvals));
        out.insert(out.end(), stage.dxil.begin(), stage.dxil.end());
    }
    if (stream_output) {
        Append(out, *stream_output);
    }
    return out;
}

std::optional<std::vector<ShaderCompiler::CompiledStage>> DecodeStages(
    std::span<const u8> in, size_t count, eden_spirv_to_dxil_stream_output* stream_output) {
    u32 stored_count{};
    if (!Take(in, stored_count) || stored_count != count) {
        return std::nullopt;
    }
    std::vector<ShaderCompiler::CompiledStage> stages(count);
    for (auto& stage : stages) {
        u32 size{};
        u8 runtime_data{};
        u8 draw_sysvals{};
        if (!Take(in, size) || !Take(in, runtime_data) || !Take(in, draw_sysvals) ||
            in.size() < size) {
            return std::nullopt;
        }
        stage.dxil.assign(in.begin(), in.begin() + size);
        stage.metadata.requires_runtime_data = runtime_data != 0;
        stage.metadata.needs_draw_sysvals = draw_sysvals != 0;
        in = in.subspan(size);
    }
    if (stream_output && !Take(in, *stream_output)) {
        return std::nullopt;
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return stages;
}

void LogTranslatorMessage(void*, const char* msg) {
    // Validation errors (e.g. sampling an integer texture) name the failing instruction: each
    // distinct message is its own bug.
    BUG_TRACK_KEY(ShaderCompile, Common::CityHash64(msg, std::strlen(msg)), "spirv_to_dxil: {}",
                  msg);
    const Common::BugTracker::TapMute bug_tracker_mute;
    LOG_ERROR(Render, "spirv_to_dxil: {}", msg);
}

} // Anonymous namespace

void SetDxilSignMode(DxilSignMode mode) {
    sign_mode.store(mode, std::memory_order_relaxed);
}

void LogDxilDigestChecks() {
    if (sign_mode.load(std::memory_order_relaxed) != DxilSignMode::Verify) {
        return;
    }
    LOG_INFO(Render, "D3D12: own DXIL digests: {} matched the validator, {} differed",
             digests_matched.load(std::memory_order_relaxed),
             digests_mismatched.load(std::memory_order_relaxed));
}

ShaderCompiler::ShaderCompiler() {
    if (!spirv_to_dxil_library.Open("spirv_to_dxil.dll")) {
        LOG_ERROR(Render, "D3D12: spirv_to_dxil.dll could not be loaded (error {})",
                  GetLastError());
        return;
    }
    if (!spirv_to_dxil_library.GetSymbol("spirv_to_dxil", &translate) ||
        !spirv_to_dxil_library.GetSymbol("spirv_to_dxil_free", &free_dxil)) {
        LOG_ERROR(Render, "D3D12: spirv_to_dxil.dll lacks its entry points");
        return;
    }
    if (spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_pipeline_v2", &translate_pipeline)) {
        lowers_integer_sampling = true;
        // Optional: only pipelines the console's driver faults on need it.
        (void)spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_pipeline_v3", &translate_pipeline_v3);
        // Optional: without it, transform feedback falls back to storage buffers.
        (void)spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_pipeline_v4", &translate_pipeline_v4);
        decltype(&::eden_spirv_to_dxil_supported_flags) supported_flags{};
        if (translate_pipeline_v3 &&
            spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_supported_flags", &supported_flags)) {
            emulates_reduction = (supported_flags() & EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION) != 0;
        }
    } else if (!spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_pipeline",
                                                &translate_pipeline)) {
        LOG_WARNING(Render, "D3D12: spirv_to_dxil.dll cannot link stages (rebuild it with "
                            "tools/xbox/build-spirv-to-dxil.ps1); stages are translated alone");
    }
    if (!dxil_library.Open("dxil.dll")) {
        LOG_ERROR(Render, "D3D12: dxil.dll could not be loaded (error {})", GetLastError());
        return;
    }
    DxcCreateInstanceProc create_instance{};
    if (!dxil_library.GetSymbol("DxcCreateInstance", &create_instance)) {
        LOG_ERROR(Render, "D3D12: dxil.dll lacks DxcCreateInstance");
        return;
    }
    const HRESULT hr = create_instance(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
    if (FAILED(hr)) {
        LOG_ERROR(Render, "D3D12: creating the DXIL validator failed (HRESULT 0x{:08X})",
                  static_cast<u32>(hr));
        return;
    }

    UINT32 major = 0;
    UINT32 minor = 0;
    ComPtr<IDxcVersionInfo> version;
    if (SUCCEEDED(validator.As(&version))) {
        version->GetVersion(&major, &minor);
    }
    translator_id = HashLoadedModule(L"spirv_to_dxil.dll");
    LOG_INFO(Render, "D3D12: shader path ready (spirv_to_dxil{}{}, DXIL validator {}.{})",
             translate_pipeline ? " with stage linking" : "",
             lowers_integer_sampling ? " and integer sampling" : "", major, minor);
    if (translate_pipeline && !lowers_integer_sampling) {
        BUG_TRACK(ShaderCompile, "spirv_to_dxil.dll predates integer texture sampling: shaders "
                                 "sampling integer textures will fail (rebuild it)");
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_WARNING(Render, "D3D12: spirv_to_dxil.dll predates integer texture sampling; shaders "
                            "sampling integer textures will fail validation (rebuild it)");
    }
    available = true;
}

ShaderCompiler::~ShaderCompiler() {
    // The validator's code lives in dxil.dll: release it before the library unloads.
    validator.Reset();
}

std::vector<u8> ShaderCompiler::Compile(std::span<const u32> spirv, dxil_spirv_shader_stage stage,
                                        bool flip_y) const {
    if (!available) {
        throw std::runtime_error("D3D12: the shader path is unavailable");
    }
    dxil_spirv_runtime_conf conf = MakeConf();
    // An unconditional flip only applies to the viewports set in its mask.
    if (flip_y) {
        conf.yz_flip = {.mode = DXIL_SPIRV_Y_FLIP_UNCONDITIONAL, .y_mask = 1, .z_mask = 0};
    }

    // spirv_to_dxil dereferences both of these unconditionally.
    const dxil_spirv_debug_options debug_options{};
    const dxil_spirv_logger logger{.priv = nullptr, .log = LogTranslatorMessage};
    dxil_spirv_object object{};
    if (!translate(spirv.data(), spirv.size(), nullptr, 0, stage, "main", VALIDATOR_VERSION,
                   &debug_options, &conf, &logger, &object)) {
        throw std::runtime_error(
            fmt::format("D3D12: spirv_to_dxil failed for stage {}", static_cast<int>(stage)));
    }
    std::vector<u8> dxil(static_cast<const u8*>(object.binary.buffer),
                         static_cast<const u8*>(object.binary.buffer) + object.binary.size);
    free_dxil(&object);

    Sign(dxil);
    return dxil;
}

std::vector<ShaderCompiler::CompiledStage> ShaderCompiler::CompilePipeline(
    std::span<const PipelineStage> stages, const PipelineOptions& options, u64 trace_pipeline) const {
    if (!available) {
        throw std::runtime_error("D3D12: the shader path is unavailable");
    }
    if (stages.empty() || stages.size() > EDEN_SPIRV_TO_DXIL_MAX_STAGES) {
        throw std::runtime_error(fmt::format("D3D12: invalid pipeline of {} stages", stages.size()));
    }
    // The last stage before the fragment one owns the clip-space position the flip applies to.
    size_t flip_stage = stages.size();
    for (size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].stage != DXIL_SPIRV_SHADER_FRAGMENT &&
            stages[i].stage != DXIL_SPIRV_SHADER_COMPUTE) {
            flip_stage = i;
        }
    }
    std::array<dxil_spirv_runtime_conf, EDEN_SPIRV_TO_DXIL_MAX_STAGES> confs{};
    std::array<eden_spirv_to_dxil_stage, EDEN_SPIRV_TO_DXIL_MAX_STAGES> inputs{};
    for (size_t i = 0; i < stages.size(); ++i) {
        confs[i] = MakeConf();
        confs[i].first_vertex_and_base_instance_mode = options.first_vertex_and_base_instance;
        if (i == flip_stage) {
            confs[i].yz_flip = {.mode = options.yz_flip,
                                .y_mask = options.y_flip_mask,
                                .z_mask = options.z_flip_mask};
        }
        inputs[i] = {.words = stages[i].spirv.data(),
                     .word_count = stages[i].spirv.size(),
                     .stage = stages[i].stage,
                     .entry_point = "main",
                     .conf = &confs[i]};
    }

    const u32 flags = (options.flatten_vertex ? EDEN_SPIRV_TO_DXIL_FLATTEN_VERTEX : 0) |
                      (options.emulate_reduction ? EDEN_SPIRV_TO_DXIL_EMULATE_REDUCTION : 0);
    DxilDiskCache& disk_cache = GetDxilDiskCache();
    const bool use_disk_cache = disk_cache.IsOpen() && translator_id != 0;
    DxilDiskCache::Key disk_key{};
    if (use_disk_cache) {
        disk_key = DiskCacheKey(stages, options, flags);
        if (const auto payload = disk_cache.Find(disk_key)) {
            if (auto cached = DecodeStages(*payload, stages.size(), options.stream_output)) {
                return std::move(*cached);
            }
        }
    }

    const dxil_spirv_debug_options debug_options{};
    const dxil_spirv_logger logger{.priv = nullptr, .log = LogTranslatorMessage};
    std::array<dxil_spirv_object, EDEN_SPIRV_TO_DXIL_MAX_STAGES> objects{};
    VideoCore::FrameTrace::ScopedSpan translate_span{
        VideoCore::FrameTrace::Event::PipelineTranslateLong, trace_pipeline, trace_pipeline != 0};
    const u64 translate_start = SteadyMicros();
    if (options.emulate_reduction && !emulates_reduction) {
        throw std::runtime_error("D3D12: spirv_to_dxil.dll cannot emulate sampler reduction");
    }
    if (options.stream_output) {
        if (!translate_pipeline_v4) {
            throw std::runtime_error("D3D12: spirv_to_dxil.dll lacks eden_spirv_to_dxil_pipeline_v4");
        }
        if (!translate_pipeline_v4(inputs.data(), static_cast<unsigned>(stages.size()),
                                   VALIDATOR_VERSION, &debug_options, &logger, flags,
                                   objects.data(), options.stream_output)) {
            throw std::runtime_error("D3D12: eden_spirv_to_dxil_pipeline_v4 failed");
        }
    } else if (flags != 0) {
        if (!translate_pipeline_v3) {
            throw std::runtime_error("D3D12: spirv_to_dxil.dll lacks eden_spirv_to_dxil_pipeline_v3");
        }
        if (!translate_pipeline_v3(inputs.data(), static_cast<unsigned>(stages.size()),
                                   VALIDATOR_VERSION, &debug_options, &logger,
                                   flags, objects.data())) {
            throw std::runtime_error("D3D12: eden_spirv_to_dxil_pipeline_v3 failed");
        }
    } else if (translate_pipeline) {
        if (!translate_pipeline(inputs.data(), static_cast<unsigned>(stages.size()),
                                VALIDATOR_VERSION, &debug_options, &logger, objects.data())) {
            throw std::runtime_error("D3D12: eden_spirv_to_dxil_pipeline failed");
        }
    } else {
        for (size_t i = 0; i < stages.size(); ++i) {
            if (translate(inputs[i].words, inputs[i].word_count, nullptr, 0, inputs[i].stage,
                          "main", VALIDATOR_VERSION, &debug_options, &confs[i], &logger,
                          &objects[i])) {
                continue;
            }
            for (size_t j = 0; j < i; ++j) {
                free_dxil(&objects[j]);
            }
            throw std::runtime_error(fmt::format("D3D12: spirv_to_dxil failed for stage {}",
                                                 static_cast<int>(inputs[i].stage)));
        }
    }

    translate_span.Finish();
    GetPipelineStats().Record(D3D12::PipelineStage::Translate, SteadyMicros() - translate_start);
    std::vector<CompiledStage> result(stages.size());
    for (size_t i = 0; i < stages.size(); ++i) {
        const auto* data = static_cast<const u8*>(objects[i].binary.buffer);
        result[i].dxil.assign(data, data + objects[i].binary.size);
        result[i].metadata = objects[i].metadata;
        free_dxil(&objects[i]);
    }
    for (CompiledStage& stage : result) {
        Sign(stage.dxil, trace_pipeline);
    }
    if (use_disk_cache) {
        disk_cache.Store(disk_key, EncodeStages(result, options.stream_output));
    }
    return result;
}

u64 ShaderCompiler::TranslatorGeneration() const {
    return translator_id == 0 ? 0 : translator_id ^ DXIL_CACHE_FORMAT;
}

DxilDiskCache::Key ShaderCompiler::DiskCacheKey(std::span<const PipelineStage> stages,
                                                const PipelineOptions& options, u32 flags) const {
    // Everything that reaches the translator besides MakeConf(), which DXIL_CACHE_FORMAT covers.
    std::vector<u32> words{DXIL_CACHE_FORMAT,
                           static_cast<u32>(translator_id),
                           static_cast<u32>(translator_id >> 32),
                           static_cast<u32>(SHADER_MODEL),
                           static_cast<u32>(VALIDATOR_VERSION),
                           flags,
                           static_cast<u32>(options.yz_flip),
                           options.y_flip_mask,
                           options.z_flip_mask,
                           static_cast<u32>(options.first_vertex_and_base_instance),
                           options.stream_output != nullptr ? 1U : 0U,
                           static_cast<u32>(stages.size())};
    for (const auto& stage : stages) {
        words.push_back(static_cast<u32>(stage.stage));
        words.push_back(static_cast<u32>(stage.spirv.size()));
    }
    u128 hash = Common::CityHash128(reinterpret_cast<const char*>(words.data()),
                                    words.size() * sizeof(u32));
    for (const auto& stage : stages) {
        hash = Common::CityHash128WithSeed(reinterpret_cast<const char*>(stage.spirv.data()),
                                           stage.spirv.size_bytes(), hash);
    }
    return {.lo = hash[0], .hi = hash[1]};
}

dxil_spirv_runtime_conf ShaderCompiler::MakeConf() const {
    dxil_spirv_runtime_conf conf{};
    conf.runtime_data_cbv = {.register_space = RUNTIME_DATA_SPACE, .base_shader_register = 0};
    conf.push_constant_cbv = {.register_space = PUSH_CONSTANT_SPACE, .base_shader_register = 0};
    conf.first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO;
    conf.workgroup_id_mode = DXIL_SPIRV_SYSVAL_TYPE_NATIVE;
    conf.declared_read_only_images_as_srvs = true;
    conf.shader_model_max = SHADER_MODEL;
    return conf;
}

void ShaderCompiler::Sign(std::vector<u8>& dxil, u64 trace_pipeline) const {
    const DxilSignMode mode = sign_mode.load(std::memory_order_relaxed);
    if (mode == DxilSignMode::Hash) {
        const u64 sign_start = SteadyMicros();
        WriteDxilDigest(dxil);
        GetPipelineStats().Record(D3D12::PipelineStage::Sign, SteadyMicros() - sign_start);
        return;
    }
    std::array<u8, DXIL_DIGEST_SIZE> own_digest{};
    if (mode == DxilSignMode::Verify) {
        std::vector<u8> copy = dxil;
        WriteDxilDigest(copy);
        std::memcpy(own_digest.data(), copy.data() + DXIL_DIGEST_OFFSET, DXIL_DIGEST_SIZE);
    }
    // The runtime refuses unsigned DXIL outside developer mode; the validator writes the hash in place.
    BorrowedBlob blob{dxil.data(), dxil.size()};
    ComPtr<IDxcOperationResult> result;
    VideoCore::FrameTrace::ScopedSpan lock_span{
        VideoCore::FrameTrace::Event::PipelineValidatorLockLong, trace_pipeline, trace_pipeline != 0};
    const u64 lock_start = SteadyMicros();
    std::scoped_lock lock{validator_mutex};
    const u64 sign_start = SteadyMicros();
    GetPipelineStats().Record(D3D12::PipelineStage::SignLock, sign_start - lock_start);
    lock_span.Finish();
    VideoCore::FrameTrace::ScopedSpan sign_span{
        VideoCore::FrameTrace::Event::PipelineSignLong, trace_pipeline, trace_pipeline != 0};
    ThrowIfFailed(validator->Validate(&blob, DxcValidatorFlags_InPlaceEdit, &result),
                  "IDxcValidator::Validate");
    GetPipelineStats().Record(D3D12::PipelineStage::Sign, SteadyMicros() - sign_start);
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (SUCCEEDED(status)) {
        if (mode == DxilSignMode::Verify) {
            CountDigest(dxil, own_digest);
        }
        return;
    }
    std::string message = "(no details)";
    ComPtr<IDxcBlobEncoding> errors;
    if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors && errors->GetBufferSize() > 0) {
        message.assign(static_cast<const char*>(errors->GetBufferPointer()),
                       errors->GetBufferSize());
    }
    throw std::runtime_error(fmt::format("D3D12: DXIL validation failed (0x{:08X}): {}",
                                         static_cast<u32>(status), message));
}

} // namespace D3D12
