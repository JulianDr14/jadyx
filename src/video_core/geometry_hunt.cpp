// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/geometry_hunt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace VideoCommon::GeoHunt {
namespace {

constexpr size_t CHUNKS = 16;
constexpr size_t CHUNK_SIZE = PAGE_SIZE / CHUNKS;
/// About 100 bytes each: 128 Ki pages (512 MiB of buffers) cost ~13 MiB at most.
constexpr size_t MAX_PAGES = 128 * 1024;
constexpr u32 DEFAULT_CHECK_KIB = 4 * 1024;
constexpr u32 DEFAULT_UPLOAD_KIB = 32 * 1024;
/// Index and vertex pages get this many times the check budget, on their own.
constexpr u64 GEOMETRY_BUDGET_SCALE = 2;
/// A checked page rests this many frames: the same early ranges no longer take the whole
/// budget every frame.
constexpr u32 RECHECK_FRAMES = 4;
constexpr u64 STATS_FRAMES = 600;
constexpr u32 MAIN_LOG_REPORTS = 64;
constexpr size_t RECENT = 48;

using Chunks = std::array<u32, CHUNKS>;

enum EntryFlags : u8 {
    FLAG_SUSPECT = 1, ///< differed once, waiting for a later frame
    FLAG_STALE = 2,   ///< confirmed and not healed: the GPU copy is behind guest memory
};

struct Entry {
    Chunks chunks;
    u32 upload_frame;
    u32 checked_frame;
    u32 suspect_frame;
    u32 events;
    u16 mask;
    Role upload_role;
    u8 flags;
};

struct Stats {
    u64 baselines;
    u64 baseline_kib;
    u64 dropped_full;
    u64 dropped_budget;
    u64 checks;
    u64 dirty_skips;
    u64 suspects;
    u64 suspects_cleared;
    u64 confirmed;
    u64 budget_frames;
    u64 geometry_checks;
    u64 geometry_budget_frames;
    u64 audits;
    u64 audit_kib;
    u64 audit_stale;
};

struct Recent {
    u64 page;
    u64 frame;
    u64 time_ms;
    u16 mask;
    Role role;
};

std::mutex mutex;
std::unordered_map<u64, Entry> pages;
std::unordered_set<u64> ranges_this_frame;
std::vector<Recent> recent;
std::FILE* file{};
Stats stats{};
Stats window{};
u64 frame{};
u64 last_stats_frame{};
u64 check_budget{u64{DEFAULT_CHECK_KIB} * 1024};
u64 upload_budget{u64{DEFAULT_UPLOAD_KIB} * 1024};
u64 upload_left{};
constexpr u32 DEFAULT_AUDIT_KIB = 2 * 1024;
/// A range audited clean rests this many frames before its next audit.
constexpr u64 AUDIT_REST_FRAMES = 30;
u64 audit_budget{u64{DEFAULT_AUDIT_KIB} * 1024};
u64 audit_left{u64{DEFAULT_AUDIT_KIB} * 1024};
std::unordered_map<u64, u64> audited_ranges;
struct UploadNote {
    u32 frame;
    u32 count;
    u32 first_frame;
};
/// Every upload into a page (whole or part), for the audit report.
std::unordered_map<u64, UploadNote> uploads;
std::atomic<u64> check_left{};
std::atomic<u64> geometry_left{};
bool budget_hit{};
bool geometry_budget_hit{};
u32 marks{};
u32 main_log_reports{};
const auto start_time = std::chrono::steady_clock::now();

thread_local u64 shader_a{};
thread_local u64 shader_b{};
thread_local bool shader_compute{};

[[nodiscard]] u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time)
                                .count());
}

const char* RoleName(Role role) {
    switch (role) {
    case Role::Other:
        return "other";
    case Role::Index:
        return "index";
    case Role::Vertex:
        return "vertex";
    case Role::Uniform:
        return "uniform";
    case Role::Storage:
        return "storage";
    case Role::Texel:
        return "texel";
    case Role::ComputeUniform:
        return "cs-uniform";
    case Role::ComputeStorage:
        return "cs-storage";
    case Role::ComputeTexel:
        return "cs-texel";
    case Role::Indirect:
        return "indirect";
    case Role::Feedback:
        return "feedback";
    }
    return "?";
}

void HashPage(const u8* guest, Chunks& out) {
    for (size_t i = 0; i < CHUNKS; ++i) {
        out[i] = static_cast<u32>(
            Common::CityHash64(reinterpret_cast<const char*>(guest + i * CHUNK_SIZE), CHUNK_SIZE));
    }
}

