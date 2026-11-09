// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_glitch_hunt.h"

namespace D3D12 {

namespace {

/// Readback memory per captured frame; what does not fit is hashed but not compared.
constexpr u64 FRAME_BUDGET = 24ULL << 20;
/// Of it, what Dump may take per frame, and the frames it dumps (the .bin grows fast).
constexpr u64 DUMP_BUDGET = 12ULL << 20;
constexpr u32 DUMP_FRAMES = 20;
/// Closing the game menu hitches the first frames.
constexpr u32 WARMUP_FRAMES = 10;
constexpr u32 DEFAULT_FRAMES = 20;

std::atomic<u32> glitch_requested{};
std::atomic<u32> glitch_default_frames{DEFAULT_FRAMES};

} // namespace

void HuntGlitchesNextFrames(u32 frames) {
    if (frames == 0) {
        frames = glitch_default_frames.load(std::memory_order_relaxed);
    }
    glitch_requested.store(std::clamp<u32>(frames, 1, 240), std::memory_order_relaxed);
}

void SetGlitchFrames(u32 frames) {
    glitch_default_frames.store(frames == 0 ? DEFAULT_FRAMES : std::min<u32>(frames, 240),
                                std::memory_order_relaxed);
}

namespace {
/// Query slots per heap: one per recorded draw, reused round robin. Each slot is resolved right
/// after its draw, so reuse only needs the queue's order.
constexpr u32 PROBE_SLOTS = 8192;
/// Readback bytes of one probe: the occlusion count, then the pipeline statistics.
constexpr u64 PROBE_STATS_OFFSET = sizeof(u64);
constexpr u64 PROBE_BYTES = PROBE_STATS_OFFSET + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
} // namespace

GlitchHunt::GlitchHunt(const Device& device_, Scheduler& scheduler_, StagingBufferPool& staging_)
    : device{device_}, scheduler{scheduler_}, staging{staging_} {}

std::optional<GlitchHunt::Probe> GlitchHunt::BeginProbe(bool occlusion, bool stats,
                                                        ID3D12QueryHeap* guest_heap,
                                                        u32 guest_slot) {
    if (guest_heap) {
        occlusion = false; // the guest's slot counts the samples
    }
    if (probe_failed || (!occlusion && !stats && !guest_heap)) {
        return std::nullopt;
    }
    if (!occlusion_heap) {
        D3D12_QUERY_HEAP_DESC desc{.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION,
                                   .Count = PROBE_SLOTS,
                                   .NodeMask = 0};
        const HRESULT occlusion_hr =
            device.Get()->CreateQueryHeap(&desc, IID_PPV_ARGS(&occlusion_heap));
        desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        const HRESULT stats_hr = device.Get()->CreateQueryHeap(&desc, IID_PPV_ARGS(&stats_heap));
        if (FAILED(occlusion_hr) || FAILED(stats_hr)) {
            LOG_WARNING(Render, "D3D12: glitch hunt draw probes off: query heaps failed {:x} {:x}",
                        static_cast<u32>(occlusion_hr), static_cast<u32>(stats_hr));
            occlusion_heap.Reset();
            stats_heap.Reset();
            probe_failed = true;
            return std::nullopt;
        }
    }
    const Probe probe{.slot = probe_slot,
                      .occlusion = occlusion,
                      .stats = stats,
                      .guest_heap = guest_heap,
                      .guest_slot = guest_slot};
    probe_slot = (probe_slot + 1) % PROBE_SLOTS;
    auto* const cmd = scheduler.CommandList();
    if (occlusion) {
        cmd->BeginQuery(occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, probe.slot);
    }
    if (stats) {
        cmd->BeginQuery(stats_heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, probe.slot);
    }
    return probe;
}

void GlitchHunt::EndProbe(u32 call_index, const Probe& probe) {
    auto* const cmd = scheduler.CommandList();
    if (probe.occlusion) {
        cmd->EndQuery(occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, probe.slot);
    }
    if (probe.stats) {
        cmd->EndQuery(stats_heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, probe.slot);
    }
    const std::optional<u64> at = Reserve(PROBE_BYTES);
    if (!at) {
        return;
    }
    const u64 base = current.ref.offset + *at;
    if (probe.guest_heap) {
        cmd->ResolveQueryData(probe.guest_heap, D3D12_QUERY_TYPE_OCCLUSION, probe.guest_slot, 1,
                              current.ref.buffer, base);
    } else if (probe.occlusion) {
        cmd->ResolveQueryData(occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, probe.slot, 1,
                              current.ref.buffer, base);
    }
    if (probe.stats) {
        cmd->ResolveQueryData(stats_heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, probe.slot,
                              1, current.ref.buffer, base + PROBE_STATS_OFFSET);
    }
    current.pending.push_back({
        .kind = Kind::Probe,
        .frame = frame,
        .call = call_index,
        .role = "draw",
        .readback_offset = *at,
        .size = PROBE_BYTES,
        .stride = (probe.occlusion || probe.guest_heap ? 1u : 0u) | (probe.stats ? 2u : 0u) |
                  (probe.guest_heap ? 4u : 0u),
    });
    ++probes;
}

GlitchHunt::~GlitchHunt() {
    for (FrameReadback& readback : in_flight) {
        if (readback.ref.buffer) {
            staging.FreeDeferred(readback.ref);
        }
    }
    if (current.ref.buffer) {
        staging.FreeDeferred(current.ref);
    }
}

void GlitchHunt::Line(std::string_view line) {
    text.append(line);
    text.push_back('\n');
}

void GlitchHunt::Begin() {
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "glitch";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    path = (dir / fmt::format("glitch_{}.txt", stamp)).string();
    bin_path = (dir / fmt::format("glitch_{}.bin", stamp)).string();
    bin_size = 0;
    dump_used = 0;
    ++captures;
    frame = 0;
    call = 0;
    compares = 0;
    inspects = 0;
    probes = 0;
    targets_read = 0;
    mismatches = 0;
    skipped_budget = 0;
    compared.clear();
    text.clear();
    Line(fmt::format("# jadyx glitch hunt v16, capture {}, {} frames", captures, frames_left));
    Line("# D=draw C=dispatch B=binding M=host differs from guest H=GPU-written bytes "
         "X=transform feedback buffer T=GPU words (r=so counters, args, count, ring=ExecuteIndirect records) P=what a draw rasterized W=CPU write over GPU-written buffer bytes S=sampled texture G=depth/stencil state K=clear V=render target after a pass A=indirect draw arguments D=host bytes in the .bin (Y=guest bytes) "
         "F=frame end R=compare totals");
    LOG_INFO(Render, "D3D12 glitch hunt: capturing {} frames into {}", frames_left, path);
}

bool GlitchHunt::Compare(u32 call_index, std::string_view role, ID3D12Resource* resource,
                         u64 offset, std::span<const u8> guest, u64 guest_hash,
                         D3D12_RESOURCE_STATES resource_state) {
    if (!resource || guest.empty()) {
        return false;
    }
    const u64 size = guest.size();
    const u64 key = reinterpret_cast<std::uintptr_t>(resource) ^ (offset << 20) ^ size;
    const std::pair<u64, u64> entry{key, guest_hash};
    if (std::ranges::find(compared, entry) != compared.end()) {
        return false; // already compared this frame with the same guest bytes
    }
    const std::optional<u64> at = Reserve(size);
    if (!at) {
        return false;
    }
    compared.push_back(entry);
    CopyOut(resource, offset, size, *at, resource_state);
    current.pending.push_back({
        .kind = Kind::Compare,
        .frame = frame,
        .call = call_index,
        .role = std::string{role},
        .readback_offset = *at,
        .guest = {guest.begin(), guest.end()},
        .size = size,
    });
    ++compares;
    return true;
}

void GlitchHunt::CopyOut(ID3D12Resource* resource, u64 offset, u64 size, u64 at,
                         D3D12_RESOURCE_STATES resource_state) {
    // COMMON promotes to COPY_SOURCE on its own; GENERIC_READ includes it.
    const bool wrap = resource_state != D3D12_RESOURCE_STATE_COMMON &&
                      (resource_state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    auto* const cmd = scheduler.CommandList();
    if (wrap) {
        const D3D12_RESOURCE_BARRIER to{
            TransitionBarrier(resource, resource_state, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        cmd->ResourceBarrier(1, &to);
    }
    cmd->CopyBufferRegion(current.ref.buffer, current.ref.offset + at, resource, offset, size);
    if (wrap) {
        const D3D12_RESOURCE_BARRIER back{
            TransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, resource_state)};
        cmd->ResourceBarrier(1, &back);
    }
}

u64 GlitchHunt::DumpRoom() const noexcept {
    return frame < DUMP_FRAMES && dump_used < DUMP_BUDGET ? DUMP_BUDGET - dump_used : 0;
}

bool GlitchHunt::Dump(u32 call_index, std::string_view role, ID3D12Resource* resource,
                      u64 offset, u64 size, D3D12_RESOURCE_STATES resource_state,
                      std::string extra) {
    if (!resource || size == 0 || size > DumpRoom()) {
        return false;
    }
    const std::optional<u64> at = Reserve(size);
    if (!at) {
        return false;
    }
    dump_used += size;
    CopyOut(resource, offset, size, *at, resource_state);
    current.pending.push_back({
        .kind = Kind::Dump,
        .frame = frame,
        .call = call_index,
        .role = std::string{role},
        .readback_offset = *at,
        .size = size,
        .extra = std::move(extra),
    });
    return true;
}

void GlitchHunt::DumpGuest(u32 call_index, std::string_view role, std::span<const u8> guest,
                           std::string_view extra) {
    if (guest.empty() || frame >= DUMP_FRAMES) {
        return;
    }
    const u64 at = AppendBin(guest.data(), guest.size());
    Line(fmt::format("Y f={} n={} r={} bin={} size={} h={:016x} {}", frame, call_index, role, at,
                     guest.size(), Common::CityHash64(reinterpret_cast<const char*>(guest.data()),
                                                      guest.size()),
                     extra));
}

u64 GlitchHunt::AppendBin(const u8* data, u64 size) {
    const u64 at = bin_size;
    std::ofstream file{std::filesystem::path{bin_path}, std::ios::app | std::ios::binary};
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    bin_size += size;
    return at;
}

std::optional<u64> GlitchHunt::Reserve(u64 size) {
    const u64 aligned = (size + 15) & ~u64{15};
    if (!current.ref.buffer) {
        current.ref = staging.Request(FRAME_BUDGET, MemoryUsage::Download, true);
        current.used = 0;
        if (!current.ref.buffer || current.ref.mapped_span.size() < FRAME_BUDGET) {
            if (current.ref.buffer) {
                staging.FreeDeferred(current.ref);
            }
            current.ref = {};
            ++skipped_budget;
            return std::nullopt;
        }
    }
    if (current.used + aligned > FRAME_BUDGET) {
        ++skipped_budget;
        return std::nullopt;
    }
    const u64 at = current.used;
    current.used += aligned;
    current.tick = scheduler.CurrentTick();
    return at;
}

bool GlitchHunt::Inspect(u32 call_index, std::string_view role, ID3D12Resource* resource,
                         u64 offset, u64 size, u32 stride, std::string extra) {
    if (!resource || size == 0) {
        return false;
    }
    const std::optional<u64> at = Reserve(size);
    if (!at) {
        return false;
    }
    scheduler.CommandList()->CopyBufferRegion(current.ref.buffer, current.ref.offset + *at,
                                              resource, offset, size);
    current.pending.push_back({
        .kind = Kind::Inspect,
        .frame = frame,
        .call = call_index,
        .role = std::string{role},
        .readback_offset = *at,
        .size = size,
        .stride = stride,
        .extra = std::move(extra),
    });
    ++inspects;
    return true;
}

bool GlitchHunt::InspectWords(u32 call_index, std::string_view role, u64 size, u32 word_stride,
                              std::string extra,
                              const std::function<void(ID3D12Resource*, u64)>& copy) {
    const std::optional<u64> at = Reserve(size);
    if (!at) {
        return false;
    }
    copy(current.ref.buffer, current.ref.offset + *at);
    current.pending.push_back({
        .kind = Kind::Words,
        .frame = frame,
        .call = call_index,
        .role = std::string{role},
        .readback_offset = *at,
        .size = size,
        .stride = std::max<u32>(word_stride, 4),
        .extra = std::move(extra),
    });
    return true;
}

void GlitchHunt::ResolveInspect(const Pending& pending, const u8* host) {
    const u64 size = pending.size;
    u32 words = 0;
    u32 nonfinite = 0;
    u32 zeros = 0;
    u32 big = 0;
    float maxabs = 0.0f;
    for (u64 at = 0; at + 4 <= size; at += 4) {
        u32 word;
        std::memcpy(&word, host + at, 4);
        ++words;
        if (word == 0) {
            ++zeros;
            continue;
        }
        if ((word & 0x7f800000u) == 0x7f800000u) {
            ++nonfinite;
            continue;
        }
        const float value = std::fabs(std::bit_cast<float>(word));
        maxabs = std::max(maxabs, value);
        if (value > 1e4f) {
            ++big;
        }
    }
    // Bounding box of the first three floats of each element (positions when stride holds xyz).
    std::string box = "box=none";
    u32 elements = 0;
    u32 zero_elements = 0;
    if (pending.stride >= 12) {
        constexpr float inf = std::numeric_limits<float>::infinity();
        float lo[3]{inf, inf, inf};
        float hi[3]{-inf, -inf, -inf};
        for (u64 at = 0; at + pending.stride <= size; at += pending.stride) {
            float xyz[3];
            std::memcpy(xyz, host + at, 12);
            ++elements;
            if (xyz[0] == 0.0f && xyz[1] == 0.0f && xyz[2] == 0.0f) {
                ++zero_elements;
            }
            for (int c = 0; c < 3; ++c) {
                if (std::isfinite(xyz[c])) {
                    lo[c] = std::min(lo[c], xyz[c]);
                    hi[c] = std::max(hi[c], xyz[c]);
                }
            }
        }
        if (elements != 0 && lo[0] <= hi[0]) {
            box = fmt::format("box={:.4g},{:.4g},{:.4g}:{:.4g},{:.4g},{:.4g}", lo[0], lo[1], lo[2],
                              hi[0], hi[1], hi[2]);
        }
    }
    const u64 hash =
        Common::CityHash64(reinterpret_cast<const char*>(host), static_cast<size_t>(size));
    u32 head[4]{};
    std::memcpy(head, host, static_cast<size_t>(std::min<u64>(size, sizeof(head))));
    Line(fmt::format("H f={} n={} r={} len={} stride={} words={} zero={} nan={} big={} max={:.4g} "
                     "elems={} zelems={} {} h={:016x} head={:08x},{:08x},{:08x},{:08x} {}",
                     pending.frame, pending.call, pending.role, size, pending.stride, words, zeros,
                     nonfinite, big, maxabs, elements, zero_elements, box, hash, head[0], head[1],
                     head[2], head[3], pending.extra));
}

namespace {

/// Render target readback (ReadTarget): tile grid of the V lines, frames that also save an
/// image, and the width those images are box-filtered down to.
constexpr u32 TILES_X = 16;
constexpr u32 TILES_Y = 9;
constexpr u32 IMAGE_FRAMES = 3;
constexpr u32 IMAGE_WIDTH = 320;

float HalfToFloat(u16 half) {
    const u32 sign = u32{half & 0x8000u} << 16;
    const u32 exponent = (half >> 10) & 0x1f;
    const u32 mantissa = half & 0x3ff;
    if (exponent == 0x1f) {
        return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
    }
    if (exponent == 0) {
        const float value = static_cast<float>(mantissa) * (1.0f / 16777216.0f);
        return sign ? -value : value;
    }
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

/// Unsigned small float of R11G11B10: 5 exponent bits over mantissa_bits.
float SmallFloat(u32 bits, u32 mantissa_bits) {
    const u32 exponent = bits >> mantissa_bits;
    const u32 mantissa = bits & ((1u << mantissa_bits) - 1);
    if (exponent == 0x1f) {
        return mantissa ? std::numeric_limits<float>::quiet_NaN()
                        : std::numeric_limits<float>::infinity();
    }
    const float m = static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits);
    if (exponent == 0) {
        return std::ldexp(m, -14);
    }
    return std::ldexp(1.0f + m, static_cast<int>(exponent) - 15);
}

/// Bytes per texel, 0 when DecodeTexel cannot read the format, and whether it holds floats.
struct TexelFormat {
    u32 bytes;
    bool is_float;
};

TexelFormat TexelInfo(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_UINT:
        return {4, false};
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return {4, true};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
        return {8, true};
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return {8, false};
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return {16, true};
    case DXGI_FORMAT_R16_FLOAT:
        return {2, true};
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return {2, false};
    case DXGI_FORMAT_R8_UNORM:
        return {1, false};
    default:
        return {0, false};
    }
}

void DecodeTexel(DXGI_FORMAT format, u32 bytes, const u8* texel, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    u32 word = 0;
    std::memcpy(&word, texel, std::min<u32>(bytes, 4));
    const auto unorm8 = [word](u32 shift) {
        return static_cast<float>((word >> shift) & 0xff) / 255.0f;
    };
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        out[0] = unorm8(0);
        out[1] = unorm8(8);
        out[2] = unorm8(16);
        out[3] = unorm8(24);
        return;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        out[0] = unorm8(16);
        out[1] = unorm8(8);
        out[2] = unorm8(0);
        out[3] = unorm8(24);
        return;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        for (u32 c = 0; c < 3; ++c) {
            out[c] = static_cast<float>((word >> (c * 10)) & 0x3ff) / 1023.0f;
        }
        out[3] = static_cast<float>(word >> 30) / 3.0f;
        return;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        out[0] = SmallFloat(word & 0x7ff, 6);
        out[1] = SmallFloat((word >> 11) & 0x7ff, 6);
        out[2] = SmallFloat(word >> 22, 5);
        return;
    case DXGI_FORMAT_R16G16_FLOAT:
        out[0] = HalfToFloat(static_cast<u16>(word));
        out[1] = HalfToFloat(static_cast<u16>(word >> 16));
        return;
    case DXGI_FORMAT_R16G16_UNORM:
        out[0] = static_cast<float>(word & 0xffff) / 65535.0f;
        out[1] = static_cast<float>(word >> 16) / 65535.0f;
        return;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        out[0] = std::bit_cast<float>(word);
        return;
    case DXGI_FORMAT_R32_UINT:
        out[0] = static_cast<float>(word);
        return;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: {
        u16 halves[4];
        std::memcpy(halves, texel, sizeof(halves));
        for (u32 c = 0; c < 4; ++c) {
            out[c] = format == DXGI_FORMAT_R16G16B16A16_UNORM
                         ? static_cast<float>(halves[c]) / 65535.0f
                         : HalfToFloat(halves[c]);
        }
        return;
    }
    case DXGI_FORMAT_R32G32_FLOAT:
        std::memcpy(out, texel, 8);
        return;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        std::memcpy(out, texel, 16);
        return;
    case DXGI_FORMAT_R16_FLOAT:
        out[0] = HalfToFloat(static_cast<u16>(word));
        return;
    case DXGI_FORMAT_R16_UNORM:
        out[0] = static_cast<float>(word & 0xffff) / 65535.0f;
        return;
    case DXGI_FORMAT_R8G8_UNORM:
        out[0] = unorm8(0);
        out[1] = unorm8(8);
        return;
    case DXGI_FORMAT_R8_UNORM:
        out[0] = unorm8(0);
        return;
    default:
        return;
    }
}

/// One PPM channel: float formats are tone mapped (x / (1 + x), gamma 2.2) to show HDR.
u8 ImageByte(float value, bool is_float) {
    if (!std::isfinite(value)) {
        return 255; // non-finite texels show white
    }
    if (is_float) {
        value = std::max(value, 0.0f);
        value = std::pow(value / (1.0f + value), 1.0f / 2.2f);
    }
    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

void GlitchHunt::ReadTarget(std::string_view head, ID3D12Resource* resource, u32 subresource,
                            DXGI_FORMAT format, const std::function<void(bool)>& transition) {
    if (!resource) {
        Line(fmt::format("V f={} {} skip=null", frame, head));
        return;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) {
        Line(fmt::format("V f={} {} skip=dim{}:msaa{}", frame, head,
                         static_cast<int>(desc.Dimension), desc.SampleDesc.Count));
        return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    device.Get()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &row_bytes,
                                        &total);
    if (total == 0 || total == UINT64_MAX || total > (192ULL << 20)) {
        Line(fmt::format("V f={} {} skip=size{}", frame, head, total));
        return;
    }
    constexpr u64 PLACEMENT = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    StagingBufferRef ref = staging.Request(total + PLACEMENT, MemoryUsage::Download, true);
    if (!ref.buffer || ref.mapped_span.size() < total + PLACEMENT) {
        if (ref.buffer) {
            staging.FreeDeferred(ref);
        }
        Line(fmt::format("V f={} {} skip=staging", frame, head));
        return;
    }
    const u64 aligned = (ref.offset + PLACEMENT - 1) & ~(PLACEMENT - 1);
    footprint.Offset = aligned;
    transition(true);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = ref.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(false);
    scheduler.Finish();
    ++targets_read;

    const u8* const data = ref.mapped_span.data() + (aligned - ref.offset);
    const u32 width = footprint.Footprint.Width;
    const u32 height = rows;
    const u32 pitch = footprint.Footprint.RowPitch;
    if (format == DXGI_FORMAT_UNKNOWN) {
        format = footprint.Footprint.Format;
    }
    const TexelFormat texel = TexelInfo(format);
    const u32 texel_bytes =
        texel.bytes != 0 ? texel.bytes : static_cast<u32>(row_bytes / std::max(width, 1u));
    const bool decodes = texel.bytes != 0 && u64{texel.bytes} * width <= row_bytes;

    struct Tile {
        double sum[4]{};
        u64 hash = 0xcbf29ce484222325ULL;
        u32 texels{};
        u32 nonfinite{};
    };
    std::vector<Tile> tiles(TILES_X * TILES_Y);
    double sum[4]{};
    u64 nonfinite = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* const row = data + u64{y} * pitch;
        const u32 ty = std::min(y * TILES_Y / std::max(height, 1u), TILES_Y - 1);
        for (u32 tx = 0; tx < TILES_X; ++tx) {
            const u32 x0 = tx * width / TILES_X;
            const u32 x1 = (tx + 1) * width / TILES_X;
            Tile& tile = tiles[ty * TILES_X + tx];
            // FNV-1a over the tile's bytes of this row, eight at a time.
            const u8* bytes = row + u64{x0} * texel_bytes;
            u64 length = u64{x1 - x0} * texel_bytes;
            for (; length >= 8; length -= 8, bytes += 8) {
                u64 word;
                std::memcpy(&word, bytes, 8);
                tile.hash = (tile.hash ^ word) * 0x100000001b3ULL;
            }
            for (; length != 0; --length, ++bytes) {
                tile.hash = (tile.hash ^ *bytes) * 0x100000001b3ULL;
            }
            if (!decodes) {
                continue;
            }
            for (u32 x = x0; x < x1; ++x) {
                float value[4];
                DecodeTexel(format, texel_bytes, row + u64{x} * texel_bytes, value);
                if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
                    !std::isfinite(value[2]) || !std::isfinite(value[3])) {
                    ++tile.nonfinite;
                    ++nonfinite;
                    continue;
                }
                for (u32 c = 0; c < 4; ++c) {
                    tile.sum[c] += value[c];
                    sum[c] += value[c];
                }
                ++tile.texels;
            }
        }
    }
    const u64 finite_texels = u64{width} * height - nonfinite;
    const auto mean = [](double total_value, u64 count) {
        return count != 0 ? total_value / static_cast<double>(count) : 0.0;
    };
    u64 hash = 0xcbf29ce484222325ULL;
    std::string tile_text;
    tile_text.reserve(TILES_X * TILES_Y * 28);
    for (const Tile& tile : tiles) {
        hash = (hash ^ tile.hash) * 0x100000001b3ULL;
        if (!tile_text.empty()) {
            tile_text.push_back(';');
        }
        const u32 short_hash = static_cast<u32>(tile.hash ^ (tile.hash >> 32)) & 0xffff;
        if (!decodes) {
            tile_text += fmt::format("{:04x}", short_hash);
            continue;
        }
        tile_text += fmt::format("{:04x}:{:.3g},{:.3g},{:.3g},{:.3g}", short_hash,
                                 mean(tile.sum[0], tile.texels), mean(tile.sum[1], tile.texels),
                                 mean(tile.sum[2], tile.texels), mean(tile.sum[3], tile.texels));
        if (tile.nonfinite != 0) {
            tile_text += fmt::format(":n{}", tile.nonfinite);
        }
    }
    std::string image = "-";
    if (decodes && frame < IMAGE_FRAMES && width >= 64 && height >= 32) {
        const u32 step = std::max(1u, (width + IMAGE_WIDTH - 1) / IMAGE_WIDTH);
        const u32 out_width = width / step;
        const u32 out_height = height / step;
        std::string pixels;
        pixels.reserve(u64{out_width} * out_height * 3);
        for (u32 oy = 0; oy < out_height; ++oy) {
            for (u32 ox = 0; ox < out_width; ++ox) {
                double box[3]{};
                u32 count = 0;
                for (u32 dy = 0; dy < step; ++dy) {
                    const u8* const row = data + u64{oy * step + dy} * pitch;
                    for (u32 dx = 0; dx < step; ++dx) {
                        float value[4];
                        DecodeTexel(format, texel_bytes, row + u64{ox * step + dx} * texel_bytes,
                                    value);
                        if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
                            !std::isfinite(value[2])) {
                            continue;
                        }
                        for (u32 c = 0; c < 3; ++c) {
                            box[c] += value[c];
                        }
                        ++count;
                    }
                }
                for (u32 c = 0; c < 3; ++c) {
                    const float value = count == 0 ? std::numeric_limits<float>::infinity()
                                                   : static_cast<float>(box[c] / count);
                    pixels.push_back(static_cast<char>(ImageByte(value, texel.is_float)));
                }
            }
        }
        std::filesystem::path dir{path};
        dir.replace_extension();
        dir += "_img";
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        image = fmt::format("f{}_v{}.ppm", frame, targets_read);
        std::ofstream file{dir / image, std::ios::binary};
        file << fmt::format("P6\n{} {}\n255\n", out_width, out_height);
        file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    }
    staging.FreeDeferred(ref);
    Line(fmt::format("V f={} {} fmt={} w={} h={} sub={} mean={:.4g},{:.4g},{:.4g},{:.4g} nan={} "
                     "h={:016x} img={} g={}x{} t={}",
                     frame, head, static_cast<int>(format), width, height, subresource,
                     mean(sum[0], finite_texels), mean(sum[1], finite_texels),
                     mean(sum[2], finite_texels), mean(sum[3], finite_texels), nonfinite, hash,
                     image, TILES_X, TILES_Y, tile_text));
}

