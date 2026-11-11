// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "common/common_types.h"

/// Diagnostic ("geo_hunt=1"): hunts guest writes the buffer cache never heard of, the class of
/// bug behind meshes that deform. Every full 4 KiB page the buffer cache uploads is hashed (16
/// chunks of 256 bytes). When a draw or dispatch binds a buffer again, its pages that the tracker
/// still calls clean (no CPU, cached CPU or GPU modification) are hashed again from guest memory,
/// each page at most once per frame and within a per-frame byte budget. A page that differs on
/// two checks in different frames, with the tracker still clean, was written by a path that
/// skipped the CPU write hook: the GPU keeps drawing the old bytes.
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

[[nodiscard]] inline bool Enabled() {
    return enabled.load(std::memory_order_relaxed);
}

/// Boot: turns the hunt on. Budgets in KiB per frame, 0 keeps the default.
void Configure(bool on, u32 check_budget_kib, u32 upload_budget_kib);
/// Menu: the user saw a deformation now. Writes a numbered mark and a summary to the log.
void Mark();
/// Ends the log. Before guest memory goes away.
void Stop();

/// Shaders of the pipeline about to bind its buffers. GPU thread only.
void SetShaders(u64 vertex_or_compute, u64 fragment, bool compute);
/// The vertex (or compute) shader SetShaders last gave this thread.
[[nodiscard]] u64 CurrentShader();
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
/// Every upload from guest memory, whole pages or not: the audit report's upload history.
void NoteUpload(u64 address, u64 size);
/// True when the page has a baseline, was not checked in the last few frames (so the budget
/// reaches the draws bound late in a frame) and budget remains for the role.
[[nodiscard]] bool Due(u64 page, Role role);
/// The tracker marks the page modified: it will be uploaded again, any suspicion is void.
void Dirty(u64 page);
/// Compares the page's guest bytes against its baseline.
[[nodiscard]] Result Check(u64 page, const u8* guest, Role role);
/// Logs the confirmed missed write Check just returned.
void Report(u64 page, Role role, const Protection& protection);

// ---- Out-of-range indices (on in every build, not only with geo_hunt) ----
// An index that points past the vertex buffers makes D3D12 fetch zeros: the triangle stretches
// to the model origin (big flat spikes). The buffer cache checks each index range against the
// draw's vertex streams; with the fix on, triangles using such indices are dropped.

inline std::atomic_bool oob_check{true};
inline std::atomic_bool oob_fix{true};

[[nodiscard]] inline bool OobCheck() {
    return oob_check.load(std::memory_order_relaxed);
}
[[nodiscard]] inline bool OobFix() {
    return oob_fix.load(std::memory_order_relaxed);
}
/// Boot and menu: check / drop out-of-range triangles.
void ConfigureOob(bool check, bool fix);
/// Buffer cache, every frame: the frame number the journal and the reports carry.
void NoteFrame(u64 frame);
void SetOobFix(bool on);

// ---- Vertex resync (diagnostic: "vb_resync=1" and the geo_hunt menu) ----
// A vertex stream the buffer cache holds as GPU-written skips the upload of the guest's bytes.
// When on, the GPU-written bytes a transform feedback of an earlier frame (none this frame) left
// there are dropped and uploaded from guest memory.
inline std::atomic_bool vb_resync{false};

[[nodiscard]] inline bool VbResync() {
    return vb_resync.load(std::memory_order_relaxed);
}
void SetVbResync(bool on);
/// geo_hunt: a transform feedback output bound for a draw. Logged once per range and shader
/// (again after a while), so the log tells which shader wrote which range in which frame.
void NoteFeedbackWrite(u64 address, u64 size, u32 index, u32 stride, u32 varyings, u64 shader,
                       u64 frame);
struct StaleFeedback {
    u64 vertex_address;   ///< bound vertex range
    u64 vertex_size;
    u64 lo;               ///< GPU-written bytes in it left by an older transform feedback
    u64 hi;
    u64 feedback_address; ///< that transform feedback binding
    u64 feedback_size;
    u64 feedback_frame;   ///< frame it was last bound
    u64 feedback_shader;
    u64 frame;            ///< now
    u64 vertex_shader;
    u64 origin;           ///< last GPU writer of [lo, hi) (seq << 8 | WriteOrigin), 0 unknown
    bool resynced;        ///< VbResync: dropped and uploaded from guest memory
};
/// Logs a vertex stream reading bytes an older frame's transform feedback wrote, once per range.
void NoteStaleFeedback(const StaleFeedback& stale);