/// Needs the lock.
std::FILE* Log() {
    if (file) {
        return file;
    }
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "geometry";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    const std::string path = (dir / fmt::format("geo_{}.txt", stamp)).string();
    fopen_s(&file, path.c_str(), "wb");
    if (file) {
        fmt::print(file,
                   "geo_hunt: check budget {} KiB/frame, upload budget {} KiB/frame\n"
                   "MISS lines: page = device address, chunks = 256-byte pieces that changed "
                   "(bit 0 = first), cached = this device page's cached count, cpu_cached = CPU "
                   "page flag, aliases = device pages on the same memory (cached ones)\n",
                   check_budget / 1024, upload_budget / 1024);
        LOG_INFO(HW_GPU, "geo_hunt: log at {}", path);
    }
    return file;
}

/// Needs the lock.
void WriteStats(const char* label, const Stats& s) {
    std::FILE* const out = Log();
    if (!out) {
        return;
    }
    fmt::print(out,
               "{} f{} t{} ms: tracked {} pages, baselines {} ({} KiB), dropped full {} budget {}, "
               "checks {} ({} KiB), dirty skips {}, suspects {} (cleared by tracker {}), "
               "confirmed {}, frames out of budget {} (index/vertex checks {}, "
               "frames out of their budget {}), host audits {} ({} KiB) stale {}\n",
               label, frame, NowMs(), pages.size(), s.baselines, s.baseline_kib, s.dropped_full,
               s.dropped_budget, s.checks, s.checks * PAGE_SIZE / 1024, s.dirty_skips, s.suspects,
               s.suspects_cleared, s.confirmed, s.budget_frames, s.geometry_checks,
               s.geometry_budget_frames, s.audits, s.audit_kib, s.audit_stale);
    std::fflush(out);
}

void Add(Stats& total, const Stats& part) {
    total.baselines += part.baselines;
    total.baseline_kib += part.baseline_kib;
    total.dropped_full += part.dropped_full;
    total.dropped_budget += part.dropped_budget;
    total.checks += part.checks;
    total.dirty_skips += part.dirty_skips;
    total.suspects += part.suspects;
    total.suspects_cleared += part.suspects_cleared;
    total.confirmed += part.confirmed;
    total.budget_frames += part.budget_frames;
    total.geometry_checks += part.geometry_checks;
    total.geometry_budget_frames += part.geometry_budget_frames;
    total.audits += part.audits;
    total.audit_kib += part.audit_kib;
    total.audit_stale += part.audit_stale;
}

} // Anonymous namespace

void Configure(bool on, u32 check_budget_kib, u32 upload_budget_kib) {
    std::scoped_lock lock{mutex};
    check_budget = u64{check_budget_kib != 0 ? check_budget_kib : DEFAULT_CHECK_KIB} * 1024;
    upload_budget = u64{upload_budget_kib != 0 ? upload_budget_kib : DEFAULT_UPLOAD_KIB} * 1024;
    check_left.store(check_budget, std::memory_order_relaxed);
    geometry_left.store(check_budget * GEOMETRY_BUDGET_SCALE, std::memory_order_relaxed);
    upload_left = upload_budget;
    if (on) {
        pages.reserve(MAX_PAGES / 4);
        LOG_INFO(HW_GPU,
                 "geo_hunt on: check {} KiB/frame (+{} KiB for index/vertex), upload {} "
                 "KiB/frame, recheck every {} frames",
                 check_budget / 1024, check_budget * GEOMETRY_BUDGET_SCALE / 1024,
                 upload_budget / 1024, RECHECK_FRAMES);
    }
    enabled.store(on, std::memory_order_relaxed);
}

void Mark() {
    std::scoped_lock lock{mutex};
    ++marks;
    Stats total = stats;
    Add(total, window);
    std::FILE* const out = Log();
    if (out) {
        fmt::print(out, "\n==== MARK {} f{} t{} ms (user saw a deformation) ====\n", marks,
                   frame, NowMs());
    }
    WriteStats("since start", total);
    if (out) {
        const u64 now = NowMs();
        fmt::print(out, "last {} confirmed misses (newest last):\n", recent.size());
        for (const Recent& miss : recent) {
            fmt::print(out, "  page {:#x} {} chunks {:016b} f{} ({} ms before the mark)\n",
                       miss.page, RoleName(miss.role), miss.mask, miss.frame,
                       now - miss.time_ms);
        }
        fmt::print(out, "==== END MARK {} ====\n\n", marks);
        std::fflush(out);
    }
    LOG_WARNING(HW_GPU, "geo_hunt: mark {} at frame {}, {} confirmed misses so far", marks,
                frame, total.confirmed);
}

void Stop() {
    std::scoped_lock lock{mutex};
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    enabled.store(false, std::memory_order_relaxed);
    Stats total = stats;
    Add(total, window);
    WriteStats("final", total);
    if (file) {
        std::fclose(file);
        file = nullptr;
    }
    pages.clear();
    ranges_this_frame.clear();
}

void SetShaders(u64 vertex_or_compute, u64 fragment, bool compute) {
    shader_a = vertex_or_compute;
    shader_b = fragment;
    shader_compute = compute;
}

