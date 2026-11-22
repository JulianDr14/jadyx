// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_memory_guard.h"

// Pure policy for which graphics PSOs stay resident: no device, file or clock, so every decision is
// covered by tools/xbox/tests/pipeline-residency.cpp.
//
// A compiled PSO is the driver's: about 100 KiB each, against some 12 KiB for the pipeline's own
// bookkeeping and a share of its DXIL. A game that built every pipeline of its disk cache at boot
// (17,000 for one of them) spent 1.3 GiB on PSOs before its first frame. So a pipeline keeps its
// DXIL and only has a PSO while it is drawn: the ones used in recent sessions are built at boot,
// the rest when first drawn, and a PSO idle for long enough is released and built again from the
// kept DXIL if it is drawn again.
namespace D3D12 {

/// How long an idle PSO is kept, in frames, for the app's free memory: for good while there is
/// plenty, minutes while there is room, seconds as the memory guard's thresholds near.
///
/// Releasing an idle PSO saves some 80 KiB and costs a driver compile (about 50 ms on the Series,
/// a stall when drawn again); with gigabytes free that trade only loses, so eviction waits until
/// free memory falls under PLENTY_FREE. The background prewarm of dormant PSOs stops at the same
/// line, so the two never undo each other's work.
struct PipelineResidencyPolicy {
    static constexpr std::uint64_t KEEP_IDLE_FRAMES = ~std::uint64_t{0}; ///< Never released.
    static constexpr std::uint64_t IDLE_FRAMES = 3 * 60 * 60;      ///< Three minutes at 60 FPS.
    static constexpr std::uint64_t LOW_IDLE_FRAMES = 10 * 60;      ///< Ten seconds.
    static constexpr std::uint64_t EMERGENCY_IDLE_FRAMES = 60;     ///< One second.
    static constexpr std::size_t SWEEP_PER_FRAME = 256;            ///< Pipelines checked a frame.
    /// 1.5 GiB: some 19,000 PSOs, more than the largest disk cache seen builds after dedup.
    static constexpr std::uint64_t PLENTY_FREE = 1536 * MemoryGuardPolicy::MiB;

    /// limit 0 is an unknown limit: as with the memory guard, it never implies pressure, but it
    /// is not known to be plenty either, so idle PSOs keep the minutes limit.
    [[nodiscard]] static constexpr std::uint64_t IdleFrames(std::uint64_t used,
                                                            std::uint64_t limit) noexcept {
        if (limit == 0) {
            return IDLE_FRAMES;
        }
        const std::uint64_t free = used >= limit ? 0 : limit - used;
        if (free <= MemoryGuardPolicy::EMERGENCY_FREE) {
            return EMERGENCY_IDLE_FRAMES;
        }
        if (free <= MemoryGuardPolicy::LOW_FREE) {
            return LOW_IDLE_FRAMES;
        }
        if (free > PLENTY_FREE) {
            return KEEP_IDLE_FRAMES;
        }
        return IDLE_FRAMES;
    }

    /// Whether a dormant PSO may be built ahead of its first draw: only with plenty free, and
    /// never with an unknown limit.
    [[nodiscard]] static constexpr bool MayPrewarm(std::uint64_t used,
                                                   std::uint64_t limit) noexcept {
        return limit != 0 && used < limit && limit - used > PLENTY_FREE;
    }
};

/// Whether releasing idle PSOs under memory pressure gives commit back, measured on the first
/// releases: a few hundred are released, eviction pauses until their fences retire, and the drop
/// in the commit the ledger does not track (DXIL and bookkeeping are tracked) is split among them.
/// Below MIN_RELEASE_BYTES a PSO, pressure eviction stops for the session and idle PSOs are
/// released only in an emergency. On the Series the driver keeps PSOs outside the app's commit:
/// 2737 released at a time of pressure freed nothing and were compiled again mid-game, 24-34 s of
/// stalls a run. A measure at boot is no substitute: the compiles there leave transient heap
/// growth behind (+29 MiB one boot, +263 MiB the next for a similar number of PSOs).
class PipelineReleaseProbe {
public:
    static constexpr std::uint64_t PROBE_PSOS = 256;
    /// Frames for the released PSOs' fences to retire before the commit is read again.
    static constexpr std::uint64_t SETTLE_FRAMES = 120;
    /// A probe that has not released PROBE_PSOS by then starts over from a fresh baseline.
    static constexpr std::uint64_t MAX_FRAMES = 60 * 60;
    static constexpr std::uint64_t MIN_RELEASE_BYTES = 16 * 1024;