void GlitchHunt::Resolve(bool all_done) {
    for (auto it = in_flight.begin(); it != in_flight.end();) {
        if (!all_done && !scheduler.IsFree(it->tick)) {
            ++it;
            continue;
        }
        const u8* const mapped = it->ref.mapped_span.data();
        for (const Pending& pending : it->pending) {
            const u8* const host = mapped + pending.readback_offset;
            if (pending.kind == Kind::Inspect) {
                ResolveInspect(pending, host);
                continue;
            }
            if (pending.kind == Kind::Dump) {
                const u64 at = AppendBin(host, pending.size);
                Line(fmt::format("D f={} n={} r={} bin={} size={} h={:016x} {}", pending.frame,
                                 pending.call, pending.role, at, pending.size,
                                 Common::CityHash64(reinterpret_cast<const char*>(host),
                                                    pending.size),
                                 pending.extra));
                continue;
            }
            if (pending.kind == Kind::Probe) {
                std::string occlusion = "-";
                if (pending.stride & 1) {
                    u64 samples;
                    std::memcpy(&samples, host, sizeof(samples));
                    occlusion = fmt::format("{}", samples);
                }
                std::string stats = "ia=- vsi=- ci=- cp=- psi=-";
                if (pending.stride & 2) {
                    D3D12_QUERY_DATA_PIPELINE_STATISTICS data;
                    std::memcpy(&data, host + PROBE_STATS_OFFSET, sizeof(data));
                    stats = fmt::format("ia={} vsi={} ci={} cp={} psi={}", data.IAPrimitives,
                                        data.VSInvocations, data.CInvocations, data.CPrimitives,
                                        data.PSInvocations);
                }
                Line(fmt::format("P f={} n={} occ={} {} og={}", pending.frame, pending.call,
                                 occlusion, stats, (pending.stride & 4) ? 1 : 0));
                continue;
            }
            if (pending.kind == Kind::Words) {
                std::string values;
                for (u64 at = 0; at + 4 <= pending.size; at += pending.stride) {
                    u32 word;
                    std::memcpy(&word, host + at, 4);
                    values += fmt::format("{}{}", values.empty() ? "" : ",", word);
                }
                Line(fmt::format("T f={} n={} r={} words={} {}", pending.frame, pending.call,
                                 pending.role, values, pending.extra));
                continue;
            }
            const size_t size = pending.guest.size();
            u32 differ = 0;
            size_t first = SIZE_MAX;
            size_t last = 0;
            for (size_t at = 0; at + 4 <= size; at += 4) {
                if (std::memcmp(host + at, pending.guest.data() + at, 4) != 0) {
                    ++differ;
                    first = std::min(first, at);
                    last = at;
                }
            }
            if (differ == 0) {
                continue;
            }
            ++mismatches;
            u32 host_word = 0;
            u32 guest_word = 0;
            std::memcpy(&host_word, host + first, 4);
            std::memcpy(&guest_word, pending.guest.data() + first, 4);
            float host_float;
            float guest_float;
            std::memcpy(&host_float, &host_word, 4);
            std::memcpy(&guest_float, &guest_word, 4);
            Line(fmt::format("M f={} n={} r={} words={}/{} first={} last={} host={:08x}({:.6g}) "
                             "guest={:08x}({:.6g})",
                             pending.frame, pending.call, pending.role, differ, size / 4, first,
                             last, host_word, host_float, guest_word, guest_float));
        }
        staging.FreeDeferred(it->ref);
        it = in_flight.erase(it);
    }
}