u64 CurrentShader() {
    return shader_a;
}

void BeginFrame(u64 new_frame) {
    std::scoped_lock lock{mutex};
    frame = new_frame;
    if (budget_hit) {
        ++window.budget_frames;
        budget_hit = false;
    }
    if (geometry_budget_hit) {
        ++window.geometry_budget_frames;
        geometry_budget_hit = false;
    }
    check_left.store(check_budget, std::memory_order_relaxed);
    geometry_left.store(check_budget * GEOMETRY_BUDGET_SCALE, std::memory_order_relaxed);
    upload_left = upload_budget;
    audit_left = audit_budget;
    ranges_this_frame.clear();
    if (frame - last_stats_frame >= STATS_FRAMES) {
        WriteStats("window", window);
        Add(stats, window);
        window = {};
        last_stats_frame = frame;
    }
}

void Forget(u64 address, u64 size) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    const u64 first = address >> PAGE_BITS;
    const u64 last = (address + size - 1) >> PAGE_BITS;
    for (u64 page = first; page <= last; ++page) {
        pages.erase(page);
    }
}

bool CheckBudgetLeft(Role role) {
    const auto& left = IsGeometry(role) ? geometry_left : check_left;
    return left.load(std::memory_order_relaxed) >= PAGE_SIZE;
}

std::unique_lock<std::mutex> Lock() {
    return std::unique_lock{mutex};
}

bool FirstThisFrame(u64 address, u64 size) {
    return ranges_this_frame.insert(address ^ (size << 40) ^ (size >> 24)).second;
}

void Baseline(u64 page, const u8* guest, Role role) {
    const u64 key = page >> PAGE_BITS;
    if (upload_left < PAGE_SIZE) {
        // An old baseline would no longer match what the GPU holds.
        pages.erase(key);
        ++window.dropped_budget;
        return;
    }
    auto it = pages.find(key);
    if (it == pages.end()) {
        if (pages.size() >= MAX_PAGES) {
            ++window.dropped_full;
            return;
        }
        it = pages.emplace(key, Entry{}).first;
    }
    upload_left -= PAGE_SIZE;
    Entry& entry = it->second;
    HashPage(guest, entry.chunks);
    entry.upload_frame = static_cast<u32>(frame);
    entry.checked_frame = static_cast<u32>(frame);
    entry.flags = 0;
    entry.mask = 0;
    entry.upload_role = role;
    ++window.baselines;
    window.baseline_kib += PAGE_SIZE / 1024;
}

void Drop(u64 page) {
    pages.erase(page >> PAGE_BITS);
}

bool Due(u64 page, Role role) {
    const auto it = pages.find(page >> PAGE_BITS);
    if (it == pages.end()) {
        return false;
    }
    const Entry& entry = it->second;
    // A suspect waits for the next frame only; a clean page rests a few frames.
    const u32 rest = (entry.flags & FLAG_SUSPECT) != 0 ? 1 : RECHECK_FRAMES;
    if (static_cast<u32>(frame) - entry.checked_frame < rest) {
        return false;
    }
    const bool geometry = IsGeometry(role);
    if ((geometry ? geometry_left : check_left).load(std::memory_order_relaxed) < PAGE_SIZE) {
        (geometry ? geometry_budget_hit : budget_hit) = true;
        return false;
    }
    return true;
}