    enum class Verdict : std::uint8_t { Unknown, Pays, DoesNotPay };

    /// Called once a frame before the eviction sweep: the PSOs released so far and the commit
    /// the ledger does not track (untracked_known false when the commit is unknown).
    void Tick(std::uint64_t frame, std::uint64_t released, std::uint64_t untracked,
              bool untracked_known) noexcept {
        if (verdict != Verdict::Unknown || !untracked_known) {
            return;
        }
        if (settle_frame != 0) {
            if (frame - settle_frame < SETTLE_FRAMES) {
                return;
            }
            freed = baseline_untracked > untracked ? baseline_untracked - untracked : 0;
            probed = released - baseline_released;
            verdict = freed / probed >= MIN_RELEASE_BYTES ? Verdict::Pays : Verdict::DoesNotPay;
            return;
        }
        if (!has_baseline || released == baseline_released ||
            frame - baseline_frame > MAX_FRAMES) {
            // Nothing released since the baseline (or a stale probe): it moves up to this frame.
            has_baseline = true;
            baseline_untracked = untracked;
            baseline_released = released;
            baseline_frame = frame;
            return;
        }
        if (released - baseline_released >= PROBE_PSOS) {
            settle_frame = frame;
        }
    }

    /// Whether pressure eviction may release PSOs this frame; an emergency releases regardless.
    [[nodiscard]] bool MayRelease() const noexcept {
        return verdict == Verdict::Pays || (verdict == Verdict::Unknown && settle_frame == 0);
    }

    [[nodiscard]] Verdict GetVerdict() const noexcept {
        return verdict;
    }
    [[nodiscard]] std::uint64_t Freed() const noexcept {
        return freed;
    }
    [[nodiscard]] std::uint64_t Probed() const noexcept {
        return probed;
    }

private:
    Verdict verdict{Verdict::Unknown};
    bool has_baseline{};
    std::uint64_t baseline_untracked{};
    std::uint64_t baseline_released{};
    std::uint64_t baseline_frame{};
    std::uint64_t settle_frame{}; ///< 0 while releasing; frame numbers start at 1.
    std::uint64_t freed{};
    std::uint64_t probed{};
};

/// threshold and are already learned individually.
class PsoFrameWaits {
public:
    static constexpr std::uint64_t MIN_MEMBER_US = 8000;
    static constexpr std::uint64_t MIN_FRAME_US = 32000;
    static constexpr std::size_t MAX_MEMBERS = 32;

    void Record(std::uint64_t key, std::uint64_t wait_us) {
        if (wait_us < MIN_MEMBER_US) {
            return;
        }
        total_us = (std::min)(MIN_FRAME_US, total_us + (std::min)(MIN_FRAME_US, wait_us));
        if (wait_us >= MIN_FRAME_US) {
            return; // Long waits are already learned individually.
        }
        if (count < entries.size()) {
            entries[count++] = {key, wait_us};
            return;
        }
        const auto cheapest = std::ranges::min_element(entries, {}, &Entry::wait_us);
        if (wait_us > cheapest->wait_us) {
            *cheapest = {key, wait_us};
        }
    }

