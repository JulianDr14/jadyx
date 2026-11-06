// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "common/common_types.h"

/// Statistical CPU profiler that runs inside the process, for where no external one can attach
/// (the Xbox in Developer Mode: no xperf, no PIX for UWP).
///
/// A thread of its own suspends every named thread (Common::SetCurrentThreadName registers them)
/// at a fixed rate, reads its registers and copies its stack, and resumes it. Nothing allocates or
/// locks while a thread is suspended, since it may hold the heap lock or any other: the stack is
/// unwound afterwards, on the copy (RtlVirtualUnwind, with the pointers into the stack moved to
/// the copy). JIT code is unwound through the function table Dynarmic registers.
///
/// Each window it appends to a text file: the CPU time of every thread, and every distinct stack
/// with how often it was seen, running or waiting. Addresses are module+RVA, so the PC resolves
/// them with the build's PDB (tools/xbox/profile-report.py).
namespace Common::SamplingProfiler {

/// Adds the calling thread to the threads that are sampled. Cheap; safe before Start.
void RegisterCurrentThread(std::string_view name);

struct Options {
    /// Samples per second of each running thread. Each sample pauses its thread for some 20-30 us,
    /// so 250 Hz costs a running thread under 1%; blocked threads are sampled 32 times less.
    u32 hz{250};
    /// Seconds per report window.
    u32 window_seconds{20};
    /// File the windows are appended to; truncated by Start.
    std::filesystem::path output;
};

/// Starts sampling. No-op when already running.
void Start(const Options& options);

/// Stops sampling and writes the window in progress.
void Stop();

[[nodiscard]] bool Running();

/// A frame was finished. While the next one is more than 120 ms late, every thread is sampled, with
/// state S, whether it runs or not. Cheap; safe before Start.
void NoteFrame();

/// A JIT block was compiled: the host code at `host` runs the guest code [guest_pc, guest_end).
/// With blocks known, each window also says which guest code the samples in JIT code ran ("guest"
/// lines) and holds the bytes of the hottest blocks ("guestcode"). Any thread.
void NoteJitBlock(const void* host, size_t host_size, u64 guest_pc, u64 guest_end);

/// A JIT's shared memory access thunks are here. A sample in them goes to the block that called
/// them ("guestthunk" lines), through the return address on top of the stack. Any thread.
void NoteJitThunks(const void* host, size_t host_size);

/// Forgets the blocks and thunks, before a new session compiles its own.
void ClearJitBlocks();

/// Reads guest memory for the "guestcode" lines, on the sampler's thread. `code_base` is where the
/// game's code region starts. Clear it (empty reader) after Stop and before the guest goes away.
using GuestCodeReader = std::function<bool(u64 address, void* out, size_t size)>;
void SetGuestCodeReader(GuestCodeReader reader, u64 code_base);

} // namespace Common::SamplingProfiler