void Dirty(u64 page) {
    const auto it = pages.find(page >> PAGE_BITS);
    if (it == pages.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.checked_frame = static_cast<u32>(frame);
    ++window.dirty_skips;
    if ((entry.flags & FLAG_SUSPECT) != 0) {
        entry.flags &= ~FLAG_SUSPECT;
        ++window.suspects_cleared;
    }
}

Result Check(u64 page, const u8* guest, Role role) {
    Entry& entry = pages.find(page >> PAGE_BITS)->second;
    if (IsGeometry(role)) {
        geometry_left.fetch_sub(PAGE_SIZE, std::memory_order_relaxed);
        ++window.geometry_checks;
    } else {
        check_left.fetch_sub(PAGE_SIZE, std::memory_order_relaxed);
    }
    entry.checked_frame = static_cast<u32>(frame);
    ++window.checks;
    Chunks now;
    HashPage(guest, now);
    u16 mask = 0;
    for (size_t i = 0; i < CHUNKS; ++i) {
        if (now[i] != entry.chunks[i]) {
            mask |= static_cast<u16>(1U << i);
        }
    }
    if (mask == 0) {
        entry.flags &= ~FLAG_SUSPECT;
        return Result::Clean;
    }
    if ((entry.flags & FLAG_SUSPECT) == 0) {
        entry.flags |= FLAG_SUSPECT;
        entry.suspect_frame = static_cast<u32>(frame);
        entry.mask = mask;
        ++window.suspects;
        return Result::Suspect;
    }
    entry.mask |= mask;
    entry.flags &= ~FLAG_SUSPECT;
    ++entry.events;
    ++window.confirmed;
    // The GPU copy stays behind; the new bytes become the baseline so a later missed write
    // shows as its own event.
    entry.chunks = now;
    entry.flags |= FLAG_STALE;
    return Result::Confirmed;
}

void Report(u64 page, Role role, const Protection& protection) {
    const auto it = pages.find(page >> PAGE_BITS);
    if (it == pages.end()) {
        return;
    }
    Entry& entry = it->second;
    const u64 now = NowMs();
    if (recent.size() == RECENT) {
        recent.erase(recent.begin());
    }
    recent.push_back({page, frame, now, entry.mask, role});
    // Pages that keep missing are logged on their first events and then every 64th.
    if (entry.events > 8 && (entry.events % 64) != 0) {
        entry.mask = 0;
        return;
    }
    if (std::FILE* const out = Log()) {
        fmt::print(out,
                   "f{} t{} ms MISS page {:#x} role {} (uploaded as {} at f{}, suspect f{}) "
                   "chunks {:016b} event {} {} {:016x} {:016x} | cpu {:#x} asid {} cached {} "
                   "cpu_cached {} aliases {} ({} cached)\n",
                   frame, now, page, RoleName(role), RoleName(entry.upload_role),
                   entry.upload_frame, entry.suspect_frame, entry.mask, entry.events,
                   shader_compute ? "cs" : "vs/fs", shader_a, shader_b, protection.cpu_page,
                   protection.asid, protection.cached_count, protection.cpu_cached ? 1 : 0,
                   protection.aliases, protection.alias_cached);
        std::fflush(out);
    }
    if (main_log_reports < MAIN_LOG_REPORTS) {
        ++main_log_reports;
        LOG_WARNING(HW_GPU,
                    "geo_hunt: missed write page {:#x} role {} chunks {:016b} cached {} "
                    "cpu_cached {} aliases {}/{}",
                    page, RoleName(role), entry.mask, protection.cached_count,
                    protection.cpu_cached ? 1 : 0, protection.alias_cached, protection.aliases);
    }
    entry.mask = 0;
}

namespace {

struct JournalWrite {
    u64 address;
    u64 time_ms;
    u32 size;
    u32 frame;
    Writer writer;
};
/// The two newest emulator writes of each page, newest first.
using JournalPage = std::array<JournalWrite, 2>;
/// About 64 bytes each; cleared whole past this many pages.
constexpr size_t JOURNAL_PAGES = 512 * 1024;
constexpr u32 OOB_REPORTS = 64;

std::mutex journal_mutex;
std::unordered_map<u64, JournalPage> journal;
std::unordered_set<u64> oob_reported;
u64 journal_clears{};
u32 oob_reports{};
thread_local Writer current_writer = Writer::GpuMemory;
std::atomic<u64> journal_frame{};

const char* WriterName(Writer writer) {
    switch (writer) {
    case Writer::GpuMemory:
        return "gpu-memory(i2m/dma/cbuf)";
    case Writer::BufferDownload:
        return "buffer-download";
    case Writer::BufferImmediate:
        return "buffer-immediate";
    case Writer::BufferCheckpoint:
        return "pulse-buffer-checkpoint";
    case Writer::TextureDownload:
        return "texture-download";
    case Writer::TextureCheckpoint:
        return "pulse-texture-checkpoint";
    case Writer::Query:
        return "query";
    }
    return "?";
}

} // Anonymous namespace

void NoteFrame(u64 new_frame) {
    journal_frame.store(new_frame, std::memory_order_relaxed);
}

void ConfigureOob(bool check, bool fix) {
    oob_check.store(check, std::memory_order_relaxed);
    oob_fix.store(fix, std::memory_order_relaxed);
    LOG_INFO(HW_GPU, "geo_oob: check {}, fix {}", check, fix);
}

void SetOobFix(bool on) {
    oob_fix.store(on, std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex};
        if (std::FILE* const out = Log()) {
            fmt::print(out, "t{} ms: out-of-range triangle fix {}\n", NowMs(), on ? "on" : "off");
            std::fflush(out);
        }
    }
    LOG_INFO(HW_GPU, "geo_oob: fix {}", on ? "on" : "off");
}

void SetVbResync(bool on) {
    vb_resync.store(on, std::memory_order_relaxed);
    const char* const name = on ? "old feedback" : "off";
    {
        std::scoped_lock lock{mutex};
        if (std::FILE* const out = Log()) {
            fmt::print(out, "t{} ms: vertex resync {}\n", NowMs(), name);
            std::fflush(out);
        }
    }
    LOG_INFO(HW_GPU, "vb_resync: {}", name);
}

