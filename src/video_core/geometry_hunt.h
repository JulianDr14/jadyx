// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <mutex>

#include "common/common_types.h"

/// Diagnostic ("geo_hunt=1"): hunts guest writes the buffer cache never heard of, the class of
/// bug behind meshes that deform. Every full 4 KiB page the buffer cache uploads is hashed (16
/// chunks of 256 bytes). When a draw or dispatch binds a buffer again, its pages that the tracker
/// still calls clean (no CPU, cached CPU or GPU modification) are hashed again from guest memory,
/// each page at most once per frame and within a per-frame byte budget. A page that differs on
/// two checks in different frames, with the tracker still clean, was written by a path that
/// skipped the CPU write hook: the GPU keeps drawing the old bytes. Heal mode marks such a page
/// CPU modified so the next bind uploads it, which tells whether that was the deformation.
/// Output: LogDir/geometry/geo_<date>_<time>.txt, read on the PC.
namespace VideoCommon::GeoHunt {

constexpr u64 PAGE_BITS = 12;
constexpr u64 PAGE_SIZE = u64{1} << PAGE_BITS;

/// What the buffer cache was binding when a page was uploaded or checked.
enum class Role : u8 {
    Other,
    Index,
    Vertex,
    Uniform,
    Storage,
    Texel,
    ComputeUniform,
    ComputeStorage,
    ComputeTexel,
    Indirect,
    Feedback,
};

enum class Result : u8 {
    Clean,     ///< guest still holds what was uploaded
    Suspect,   ///< first difference: a CPU write may still be on its way to the hook
    Confirmed, ///< differs again in a later frame with the tracker still clean
};

/// How the device page was protected when the missed write was confirmed.
struct Protection {
    u64 cpu_page;
    u32 asid;
    u32 cached_count;
    u32 aliases;
    u32 alias_cached;
    bool cpu_cached;
};

inline std::atomic_bool enabled{};
inline std::atomic_bool heal{};

[[nodiscard]] inline bool Enabled() {
    return enabled.load(std::memory_order_relaxed);
}
[[nodiscard]] inline bool Healing() {
    return heal.load(std::memory_order_relaxed);
}

/// Boot: turns the hunt on. Budgets in KiB per frame, 0 keeps the default.
void Configure(bool on, u32 check_budget_kib, u32 upload_budget_kib, bool heal_on);
/// Menu: heal mode on or off at run time.
void SetHeal(bool on);
/// Menu: the user saw a deformation now. Writes a numbered mark and a summary to the log.
void Mark();
/// Ends the log. Before guest memory goes away.
void Stop();

/// Shaders of the pipeline about to bind its buffers. GPU thread only.
void SetShaders(u64 vertex_or_compute, u64 fragment, bool compute);
/// Buffer cache, GPU thread: the frame advanced.
void BeginFrame(u64 frame);
/// Buffer cache: guest memory in the range was written from host data (download landing).
void Forget(u64 address, u64 size);
/// Index and vertex pages: checked from their own budget, so uniform and storage rebinds
/// cannot use it all up before the meshes are bound.
[[nodiscard]] constexpr bool IsGeometry(Role role) {
    return role == Role::Index || role == Role::Vertex;
}
/// Budget left for checks of this role this frame; a cheap early out without the lock.
[[nodiscard]] bool CheckBudgetLeft(Role role);

/// The calls below need this lock held; the buffer cache takes it once per buffer range.
[[nodiscard]] std::unique_lock<std::mutex> Lock();
/// False when this exact range was already verified this frame: draws rebind the same ranges.
[[nodiscard]] bool FirstThisFrame(u64 address, u64 size);
/// The whole page was just uploaded from guest memory; guest points at its bytes.
void Baseline(u64 page, const u8* guest, Role role);
/// The page is only partly uploaded or its bytes are not readable: no baseline.
void Drop(u64 page);
/// True when the page has a baseline, was not checked in the last few frames (so the budget
/// reaches the draws bound late in a frame) and budget remains for the role.
[[nodiscard]] bool Due(u64 page, Role role);
/// The tracker marks the page modified: it will be uploaded again, any suspicion is void.
void Dirty(u64 page);
/// Compares the page's guest bytes against its baseline.
[[nodiscard]] Result Check(u64 page, const u8* guest, Role role);
/// Logs the confirmed missed write Check just returned. healed: marked CPU modified.
void Report(u64 page, Role role, const Protection& protection, bool healed);

} // namespace VideoCommon::GeoHunt