    void Finish(auto&& learn) {
        if (total_us >= MIN_FRAME_US) {
            for (const auto& [key, wait_us] : std::span{entries}.first(count)) {
                learn(key, wait_us);
            }
        }
        count = 0;
        total_us = 0;
    }

private:
    struct Entry {
        std::uint64_t key, wait_us;
    };
    std::array<Entry, MAX_MEMBERS> entries{};
    std::size_t count{};
    std::uint64_t total_us{};
};

/// The pipelines drawn in a game's recent sessions, by key hash, persisted next to its disk
/// cache. Each session marks what it draws; a pipeline not drawn for KEEP_SESSIONS sessions is
/// forgotten, so the set follows where the player is instead of growing with the whole game.
class HotPipelineSet {
public:
    /// Short test launches count as sessions too: with 4, a few of them in a row forgot the areas
    /// played before, and their PSOs were built again mid-game. MAX_PREWARM still bounds boot.
    static constexpr std::uint32_t KEEP_SESSIONS = 16;
    /// Pipelines built at boot at most.
    static constexpr std::size_t MAX_PREWARM = 4096;
    /// Reserve a small part of the existing cap for expensive waits from the previous route.
    static constexpr std::size_t MAX_WAIT_PRIORITY = 512;
    /// Taken from recency slots, leaving the existing long-wait tier intact.
    static constexpr std::size_t MAX_FRAME_WAIT_PRIORITY = 128;
    /// Multi-session wait retention: costly waits from earlier kept sessions (up to KEEP_SESSIONS)
    /// retain priority after the immediate previous session's tiers are filled.
    static constexpr std::size_t MAX_RETAINED_WAIT_PRIORITY = 1024;
    static constexpr std::uint64_t MIN_DORMANT_WAIT_US = PsoFrameWaits::MIN_FRAME_US;
    static constexpr std::uint64_t REAL_STALL_BIAS_US = 1000000;
    /// With no history yet (first session, or a file of an older format), the first pipelines
    /// of the disk cache, which is in the order they were first seen: the start of the game.
    static constexpr std::size_t FIRST_SESSION_PREWARM = 2048;

    /// Scores observed GPU/recorder waits strictly above background worker build times whose
    /// latency was already absorbed asynchronously before the draw waited.
    [[nodiscard]] static constexpr std::uint64_t EffectiveLearnedWaitUs(
        std::uint64_t observed_wait_us, std::uint64_t total_build_us) noexcept {
        if (observed_wait_us >= MIN_DORMANT_WAIT_US) {
            return REAL_STALL_BIAS_US + (std::min)(observed_wait_us, std::uint64_t{3000000});
        }
        if (total_build_us >= MIN_DORMANT_WAIT_US) {
            return (std::min)(total_build_us, REAL_STALL_BIAS_US - 1);
        }
        return 0;
    }

    void SetWaitPriorityCap(std::size_t cap) noexcept {
        wait_priority_cap = (std::min)(cap, MAX_PREWARM);
    }
    void SetKeepSessions(std::uint32_t sessions) noexcept {
        keep_sessions = sessions == 0 ? 1u : sessions;
    }
    void SetRetainMultiSessionWaits(bool enabled) noexcept {
        retain_multi_session_waits = enabled;
    }
    [[nodiscard]] bool RetainsMultiSessionWaits() const noexcept {
        return retain_multi_session_waits;
    }