void NoteFeedbackWrite(u64 address, u64 size, u32 index, u32 stride, u32 varyings, u64 shader,
                       u64 frame_now) {
    static std::unordered_map<u64, u64> seen;
    static u32 lines{};
    const u64 key = address ^ (size << 40) ^ (shader * 0x9E3779B97F4A7C15ULL);
    std::scoped_lock lock{mutex};
    if (lines >= 4096) {
        return;
    }
    if (seen.size() > 64 * 1024) {
        seen.clear();
    }
    auto [it, inserted] = seen.try_emplace(key, frame_now);
    if (!inserted) {
        if (frame_now - it->second < 600) {
            return;
        }
        it->second = frame_now;
    }
    ++lines;
    if (std::FILE* const out = Log()) {
        fmt::print(out,
                   "f{} t{} ms XFB-WRITE #{} {:#x}+{:#x} (end {:#x}) stride {} varyings {} "
                   "vs {:016x}\n",
                   frame_now, NowMs(), index, address, size, address + size, stride, varyings,
                   shader);
        std::fflush(out);
    }
}

void NoteStaleFeedback(const StaleFeedback& stale) {
    static constexpr std::array<const char*, 9> names{
        "none", "multi_range", "graphics_storage", "graphics_texture", "transform_feedback",
        "compute_storage", "compute_texture", "dma_image", "dma_copy"};
    static std::unordered_map<u64, u64> seen;
    static u32 lines{};
    const u64 key = stale.vertex_address ^ (stale.lo << 20) ^ (u64{stale.resynced} << 63);
    std::scoped_lock lock{mutex};
    if (lines >= 1024) {
        return;
    }
    auto [it, inserted] = seen.try_emplace(key, stale.frame);
    if (!inserted) {
        if (stale.frame - it->second < 600) {
            return;
        }
        it->second = stale.frame;
    }
    ++lines;
    const u64 kind = stale.origin & 0xff;
    if (std::FILE* const out = Log()) {
        fmt::print(out,
                   "f{} t{} ms XFB-STALE-VB vertex {:#x}+{:#x} vs {:016x} reads {:#x}..{:#x} "
                   "({} bytes) written by xfb {:#x}+{:#x} vs {:016x} f{} ({} frames ago) | last "
                   "gpu writer {} (mark #{}){}\n",
                   stale.frame, NowMs(), stale.vertex_address, stale.vertex_size,
                   stale.vertex_shader, stale.lo, stale.hi, stale.hi - stale.lo,
                   stale.feedback_address, stale.feedback_size, stale.feedback_shader,
                   stale.feedback_frame, stale.frame - stale.feedback_frame,
                   kind < names.size() ? names[kind] : "unknown", stale.origin >> 8,
                   stale.resynced ? " | RESYNCED from guest" : "");
        std::fflush(out);
    }
}

WriterScope::WriterScope(Writer writer) : previous{current_writer} {
    current_writer = writer;
}

WriterScope::~WriterScope() {
    current_writer = previous;
}

namespace {
std::string CheckCommandWrite(Writer writer, u64 address, u64 size, u32 now_frame);
} // Anonymous namespace
void LogCommandWrite(const std::string& line);

void RecordWrite(u64 address, u64 size) {
    RecordWrite(current_writer, address, size);
}

void RecordWrite(Writer writer, u64 address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 now = NowMs();
    const u32 now_frame = static_cast<u32>(journal_frame.load(std::memory_order_relaxed));
    const u64 first = address >> PAGE_BITS;
    const u64 last = (address + size - 1) >> PAGE_BITS;
    std::string command_line;
    {
        std::scoped_lock lock{journal_mutex};
        command_line = CheckCommandWrite(writer, address, size, now_frame);
    }
    if (!command_line.empty()) {
        LogCommandWrite(command_line);
    }
    std::scoped_lock lock{journal_mutex};
    if (journal.size() > JOURNAL_PAGES) {
        journal.clear();
        ++journal_clears;
    }
    for (u64 page = first; page <= last; ++page) {
        JournalPage& slots = journal[page];
        const u64 page_start = page << PAGE_BITS;
        const u64 begin = std::max(address, page_start);
        const u64 end = std::min(address + size, page_start + PAGE_SIZE);
        const JournalWrite write{begin, now, static_cast<u32>(end - begin), now_frame, writer};
        // Back-to-back writes of the same kind (a download in pieces) keep one slot.
        if (slots[0].size != 0 && slots[0].writer == writer && slots[0].time_ms == now) {
            const u64 lo = std::min(slots[0].address, begin);
            const u64 hi = std::max(slots[0].address + slots[0].size, end);
            slots[0].address = lo;
            slots[0].size = static_cast<u32>(hi - lo);
            continue;
        }
        slots[1] = slots[0];
        slots[0] = write;
    }
}