/// Who wrote guest memory from the emulator side, for the journal.
enum class Writer : u8 {
    GpuMemory,        ///< MemoryManager writes: inline-to-memory, DMA, const buffer updates
    BufferDownload,   ///< buffer cache download landing (GPU-written bytes)
    BufferImmediate,  ///< buffer cache immediate download
    BufferCheckpoint, ///< Pulse checkpoint of GPU-modified buffer ranges
    TextureDownload,  ///< texture cache flush of a GPU-written image
    TextureCheckpoint,///< Pulse checkpoint of GPU-modified images
    Query,            ///< query/semaphore results
};

/// Tags the MemoryManager writes of this thread while alive.
class WriterScope {
public:
    explicit WriterScope(Writer writer);
    ~WriterScope();
    WriterScope(const WriterScope&) = delete;
    WriterScope& operator=(const WriterScope&) = delete;

private:
    Writer previous;
};

void RecordWrite(u64 address, u64 size);
void RecordWrite(Writer writer, u64 address, u64 size);
/// Records an emulator write to guest memory (device address). Diagnostic: only with geo_hunt,
/// an inline flag test otherwise (the out-of-range fix does not need it).
inline void Journal(u64 address, u64 size) {
    if (Enabled()) [[unlikely]] {
        RecordWrite(address, size);
    }
}
inline void Journal(Writer writer, u64 address, u64 size) {
    if (Enabled()) [[unlikely]] {
        RecordWrite(writer, address, size);
    }
}

struct OobDraw {
    u64 index_address;   ///< device address of the first index of the draw
    u64 vertex_shader;
    u32 count;           ///< indices
    u32 index_size;      ///< bytes
    u32 max_index;       ///< largest index (restart excluded)
    u32 bad;             ///< indices at or above limit
    u32 first_bad;       ///< position of the first one
    u32 first_bad_value;
    s64 base_vertex;
    u32 vertex_limit;    ///< vertices the largest per-vertex stream holds
    u32 topology;
    bool fixed;
};
/// Logs the first out-of-range draw of an index range, with the journal of its pages.
void ReportOob(const OobDraw& draw);

// ---- Host audit (geo_hunt): what the GPU really holds for a bound index/vertex range ----
// Right after a range is synchronized for a draw, its bytes are copied back from the GPU buffer
// and compared, once the copy is done, with the guest bytes read at the same moment. GPU-written
// ranges and pages with pending CPU writes are left out: everything else must match. A mismatch
// is a page the cache believed up to date and never uploaded.

/// Per-frame audit budget in KiB (0 keeps the default) and how many frames a range rests.
void ConfigureAudit(u32 budget_kib);
/// Takes size bytes of this frame's audit budget; false when it is used up.
[[nodiscard]] bool AuditTake(u64 address, u64 size);

struct AuditPage {
    u64 page;          ///< device address of the 4 KiB page
    u32 diff_bytes;    ///< bytes that differ inside the audited part of the page
    u32 audited_bytes;
    u32 first_diff;    ///< offset in the page
    bool host_zero;    ///< every differing host byte is zero (never written on the GPU)
    bool guest_moved;  ///< guest memory changed after the bind: a write in flight, not proof
    bool cpu_now;      ///< the tracker marks it CPU modified by now
    bool gpu_now;      ///< or GPU modified
};
struct AuditReport {
    Role role;
    u64 bind_frame;
    u64 address;       ///< bound range
    u64 size;
    u64 buffer_address;///< cache buffer holding it
    u64 buffer_size;
    u64 buffer_frame;  ///< frame the cache buffer was created
    u32 buffer_id;
    u32 excluded_gpu;  ///< bytes left out as GPU-written
    u32 excluded_cpu;  ///< bytes left out as pending CPU writes
    u64 vertex_shader;
};
/// Logs a host copy that differs from guest memory, with the upload history and the journal of
/// each page. host and guest point at the first difference, bytes of them readable.
void ReportHostStale(const AuditReport& report, const std::vector<AuditPage>& pages,
                     const u8* host, const u8* guest, u64 bytes);

// ---- Command stream guard (geo_hunt) ----
/// DMA pusher: the command segment about to be parsed (GPU VA, its device address or 0, bytes).
void NoteCommandSegment(u64 gpu_va, u64 device_addr, u64 size);
/// An engine got a method past its registers: the command stream is garbage. Logs the segment
/// being parsed, its journal and, after the segment, its words.
void CommandGarbage(const char* engine, u32 method, u32 argument);
/// DMA pusher, after parsing a segment: true once when CommandGarbage fired inside it.
[[nodiscard]] bool TakeGarbageDump();
/// Dumps the words of the segment that held garbage.
void DumpCommandWords(const u32* words, size_t count);

/// Container census: true once after CommandGarbage fired, so the next frame logs one.
[[nodiscard]] bool TakeCensusRequest();
/// Logs "CENSUS" with the caller's counts (buffer cache ranges) plus GeoHunt's own map sizes.
void NoteCensus(u64 frame, const std::string& counts);

} // namespace VideoCommon::GeoHunt
