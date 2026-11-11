// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/diagnostics/full_capture.h"

namespace VideoCommon::ReadbackTrace {

/// Diagnostic: data the GPU hands back to guest memory (first 400 events, then each 128th), with
/// how much of it is non-zero, to tell a stale or empty download from a real one.
inline void Log(const char* kind, u64 address, u64 size, std::span<const u8> data,
                u32 format = 0, u32 width = 0, u32 height = 0) {
    if (FullCapture::Active()) {
        FullCapture::Data("guest_readback", fmt::format(
            "{{\"route\":\"{}\",\"address\":{},\"requested_size\":{},\"format\":{},"
            "\"width\":{},\"height\":{}}}", kind, address, size, format, width, height), data);
        if (data.size() < size) FullCapture::Omit("short_guest_readback");
    }
    static std::atomic<u64> events{};
    const u64 count = ++events;
    if (count > 400 && (count & 127) != 0) {
        return;
    }
    const std::span<const u8> checked = data.first((std::min)(data.size(), size_t{1} << 20));
    u64 nonzero{};
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u8 byte : checked) {
        nonzero += byte != 0;
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    u64 head{};
    for (size_t i = 0; i < (std::min)(checked.size(), size_t{8}); ++i) {
        head |= u64{checked[i]} << (8 * i);
    }
    LOG_WARNING(Render,
                "Readback #{} {}: addr {:#x} size {} fmt {} {}x{} | nonzero {}/{} hash {:016x} "
                "head {:016x}",
                count, kind, address, size, format, width, height, nonzero, checked.size(), hash,
                head);
}

/// Diagnostic: a CPU read that had to wait for a GPU flush (first 400, then each 128th).
inline void LogCpuReadFlush(u64 address, u64 size, u64 start, u64 end) {
    if (FullCapture::Active()) FullCapture::Event("cpu_read_flush", fmt::format(
        "{{\"address\":{},\"size\":{},\"start\":{},\"end\":{}}}", address, size, start, end));
    static std::atomic<u64> events{};
    const u64 count = ++events;
    if (count > 400 && (count & 127) != 0) {
        return;
    }
    LOG_WARNING(Render, "CPU read flush #{}: addr {:#x} size {} area {:#x}-{:#x}", count, address,
                size, start, end);
}

/// Path that marked a buffer range as written by the GPU (buffer_write_origins=1).
enum class WriteOrigin : u8 {
    None,
    MultiRange,
    GraphicsStorage,
    GraphicsTexture,
    TransformFeedback,
    ComputeStorage,
    ComputeTexture,
    DmaImage,
    DmaCopy,
};

inline std::atomic_bool write_origins_enabled{};
inline void SetWriteOrigins(bool enabled) { write_origins_enabled = enabled; }
inline bool WriteOriginsEnabled() { return write_origins_enabled.load(std::memory_order_relaxed); }

/// Diagnostic: guest bytes the CPU wrote that lose against a GPU-written range, either skipped
/// by an upload or overwritten by a download. Each range start is reported once (up to 4096).
inline void LogWriteConflict(const char* kind, u64 address, u64 size, u64 origin, u64 origin_seq,
                             u64 current_seq) {
    static constexpr std::array<const char*, 9> names{
        "none", "multi_range", "graphics_storage", "graphics_texture", "transform_feedback",
        "compute_storage", "compute_texture", "dma_image", "dma_copy"};
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    {
        std::scoped_lock lock{mutex};
        if (seen.size() >= 4096 || !seen.insert(address).second) return;
    }
    const char* name = origin < names.size() ? names[origin] : "unknown";
    if (FullCapture::Active()) FullCapture::Event("buffer_write_conflict", fmt::format(
        "{{\"kind\":\"{}\",\"address\":{},\"size\":{},\"origin\":\"{}\",\"origin_seq\":{},"
        "\"current_seq\":{}}}", kind, address, size, name, origin_seq, current_seq));
    LOG_WARNING(Render, "Buffer write conflict, {}: addr {:#x} size {} | last GPU write by {} "
                "(mark #{} of {})", kind, address, size, name, origin_seq, current_seq);
}

/// Glitch hunt: a gathered CPU write that took bytes the GPU had written out of the buffer
/// cache's GPU ranges, so the next upload sends the guest's bytes over them.
struct GpuByteLoss {
    u64 address;
    u64 size;
    u64 gpu_bytes; ///< bytes of the write the GPU had written
    bool kept;     ///< precise_writes left them in the GPU ranges
};
inline std::atomic_bool gpu_loss_recording{};
inline std::mutex gpu_loss_mutex;
inline std::vector<GpuByteLoss> gpu_losses;
/// Recorded only while a glitch capture runs (GPU thread sets it).
inline void SetGpuLossRecording(bool enabled) {
    gpu_loss_recording.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        std::scoped_lock lock{gpu_loss_mutex};
        gpu_losses.clear();
    }
}
inline bool GpuLossRecording() { return gpu_loss_recording.load(std::memory_order_relaxed); }
inline void RecordGpuLoss(u64 address, u64 size, u64 gpu_bytes, bool kept) {
    std::scoped_lock lock{gpu_loss_mutex};
    if (gpu_losses.size() < 8192) gpu_losses.push_back({address, size, gpu_bytes, kept});
}
inline std::vector<GpuByteLoss> TakeGpuLosses() {
    std::vector<GpuByteLoss> taken;
    std::scoped_lock lock{gpu_loss_mutex};
    taken.swap(gpu_losses);
    return taken;
}

/// On by default ("precise_writes=0" or the glitch hunt page turns it off): a CPU write into a
/// page the GPU wrote, but over none of the bytes the GPU wrote, is applied at once instead of
/// waiting for the next cache invalidation. Without it, data the guest writes next to GPU output
/// reaches a dispatch stale.
inline std::atomic_bool precise_cpu_writes{true};
inline void SetPreciseCpuWrites(bool enabled) { precise_cpu_writes.store(enabled, std::memory_order_relaxed); }
inline bool PreciseCpuWrites() { return precise_cpu_writes.load(std::memory_order_relaxed); }

} // namespace VideoCommon::ReadbackTrace