void ReportOob(const OobDraw& draw) {
    const u64 key = draw.index_address ^ (u64{draw.count} << 40) ^ (u64{draw.index_size} << 60);
    std::vector<std::string> lines;
    {
        std::scoped_lock lock{journal_mutex};
        if (oob_reports >= OOB_REPORTS || !oob_reported.insert(key).second) {
            return;
        }
        ++oob_reports;
        const u64 bytes = u64{draw.count} * draw.index_size;
        const u64 first = draw.index_address >> PAGE_BITS;
        const u64 last = (draw.index_address + std::max<u64>(bytes, 1) - 1) >> PAGE_BITS;
        if (!Enabled()) {
            lines.push_back("    (journal off: it runs only with geo_hunt=1)");
        }
        for (u64 page = first; page <= last && page < first + 16 && Enabled(); ++page) {
            const auto it = journal.find(page);
            if (it == journal.end() || it->second[0].size == 0) {
                lines.push_back(fmt::format("    page {:#x}: no emulator write recorded", page
                                                                                       << PAGE_BITS));
                continue;
            }
            for (const JournalWrite& write : it->second) {
                if (write.size == 0) {
                    continue;
                }
                const bool overlaps = write.address < draw.index_address + bytes &&
                                      draw.index_address < write.address + write.size;
                lines.push_back(fmt::format(
                    "    page {:#x}: {} {:#x}+{:#x} f{} t{} ms{}", page << PAGE_BITS,
                    WriterName(write.writer), write.address, write.size, write.frame,
                    write.time_ms, overlaps ? " OVERLAPS THE INDICES" : ""));
            }
        }
        if (journal_clears != 0) {
            lines.push_back(fmt::format("    (journal was cleared {} times: older writes lost)",
                                        journal_clears));
        }
    }
    const u64 vs = draw.vertex_shader != 0 ? draw.vertex_shader : shader_a;
    std::scoped_lock lock{mutex};
    if (std::FILE* const out = Log()) {
        fmt::print(out,
                   "f{} t{} ms OOB-INDEX ib {:#x} count {} isz {} topo {} max {} base {} "
                   "limit {} bad {} first bad #{}={} vs {:016x} -> {}\n",
                   frame, NowMs(), draw.index_address, draw.count, draw.index_size,
                   draw.topology, draw.max_index, draw.base_vertex, draw.vertex_limit, draw.bad,
                   draw.first_bad, draw.first_bad_value, vs,
                   draw.fixed ? "bad triangles dropped" : "drawn as is (fix off)");
        fmt::print(out, "  emulator writes to the index pages (newest first):\n");
        for (const std::string& line : lines) {
            fmt::print(out, "{}\n", line);
        }
        std::fflush(out);
    }
    LOG_WARNING(HW_GPU,
                "geo_oob: ib {:#x} count {} max index {} + base {} >= {} vertices ({} bad), {}",
                draw.index_address, draw.count, draw.max_index, draw.base_vertex,
                draw.vertex_limit, draw.bad, draw.fixed ? "dropped" : "drawn");
}

namespace {

struct CommandSegment {
    u64 gpu_va;
    u64 device_addr;
    u64 size;
    u64 frame;
};
/// Device pages parsed as commands, with the frame they were last parsed.
std::unordered_map<u64, u32> command_pages;
CommandSegment last_segment{};
bool garbage_in_segment{};
u32 garbage_reports{};
u32 command_write_reports{};
u32 stale_reports{};
std::unordered_map<u64, u64> stale_reported;
constexpr u32 GARBAGE_REPORTS = 8;
std::atomic<bool> census_request{};
constexpr u32 COMMAND_WRITE_REPORTS = 1024;
constexpr u32 STALE_REPORTS = 256;
/// A page parsed as commands this recently still counts as a command page.
constexpr u32 COMMAND_PAGE_FRAMES = 600;

/// Needs journal_mutex.
void JournalLines(u64 page, u64 lo, u64 hi, std::vector<std::string>& lines, const char* hit) {
    const auto it = journal.find(page >> PAGE_BITS);
    if (it == journal.end() || it->second[0].size == 0) {
        lines.push_back(fmt::format("    page {:#x}: no emulator write recorded", page));
        return;
    }
    for (const JournalWrite& write : it->second) {
        if (write.size == 0) {
            continue;
        }
        const bool overlaps = write.address < hi && lo < write.address + write.size;
        lines.push_back(fmt::format("    page {:#x}: {} {:#x}+{:#x} f{} t{} ms{}", page,
                                    WriterName(write.writer), write.address, write.size,
                                    write.frame, write.time_ms, overlaps ? hit : ""));
    }
}

/// Needs journal_mutex. An emulator write landing on memory parsed as commands lately: the
/// line to log once the lock is released, empty when none.
std::string CheckCommandWrite(Writer writer, u64 address, u64 size, u32 now_frame) {
    if (command_pages.empty() || command_write_reports >= COMMAND_WRITE_REPORTS) {
        return {};
    }
    for (u64 page = address >> PAGE_BITS; page <= (address + size - 1) >> PAGE_BITS; ++page) {
        const auto it = command_pages.find(page);
        if (it == command_pages.end() || now_frame - it->second > COMMAND_PAGE_FRAMES) {
            continue;
        }
        ++command_write_reports;
        return fmt::format("f{} t{} ms WRITE-INTO-COMMANDS {} {:#x}+{:#x} on page {:#x} parsed "
                           "as commands f{} (last segment gpu {:#x} device {:#x}+{:#x})\n",
                           now_frame, NowMs(), WriterName(writer), address, size,
                           page << PAGE_BITS, it->second, last_segment.gpu_va,
                           last_segment.device_addr, last_segment.size);
    }
    return {};
}

} // Anonymous namespace

