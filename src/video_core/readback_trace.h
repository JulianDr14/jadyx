// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <span>

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

} // namespace VideoCommon::ReadbackTrace