    /// Starts a session from a saved file; an empty, damaged or foreign one starts with no history.
    void Load(std::span<const std::uint8_t> bytes) {
        last_used.clear();
        prewarm.clear();
        wait_priority.clear();
        wait_priority_count = 0;
        retained_wait_priority.clear();
        retained_wait_priority_count = 0;
        session = 1;
        if (bytes.size() < HEADER_SIZE || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0) {
            return;
        }
        const auto version = Read32(bytes, 4);
        if (version < 1 || version > VERSION) {
            return;
        }
        const std::size_t entry_size =
            version == 1 ? LEGACY_ENTRY_SIZE : version == 2 ? RECENCY_ENTRY_SIZE : ENTRY_SIZE;
        const std::uint32_t saved_session = Read32(bytes, 8);
        const std::uint32_t count = Read32(bytes, 12);
        if (saved_session == std::numeric_limits<std::uint32_t>::max() ||
            count != (bytes.size() - HEADER_SIZE) / entry_size ||
            (bytes.size() - HEADER_SIZE) % entry_size != 0) {
            return;
        }
        session = saved_session + 1;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t offset = HEADER_SIZE + std::size_t{i} * entry_size;
            const std::uint32_t entry_session = Read32(bytes, offset + 8);
            if (entry_session < session && session - entry_session <= keep_sessions) {
                last_used.emplace(Read64(bytes, offset),
                                  Usage{entry_session, version == 1 ? 0 : Read64(bytes, offset + 12),
                                        version == VERSION
                                            ? (std::min)(Read32(bytes, offset + 20), MAX_WAIT_US)
                                            : 0});
            }
        }
        BuildPrewarm();
    }

    [[nodiscard]] bool HasHistory() const noexcept {
        return !last_used.empty();
    }

    /// Whether the pipeline should get its PSO at boot. file_index is its position in the disk
    /// cache, for the first session's fallback.
    [[nodiscard]] bool Prewarm(std::uint64_t hash, std::size_t file_index) const {
        if (!HasHistory()) {
            return file_index < FIRST_SESSION_PREWARM;
        }
        return prewarm.contains(hash);
    }

    /// Whether the pipeline was drawn in one of the kept sessions, built at boot or not.
    [[nodiscard]] bool Known(std::uint64_t hash) const {
        return last_used.contains(hash);
    }

    /// Session first, then the last frame drawn: the area most recently played gets priority
    /// within the unchanged boot cap. Legacy profiles have no frame order and keep hash ties.
    [[nodiscard]] std::pair<std::uint32_t, std::uint64_t> Recency(std::uint64_t hash) const {
        const auto it = last_used.find(hash);
        return it == last_used.end() ? std::pair<std::uint32_t, std::uint64_t>{}
                                    : std::pair{it->second.session, it->second.frame};
    }

    /// Sort descending by known, tier (previous-session long/short wait ahead of older retained
    /// waits ahead of zero-wait recency), wait/session, and last frame.
    using BackgroundRank =
        std::tuple<bool, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t>;
    [[nodiscard]] BackgroundRank RankForBackground(std::uint64_t hash) const {
        const auto it = last_used.find(hash);
        if (it == last_used.end()) {
            return {};
        }
        const auto& usage = it->second;
        const bool prev_session = usage.session + 1 == session;
        const bool allow_wait = retain_multi_session_waits || prev_session;
        const auto wait_us =
            allow_wait && usage.wait_us >= PsoFrameWaits::MIN_MEMBER_US ? usage.wait_us : 0;
        if (wait_us == 0) {
            return {true, 0, usage.session, 0, usage.frame};
        }
        const bool long_wait = wait_us >= MIN_DORMANT_WAIT_US;
        const std::uint32_t tier = (prev_session ? 2u : 0u) + (long_wait ? 2u : 1u);
        const std::uint32_t primary = prev_session ? wait_us : usage.session;
        const std::uint32_t secondary = prev_session ? usage.session : wait_us;
        return {true, tier, primary, secondary, usage.frame};
    }

    /// Updates at most once per pipeline per frame; true when a periodic save is needed.
    /// No clock or allocation is added to repeated draws of an existing pipeline.
    bool MarkUsed(std::uint64_t hash, std::uint64_t frame = 0) {
        auto [it, inserted] = last_used.try_emplace(hash, Usage{session, frame});
        if (!inserted && it->second.session == session && it->second.frame >= frame) {
            return false;
        }
        it->second.session = session;
        it->second.frame = frame;
        return true;
    }