void LogCommandWrite(const std::string& line) {
    std::scoped_lock lock{mutex};
    if (std::FILE* const out = Log()) {
        fmt::print(out, "{}", line);
        std::fflush(out);
    }
}

namespace {

} // Anonymous namespace

void NoteUpload(u64 address, u64 size) {
    if (size == 0) {
        return;
    }
    if (uploads.size() > 256 * 1024) {
        uploads.clear();
    }
    const u32 now = static_cast<u32>(frame);
    for (u64 page = address >> PAGE_BITS; page <= (address + size - 1) >> PAGE_BITS; ++page) {
        auto [it, inserted] = uploads.try_emplace(page, UploadNote{now, 0, now});
        it->second.frame = now;
        ++it->second.count;
    }
}

void ConfigureAudit(u32 budget_kib) {
    std::scoped_lock lock{mutex};
    audit_budget = u64{budget_kib != 0 ? budget_kib : DEFAULT_AUDIT_KIB} * 1024;
    audit_left = audit_budget;
    LOG_INFO(HW_GPU, "geo_hunt: host audit {} KiB/frame, a clean range rests {} frames",
             audit_budget / 1024, AUDIT_REST_FRAMES);
}

bool AuditTake(u64 address, u64 size) {
    std::scoped_lock lock{mutex};
    if (audit_left < size) {
        return false;
    }
    const u64 key = address ^ (size << 40) ^ (size >> 24);
    if (audited_ranges.size() > 64 * 1024) {
        audited_ranges.clear();
    }
    auto [it, inserted] = audited_ranges.try_emplace(key, frame);
    if (!inserted) {
        if (frame - it->second < AUDIT_REST_FRAMES) {
            return false;
        }
        it->second = frame;
    }
    audit_left -= size;
    ++window.audits;
    window.audit_kib += (size + 1023) / 1024;
    return true;
}

void ReportHostStale(const AuditReport& report, const std::vector<AuditPage>& stale_pages,
                     const u8* host, const u8* guest, u64 bytes) {
    std::vector<std::string> lines;
    {
        std::scoped_lock lock{journal_mutex};
        for (const AuditPage& page : stale_pages) {
            JournalLines(page.page, page.page, page.page + PAGE_SIZE, lines, "");
        }
    }
    std::scoped_lock lock{mutex};
    ++window.audit_stale;
    // The same range keeps failing every audit: report it again only after a while.
    const u64 key = report.address ^ (report.size << 40);
    auto [it, inserted] = stale_reported.try_emplace(key, frame);
    if (!inserted && frame - it->second < 600) {
        return;
    }
    it->second = frame;
    if (stale_reports >= STALE_REPORTS) {
        return;
    }
    ++stale_reports;
    std::FILE* const out = Log();
    if (!out) {
        return;
    }
    fmt::print(out,
               "f{} t{} ms HOST-STALE {} range {:#x}+{:#x} bound f{} | buffer #{} {:#x}+{:#x} "
               "created f{} | left out gpu {} cpu {} | vs {:016x}\n",
               frame, NowMs(), RoleName(report.role), report.address, report.size,
               report.bind_frame, report.buffer_id, report.buffer_address, report.buffer_size,
               report.buffer_frame, report.excluded_gpu, report.excluded_cpu,
               report.vertex_shader);
    for (const AuditPage& page : stale_pages) {
        const auto up = uploads.find(page.page >> PAGE_BITS);
        const std::string upload =
            up == uploads.end()
                ? std::string{"never uploaded"}
                : fmt::format("last upload f{} ({} uploads since f{})", up->second.frame,
                              up->second.count, up->second.first_frame);
        fmt::print(out, "  page {:#x}: {} of {} bytes differ, first at +{:#x}{}{}{}{} | {}\n",
                   page.page, page.diff_bytes, page.audited_bytes, page.first_diff,
                   page.host_zero ? ", host holds zeros there" : "",
                   page.guest_moved ? ", guest changed after the bind" : "",
                   page.cpu_now ? ", cpu-modified now" : "",
                   page.gpu_now ? ", gpu-modified now" : "", upload);
    }
    fmt::print(out, "  emulator writes to those pages (newest first):\n");
    for (const std::string& line : lines) {
        fmt::print(out, "{}\n", line);
    }
    if (bytes != 0 && !stale_pages.empty()) {
        // Up to 32 bytes from the first difference, host then guest.
        const u64 count = std::min<u64>(32, bytes);
        std::string h;
        std::string g;
        for (u64 i = 0; i < count; ++i) {
            h += fmt::format("{:02x}", host[i]);
            g += fmt::format("{:02x}", guest[i]);
        }
        fmt::print(out, "  at {:#x}:\n    host  {}\n    guest {}\n",
                   stale_pages.front().page + stale_pages.front().first_diff, h, g);
    }
    std::fflush(out);
}

