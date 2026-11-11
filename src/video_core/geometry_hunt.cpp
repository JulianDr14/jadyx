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
    u64 healed;
    u64 budget_frames;
    u64 geometry_checks;
    u64 geometry_budget_frames;
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
                   "geo_hunt: check budget {} KiB/frame, upload budget {} KiB/frame, heal {}\n"
                   "MISS lines: page = device address, chunks = 256-byte pieces that changed "
                   "(bit 0 = first), cached = this device page's cached count, cpu_cached = CPU "
                   "page flag, aliases = device pages on the same memory (cached ones)\n",
                   check_budget / 1024, upload_budget / 1024, Healing() ? 1 : 0);
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
               "confirmed {}, healed {}, frames out of budget {} (index/vertex checks {}, "
               "frames out of their budget {})\n",
               label, frame, NowMs(), pages.size(), s.baselines, s.baseline_kib, s.dropped_full,
               s.dropped_budget, s.checks, s.checks * PAGE_SIZE / 1024, s.dirty_skips, s.suspects,
               s.suspects_cleared, s.confirmed, s.healed, s.budget_frames, s.geometry_checks,
               s.geometry_budget_frames);
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
    total.healed += part.healed;
    total.budget_frames += part.budget_frames;
    total.geometry_checks += part.geometry_checks;
    total.geometry_budget_frames += part.geometry_budget_frames;
}

} // Anonymous namespace

void Configure(bool on, u32 check_budget_kib, u32 upload_budget_kib, bool heal_on) {
    std::scoped_lock lock{mutex};
    check_budget = u64{check_budget_kib != 0 ? check_budget_kib : DEFAULT_CHECK_KIB} * 1024;
    upload_budget = u64{upload_budget_kib != 0 ? upload_budget_kib : DEFAULT_UPLOAD_KIB} * 1024;
    check_left.store(check_budget, std::memory_order_relaxed);
    geometry_left.store(check_budget * GEOMETRY_BUDGET_SCALE, std::memory_order_relaxed);
    upload_left = upload_budget;
    heal.store(heal_on, std::memory_order_relaxed);
    if (on) {
        pages.reserve(MAX_PAGES / 4);
        LOG_INFO(HW_GPU,
                 "geo_hunt on: check {} KiB/frame (+{} KiB for index/vertex), upload {} "
                 "KiB/frame, recheck every {} frames, heal {}",
                 check_budget / 1024, check_budget * GEOMETRY_BUDGET_SCALE / 1024,
                 upload_budget / 1024, RECHECK_FRAMES, heal_on);
    }
    enabled.store(on, std::memory_order_relaxed);
}

void SetHeal(bool on) {
    std::scoped_lock lock{mutex};
    heal.store(on, std::memory_order_relaxed);
    if (std::FILE* const out = Log()) {
        fmt::print(out, "f{} t{} ms: heal {}\n", frame, NowMs(), on ? "on" : "off");
        std::fflush(out);
    }
    LOG_INFO(HW_GPU, "geo_hunt: heal {}", on ? "on" : "off");
}

void Mark() {
    std::scoped_lock lock{mutex};
    ++marks;
    Stats total = stats;
    Add(total, window);
    std::FILE* const out = Log();
    if (out) {
        fmt::print(out, "\n==== MARK {} f{} t{} ms (user saw a deformation) heal {} ====\n",
                   marks, frame, NowMs(), Healing() ? "on" : "off");
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
    if (!Healing()) {
        // Without a heal the GPU copy stays behind; the new bytes become the baseline so a
        // later missed write shows as its own event.
        entry.chunks = now;
        entry.flags |= FLAG_STALE;
    }
    return Result::Confirmed;
}

void Report(u64 page, Role role, const Protection& protection, bool healed) {
    const auto it = pages.find(page >> PAGE_BITS);
    if (it == pages.end()) {
        return;
    }
    Entry& entry = it->second;
    if (healed) {
        ++window.healed;
    }
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
                   "cpu_cached {} aliases {} ({} cached) | {}\n",
                   frame, now, page, RoleName(role), RoleName(entry.upload_role),
                   entry.upload_frame, entry.suspect_frame, entry.mask, entry.events,
                   shader_compute ? "cs" : "vs/fs", shader_a, shader_b, protection.cpu_page,
                   protection.asid, protection.cached_count, protection.cpu_cached ? 1 : 0,
                   protection.aliases, protection.alias_cached, healed ? "healed" : "left stale");
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

WriterScope::WriterScope(Writer writer) : previous{current_writer} {
    current_writer = writer;
}

WriterScope::~WriterScope() {
    current_writer = previous;
}

void Journal(u64 address, u64 size) {
    Journal(current_writer, address, size);
}

void Journal(Writer writer, u64 address, u64 size) {
    if (size == 0 || !OobCheck()) {
        return;
    }
    const u64 now = NowMs();
    const u32 now_frame = static_cast<u32>(journal_frame.load(std::memory_order_relaxed));
    const u64 first = address >> PAGE_BITS;
    const u64 last = (address + size - 1) >> PAGE_BITS;
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
        for (u64 page = first; page <= last && page < first + 16; ++page) {
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

} // namespace VideoCommon::GeoHunt
