// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/stall_watch.h"
#include "common/windows/sampling_profiler.h"
#include "jadyx/app/boot_crash.h"
#include "jadyx/app/boot_diag.h"
#include "jadyx/app/freeze_watch.h"

namespace Jadyx {
namespace {

/// Seconds without a frame at which a freeze is reported; stacks at the first and third.
constexpr std::array<u64, 7> REPORT_AT_S{FreezeWatch::FIRST_REPORT_S, 15, 30, 60, 120, 240, 480};
constexpr u32 STACK_REPORTS[] = {0, 2};
/// A check this late means the process was suspended (the console's dashboard): not a freeze.
constexpr u64 SUSPENDED_GAP_MS = 3000;

void WriteStacks() {
    using Common::SamplingProfiler::CaptureStacks;
    for (const auto& stack : CaptureStacks()) {
        std::vector<void*> frames(stack.frames.size());
        std::ranges::transform(stack.frames, frames.begin(),
                               [](u64 pc) { return reinterpret_cast<void*>(pc); });
        const std::string prefix = fmt::format("freeze stack {} ({}):", stack.name, stack.id);
        std::string line = FormatFrames(prefix.c_str(), frames.data(),
                                        static_cast<unsigned short>(frames.size()));
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        WriteDiag(line);
    }
}

} // namespace

FreezeWatch::FreezeWatch() : created_ns{Common::SamplingProfiler::SteadyNs()} {}

void FreezeWatch::Tick(bool paused) {
    const u64 now_ms = Common::StallWatch::NowMs();
    if (now_ms < next_check_ms) {
        return;
    }
    next_check_ms = now_ms + 1000;
    Check(now_ms, paused);
}

void FreezeWatch::Check(u64 now_ms, bool paused) {
    namespace Profiler = Common::SamplingProfiler;
    const u64 now_ns = Profiler::SteadyNs();
    const bool suspended = last_check_ms != 0 && now_ms - last_check_ms > SUSPENDED_GAP_MS;
    last_check_ms = now_ms;
    if (paused || suspended) {
        expect_since_ns = now_ns;
    }

    if (const u64 oldest = Common::StallWatch::OldestStartMs();
        oldest != 0 && oldest != long_work_reported_ms && now_ms - oldest >= LONG_WORK_S * 1000) {
        long_work_reported_ms = oldest;
        WriteDiag("long work: " + Common::StallWatch::Describe(LONG_WORK_S * 1000));
    }

    const u64 last_frame_ns = Profiler::LastFrameNs();
    if (last_frame_ns < created_ns) {
        return; // still loading: this game's first frame has not come
    }
    if (reports != 0 && last_frame_ns > freeze_since_ns) {
        WriteDiag(fmt::format("freeze over: the game drew again after {:.1f} s without frames",
                              static_cast<double>(last_frame_ns - freeze_since_ns) / 1e9));
        reports = 0;
    }
    const u64 since_ns = std::max(last_frame_ns, expect_since_ns);
    if (paused || now_ns <= since_ns) {
        return;
    }
    const u64 stalled_s = (now_ns - since_ns) / 1'000'000'000ULL;
    if (reports >= REPORT_AT_S.size() || stalled_s < REPORT_AT_S[reports]) {
        return;
    }
    if (reports == 0) {
        freeze_since_ns = since_ns;
    }
    const std::string work = Common::StallWatch::Describe(1000);
    WriteDiag(fmt::format("freeze: no game frame for {} s | busy: {} | {}", stalled_s,
                          work.empty() ? "nothing tracked" : work, MemoryReport()));
    if (std::ranges::find(STACK_REPORTS, reports) != std::end(STACK_REPORTS)) {
        WriteStacks();
    }
    ++reports;
}

} // namespace Jadyx