    /// Called after a successful wait for a dormant or serializable new pipeline.
    bool RecordBuildWait(std::uint64_t hash, std::uint64_t wait_us) {
        if (wait_us < MIN_DORMANT_WAIT_US) {
            return false;
        }
        const auto it = last_used.find(hash);
        if (it == last_used.end() || it->second.session != session) {
            return false;
        }
        const auto cost = static_cast<std::uint32_t>((std::min)(wait_us, std::uint64_t{MAX_WAIT_US}));
        if (it->second.wait_us == cost) {
            return false;
        }
        it->second.wait_us = cost;
        return true;
    }

    /// A short member of a costly frame keeps its own duration. Preserve stronger prior evidence.
    bool RecordFrameWait(std::uint64_t hash, std::uint64_t wait_us) {
        if (wait_us < PsoFrameWaits::MIN_MEMBER_US || wait_us >= MIN_DORMANT_WAIT_US) {
            return false;
        }
        const auto it = last_used.find(hash);
        if (it == last_used.end() || it->second.session != session ||
            it->second.wait_us >= wait_us) {
            return false;
        }
        it->second.wait_us = static_cast<std::uint32_t>(wait_us);
        return true;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return last_used.size();
    }

    [[nodiscard]] std::size_t PrewarmSize() const noexcept {
        return prewarm.size();
    }

    [[nodiscard]] std::size_t WaitPriorityCount() const noexcept {
        return wait_priority_count;
    }

    [[nodiscard]] std::size_t RetainedWaitPriorityCount() const noexcept {
        return retained_wait_priority_count;
    }

    /// Frozen at load, before MarkUsed changes the current session's last-use records.
    [[nodiscard]] bool WaitPriority(std::uint64_t hash) const {
        return wait_priority.contains(hash);
    }

    /// Frozen at load: costly pipelines from older kept sessions preserved past the immediate route.
    [[nodiscard]] bool RetainedWaitPriority(std::uint64_t hash) const {
        return retained_wait_priority.contains(hash);
    }

    [[nodiscard]] std::vector<std::uint8_t> Serialize() const {
        std::vector<std::uint8_t> bytes(HEADER_SIZE + last_used.size() * ENTRY_SIZE);
        std::memcpy(bytes.data(), MAGIC, sizeof(MAGIC));
        Write32(bytes, 4, VERSION);
        Write32(bytes, 8, session);
        Write32(bytes, 12, static_cast<std::uint32_t>(last_used.size()));
        std::size_t offset = HEADER_SIZE;
        for (const auto& [hash, usage] : last_used) {
            Write64(bytes, offset, hash);
            Write32(bytes, offset + 8, usage.session);
            Write64(bytes, offset + 12, usage.frame);
            Write32(bytes, offset + 20, usage.wait_us);
            offset += ENTRY_SIZE;
        }
        return bytes;
    }

private:
    static constexpr char MAGIC[4] = {'D', '3', 'H', 'P'};
    static constexpr std::uint32_t VERSION = 3;
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::size_t LEGACY_ENTRY_SIZE = 12;
    static constexpr std::size_t RECENCY_ENTRY_SIZE = 20;
    static constexpr std::size_t ENTRY_SIZE = 24;
    static constexpr std::uint32_t MAX_WAIT_US = 4000000;

    struct Usage {
        std::uint32_t session;
        std::uint64_t frame;
        std::uint32_t wait_us{};
    };