void NoteCommandSegment(u64 gpu_va, u64 device_addr, u64 size) {
    const u32 now = static_cast<u32>(journal_frame.load(std::memory_order_relaxed));
    std::scoped_lock lock{journal_mutex};
    last_segment = CommandSegment{gpu_va, device_addr, size, now};
    garbage_in_segment = false;
    if (device_addr == 0 || size == 0) {
        return;
    }
    if (command_pages.size() > 256 * 1024) {
        command_pages.clear();
    }
    for (u64 page = device_addr >> PAGE_BITS; page <= (device_addr + size - 1) >> PAGE_BITS;
         ++page) {
        command_pages[page] = now;
    }
}

void CommandGarbage(const char* engine, u32 method, u32 argument) {
    if (!Enabled()) {
        return;
    }
    census_request.store(true, std::memory_order_relaxed);
    std::vector<std::string> lines;
    CommandSegment segment;
    {
        std::scoped_lock lock{journal_mutex};
        if (garbage_in_segment || garbage_reports >= GARBAGE_REPORTS) {
            return;
        }
        garbage_in_segment = true;
        ++garbage_reports;
        segment = last_segment;
        if (segment.device_addr != 0) {
            const u64 first = segment.device_addr & ~(PAGE_SIZE - 1);
            for (u64 page = first;
                 page < segment.device_addr + segment.size && page < first + 16 * PAGE_SIZE;
                 page += PAGE_SIZE) {
                JournalLines(page, segment.device_addr, segment.device_addr + segment.size, lines,
                             " INSIDE THE SEGMENT");
            }
        }
    }
    std::scoped_lock lock{mutex};
    if (std::FILE* const out = Log()) {
        fmt::print(out,
                   "f{} t{} ms COMMAND-GARBAGE {} method {:#x} arg {:#x} | segment gpu {:#x} "
                   "device {:#x} {} bytes (parsed f{})\n",
                   frame, NowMs(), engine, method, argument, segment.gpu_va, segment.device_addr,
                   segment.size, segment.frame);
        fmt::print(out, "  emulator writes to the segment pages (newest first):\n");
        for (const std::string& line : lines) {
            fmt::print(out, "{}\n", line);
        }
        std::fflush(out);
    }
    LOG_ERROR(HW_GPU, "geo_hunt: garbage command ({} method {:#x}) in segment gpu {:#x}", engine,
              method, segment.gpu_va);
}

bool TakeGarbageDump() {
    std::scoped_lock lock{journal_mutex};
    const bool dump = garbage_in_segment;
    garbage_in_segment = false;
    return dump;
}

void DumpCommandWords(const u32* words, size_t count) {
    std::scoped_lock lock{mutex};
    std::FILE* const out = Log();
    if (!out) {
        return;
    }
    count = std::min<size_t>(count, 512);
    fmt::print(out, "  segment words ({} shown):\n", count);
    for (size_t i = 0; i < count; i += 8) {
        std::string line;
        for (size_t j = i; j < std::min(count, i + 8); ++j) {
            line += fmt::format(" {:08x}", words[j]);
        }
        fmt::print(out, "   +{:04x}:{}\n", i * 4, line);
    }
    std::fflush(out);
}

bool TakeCensusRequest() {
    return census_request.exchange(false, std::memory_order_relaxed);
}

void NoteCensus(u64 census_frame, const std::string& counts) {
    size_t n_journal{}, n_command{}, n_stale{}, n_oob{};
    {
        std::scoped_lock lock{journal_mutex};
        n_journal = journal.size();
        n_command = command_pages.size();
        n_stale = stale_reported.size();
        n_oob = oob_reported.size();
    }
    std::scoped_lock lock{mutex};
    if (std::FILE* const out = Log()) {
        fmt::print(out,
                   "f{} t{} ms CENSUS {} | geo pages {} ranges_this_frame {} recent {} uploads {} "
                   "audited {} journal {} (clears {}) command_pages {} stale_reported {} "
                   "oob_reported {}\n",
                   census_frame, NowMs(), counts, pages.size(), ranges_this_frame.size(),
                   recent.size(), uploads.size(), audited_ranges.size(), n_journal,
                   journal_clears, n_command, n_stale, n_oob);
        std::fflush(out);
    }
}

} // namespace VideoCommon::GeoHunt