void GlitchHunt::Flush() {
    if (text.empty() || path.empty()) {
        return;
    }
    std::ofstream file{std::filesystem::path{path}, std::ios::app | std::ios::binary};
    file << text;
    text.clear();
}

void GlitchHunt::FrameEnd(double interval_ms, std::string_view summary) {
    switch (state) {
    case State::Idle:
        if (const u32 frames = glitch_requested.exchange(0, std::memory_order_relaxed)) {
            frames_left = frames;
            warmup_left = WARMUP_FRAMES;
            state = State::Armed;
            LOG_INFO(Render, "D3D12 glitch hunt: armed, {} frames after {} warm-up frames", frames,
                     WARMUP_FRAMES);
        }
        return;
    case State::Armed:
        if (--warmup_left == 0) {
            Begin();
            state = State::Capturing;
        }
        return;
    case State::Capturing: {
        Line(fmt::format("F f={} ms={:.3f} calls={} compares={} {}", frame, interval_ms, call,
                         current.pending.size(), summary));
        if (current.ref.buffer) {
            in_flight.push_back(std::move(current));
            current = {};
        }
        compared.clear();
        dump_used = 0;
        Resolve(false);
        Flush();
        ++frame;
        call = 0;
        if (--frames_left == 0) {
            state = State::Resolving;
        }
        return;
    }
    case State::Resolving:
        Resolve(false);
        if (in_flight.empty()) {
            Line(fmt::format("R compares={} mismatches={} inspects={} probes={} over_budget={} "
                             "targets={}",
                             compares, mismatches, inspects, probes, skipped_budget,
                             targets_read));
            Flush();
            LOG_INFO(Render,
                     "D3D12 glitch hunt: capture {} written ({} frames, {} compares, {} host/guest "
                     "mismatches, {} over budget): {}",
                     captures, frame, compares, mismatches, skipped_budget, path);
            state = State::Idle;
        } else {
            Flush();
        }
        return;
    }
}

} // namespace D3D12