    void BuildPrewarm() {
        std::vector<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>> by_recency;
        std::vector<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>> by_wait;
        decltype(by_wait) by_frame_wait;
        by_recency.reserve(last_used.size());
        for (const auto& [hash, usage] : last_used) {
            by_recency.emplace_back(usage.session, usage.frame, hash);
            if (usage.session == session - 1 && usage.wait_us >= PsoFrameWaits::MIN_MEMBER_US) {
                auto& ranked = usage.wait_us >= MIN_DORMANT_WAIT_US ? by_wait : by_frame_wait;
                ranked.emplace_back(usage.wait_us, usage.frame, hash);
            }
        }
        const auto select = [this](auto& entries, std::size_t limit) {
            const auto end = entries.begin() + (std::min)(entries.size(), limit);
            std::partial_sort(entries.begin(), end, entries.end(),
                              [](const auto& a, const auto& b) {
                                  if (std::get<0>(a) != std::get<0>(b)) {
                                      return std::get<0>(a) > std::get<0>(b);
                                  }
                                  if (std::get<1>(a) != std::get<1>(b)) {
                                      return std::get<1>(a) > std::get<1>(b);
                                  }
                                  return std::get<2>(a) < std::get<2>(b);
                              });
            for (auto it = entries.begin(); it != end && prewarm.size() < MAX_PREWARM; ++it) {
                prewarm.insert(std::get<2>(*it));
            }
        };
        select(by_wait, wait_priority_cap);
        wait_priority = prewarm;
        wait_priority_count = prewarm.size();
        select(by_frame_wait, MAX_FRAME_WAIT_PRIORITY);
        if (retain_multi_session_waits) {
            using RetainedEntry =
                std::tuple<bool, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t>;
            std::vector<RetainedEntry> by_retained_wait;
            for (const auto& [hash, usage] : last_used) {
                if (usage.wait_us >= PsoFrameWaits::MIN_MEMBER_US && !prewarm.contains(hash)) {
                    by_retained_wait.emplace_back(usage.wait_us >= MIN_DORMANT_WAIT_US,
                                                  usage.session, usage.wait_us, usage.frame, hash);
                }
            }
            const auto retained_end =
                by_retained_wait.begin() +
                (std::min)(by_retained_wait.size(), MAX_RETAINED_WAIT_PRIORITY);
            std::partial_sort(by_retained_wait.begin(), retained_end, by_retained_wait.end(),
                              [](const RetainedEntry& a, const RetainedEntry& b) {
                                  if (std::get<0>(a) != std::get<0>(b)) {
                                      return std::get<0>(a) > std::get<0>(b);
                                  }
                                  if (std::get<1>(a) != std::get<1>(b)) {
                                      return std::get<1>(a) > std::get<1>(b);
                                  }
                                  if (std::get<2>(a) != std::get<2>(b)) {
                                      return std::get<2>(a) > std::get<2>(b);
                                  }
                                  if (std::get<3>(a) != std::get<3>(b)) {
                                      return std::get<3>(a) > std::get<3>(b);
                                  }
                                  return std::get<4>(a) < std::get<4>(b);
                              });
            for (auto it = by_retained_wait.begin();
                 it != retained_end && prewarm.size() < MAX_PREWARM; ++it) {
                const auto hash = std::get<4>(*it);
                if (prewarm.insert(hash).second) {
                    retained_wait_priority.insert(hash);
                }
            }
            retained_wait_priority_count = retained_wait_priority.size();
        }
        select(by_recency, MAX_PREWARM);
    }

    static std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    static std::uint64_t Read64(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint64_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    static void Write32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    static void Write64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::uint32_t session{1};
    std::uint32_t keep_sessions{KEEP_SESSIONS};
    std::size_t wait_priority_cap{MAX_WAIT_PRIORITY};
    bool retain_multi_session_waits{false};
    std::unordered_map<std::uint64_t, Usage> last_used;
    std::unordered_set<std::uint64_t> prewarm;                  ///< chosen at Load
    std::unordered_set<std::uint64_t> wait_priority;            ///< At most MAX_WAIT_PRIORITY.
    std::size_t wait_priority_count{};
    std::unordered_set<std::uint64_t> retained_wait_priority;   ///< At most MAX_RETAINED_WAIT_PRIORITY.
    std::size_t retained_wait_priority_count{};
};

} // namespace D3D12
