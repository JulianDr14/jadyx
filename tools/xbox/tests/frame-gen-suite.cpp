// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Frame generation behaviour suite: a report, not a gate. It measures how frame generation behaves
// on game-like timing; nothing in it is tuned to pass.
//
// Pacer (CPU): 35 games modelled frame by frame (render time, double or triple buffer, swap
// interval, dynamic vsync, host jitter, spikes, stalls, loading pauses) in four groups: 15-20 fps
// to 30, 45-50 fps to 60, 35-40 fps, and double/triple buffer with and without dynamic vsync; the
// last group again with the game animating by the vsync its frame is shown on. Per game: the rate
// picked and how soon, rate changes, generated share, held and backward frames, the step error and
// judder of the motion shown (in the game's own time), input lag against showing the frames as
// they come, and the pacer's delay.
//
// GPU (hardware GPU, debug layer): interpolation at the phases each group uses, slow to extreme
// motion, camera pans, still pairs and cuts; whole-path runs (ingest -> Next -> output) of six of
// the games with the motion measured on the output images.
//
// "frame-gen-suite.exe perf": ingest and compose timed at 720p, 1080p and 4K on the desktop GPU,
// and the GPU time per second each kind of game costs.
//
// The GPU harness is the gate's: frame-gen.cpp is compiled in with its main renamed.
#define main frame_gen_gate_main
#include "frame-gen.cpp"
#undef main

#include <cstdarg>

namespace {

constexpr double HOST_BASE = 100.0;
/// Host vblanks: 60 Hz, at an arbitrary phase to the guest's vsyncs.
constexpr double VBLANK_PHASE = 0.003;

double Rand(u64 i, u64 salt) {
    return Noise(i * 0x2545F491ULL + salt * 0x9E3779B1ULL + salt);
}

double Signed(u64 i, u64 salt) {
    return 2.0 * Rand(i, salt) - 1.0;
}

std::string Format(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

// ---------------------------------------------------------------------------------------------
// Game model
// ---------------------------------------------------------------------------------------------

/// Render time (seconds) of frame i, started at guest time t.
using RenderTime = std::function<double(u32 i, double t)>;

/// Uniform in base +- spread.
RenderTime Around(double base, double spread, u64 salt = 10) {
    return [=](u32 i, double) { return base + spread * Signed(i, salt); };
}

/// An unlocked game at fps.
RenderTime Fps(double fps, double spread) {
    return Around(1.0 / fps, spread);
}

/// Swings between the frame times of fps_low and fps_high over period seconds.
RenderTime Swing(double fps_low, double fps_high, double period, double spread) {
    const double slow = 1.0 / fps_low;
    const double fast = 1.0 / fps_high;
    const double mid = 0.5 * (slow + fast);
    const double amplitude = 0.5 * (slow - fast) - spread;
    return [=](u32 i, double t) {
        return mid + amplitude * std::sin(6.283185307179586 * t / period) + spread * Signed(i, 11);
    };
}

struct Game {
    std::string name;
    RenderTime render;
    u32 buffers{3};      ///< 2: the game waits for its frame's flip; 3: it renders one ahead
    u32 swap{1};         ///< minimum vsyncs between flips (1: unlocked, 2: 30 cap, 3: 20 cap)
    bool dynamic{};      ///< a late frame flips on the next vsync and the game does not wait
    double host_jitter{0.002};
    double spike_chance{}; ///< share of frames the host delivers late, by up to spike_len
    double spike_len{};
    double stall_at{-1.0}; ///< host stall: nothing arrives for stall_len, then a burst
    double stall_len{};
    double pause_at{-1.0}; ///< the game stops for pause_len (loading)
    double pause_len{};
    bool queue_times{true};
    bool anim_at_flip{}; ///< the game animates by the vsync its frame is shown on, not its start
    FrameGenMode mode{FrameGenMode::Auto};
    int want{-1};          ///< rate at the end: 30 or 60, -1 either, -2 anything (off too)
    u32 max_changes{};     ///< rate changes allowed after engaging
    double max_hold_share{0.005};
    double max_step_rms{2.0}; ///< ms
    double seconds{30.0};

    double EventAt() const {
        return stall_at >= 0.0 ? stall_at : pause_at;
    }
};

struct GameFrame {
    double start;   ///< guest time the game started it (input read then)
    double done;
    u64 vsync;      ///< guest vsync it was composed on
    double arrival; ///< host time it reached the present thread
    double queue;   ///< host time the game queued it
    double anim;    ///< game time its picture shows: start, or the flip vsync's time
};

/// The game's frames: guest vsyncs at k / 60, guest time t is host time HOST_BASE + t.
std::vector<GameFrame> Play(const Game& g) {
    std::vector<GameFrame> frames;
    double start = 0.0;
    bool paused = false;
    for (u32 i = 0; start < g.seconds; ++i) {
        double render = g.render(i, start);
        if (g.pause_at >= 0.0 && !paused && start >= g.pause_at) {
            render += g.pause_len;
            paused = true;
        }
        const double done = start + render;
        u64 vsync = static_cast<u64>(std::ceil(done * 60.0 - 1e-9));
        bool late = false;
        if (!frames.empty()) {
            const u64 earliest = frames.back().vsync + g.swap;
            late = vsync > earliest;
            vsync = std::max(vsync, earliest);
        }
        // The host renders the composed frame: 10 ms plus jitter, spikes and stalls; in order.
        double arrival = HOST_BASE + vsync / 60.0 + 0.010 + g.host_jitter * Rand(i, 1);
        if (g.spike_chance > 0.0 && Rand(i, 2) < g.spike_chance) {
            arrival += g.spike_len * (0.5 + 0.5 * Rand(i, 3));
        }
        if (g.stall_at >= 0.0) {
            const double from = HOST_BASE + g.stall_at;
            if (arrival >= from && arrival < from + g.stall_len) {
                arrival = from + g.stall_len;
            }
        }
        if (!frames.empty()) {
            arrival = std::max(arrival, frames.back().arrival);
        }
        frames.push_back({start, done, vsync, arrival, HOST_BASE + done,
                          g.anim_at_flip ? vsync / 60.0 : start});
        // The next frame needs a free buffer: double buffer, this frame's flip; triple, the
        // previous one's. Dynamic vsync: a late frame does not hold the game.
        if (g.dynamic && late) {
            start = done;
        } else if (frames.size() + 1 >= g.buffers) {
            start = std::max(done, frames[frames.size() + 1 - g.buffers].vsync / 60.0);
        } else {
            start = done;
        }
    }
    return frames;
}

double VBlank(double host_time) {
    const double phase = HOST_BASE + VBLANK_PHASE;
    return phase + std::ceil((host_time - phase) * 60.0 - 1e-9) / 60.0;
}

// ---------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------

double Mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (const double x : v) sum += x;
    return sum / v.size();
}

/// RMS around the mean.
double Spread(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const double mean = Mean(v);
    double sum = 0.0;
    for (const double x : v) sum += (x - mean) * (x - mean);
    return std::sqrt(sum / v.size());
}

double Rms(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (const double x : v) sum += x * x;
    return std::sqrt(sum / v.size());
}

double MaxAbs(const std::vector<double>& v) {
    double m = 0.0;
    for (const double x : v) m = std::max(m, std::abs(x));
    return m;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

double Median(std::vector<double> v) {
    return Percentile(std::move(v), 0.5);
}

struct Tally {
    u32 passed{};
    std::vector<std::string> failed;

    void Add(const std::string& name, const std::vector<std::string>& fails) {
        if (fails.empty()) {
            ++passed;
            return;
        }
        std::string line = name + ":";
        for (const std::string& f : fails) line += " " + f + ";";
        failed.push_back(line);
    }
};

// ---------------------------------------------------------------------------------------------
// Pacer cases
// ---------------------------------------------------------------------------------------------

struct Result {
    double fps{};
    u32 target{};
    double engage{-1.0}; ///< guest seconds
    u32 changes{};
    u32 outputs{};
    u32 blends{};
    u32 holds{};
    u32 backward{};
    u32 event_holds{};
    u32 off_slots{};  ///< passthrough vsyncs after settling (outside events)
    double step_rms{}; ///< ms
    double step_max{};
    double judder_fg{};   ///< ms: spread of display time minus the game time shown
    double judder_pass{};
    double lag_fg{};      ///< ms: from the input the newest frame shown read to the screen
    double lag_fg_p95{};
    double lag_pass{};
    double lag_pass_p95{};
    double delay_mean{};
    double delay_max{};
    bool queue_clock{};
    std::vector<std::string> fails;
};

bool InEvent(const Game& g, double t) {
    const double at = g.EventAt();
    return at >= 0.0 && t >= at - 0.2 && t < at + 2.5;
}

Result Simulate(const Game& g) {
    const std::vector<GameFrame> frames = Play(g);
    Result r;
    r.fps = (frames.size() - 1) /
            (static_cast<double>(frames.back().vsync - frames.front().vsync) / 60.0);
    FrameGenPacer pacer;
    struct Held {
        double content;
        double anim;
        double start;
        u64 serial;
    };
    std::vector<Held> history; // the last FrameGen::NUM_SLOTS frames
    u64 serial = 1;
    size_t next = 0;
    u32 target = 0;
    std::optional<double> last_shown;
    std::vector<double> steps, judder, lag, delays;
    double slot = VBlank(frames.front().arrival);
    const double end = frames.back().arrival;
    while (slot < end) {
        while (next < frames.size() && frames[next].arrival <= slot) {
            const GameFrame& f = frames[next++];
            const u32 rate = pacer.OnFrame(g.mode, 1000 + f.vsync, f.arrival,
                                           g.queue_times ? f.queue : 0.0);
            if (r.engage >= 0.0 && rate != target) ++r.changes;
            if (r.engage < 0.0 && rate != 0) r.engage = f.arrival - HOST_BASE;
            target = rate;
            if (rate != 0) {
                history.push_back({pacer.Content(), f.anim, f.start, serial++});
                if (history.size() > FrameGen::NUM_SLOTS) history.erase(history.begin());
            } else {
                // Passthrough: FrameGen::Break forgets the history.
                if (!history.empty()) pacer.Reset();
                history.clear();
                ++serial;
            }
        }
        const double t = slot - HOST_BASE;
        const bool settled = r.engage >= 0.0 && t > std::max(4.0, r.engage + 2.0);
        if (target == 0 || history.empty()) {
            if (settled && !InEvent(g, t)) ++r.off_slots;
            last_shown.reset();
            slot += 1.0 / 60.0;
            continue;
        }
        const double period = 1.0 / target;
        std::vector<FrameGenPacer::Frame> list;
        for (u32 i = 0; i < history.size(); ++i) {
            list.push_back({.content = history[i].content, .serial = history[i].serial, .slot = i});
        }
        const FrameGenPacer::Choice choice = pacer.Pick(slot, list);
        double shown = 0.0;
        double newest = 0.0;
        if (choice.kind == Kind::Blend) {
            shown = history[choice.a].anim +
                    choice.phase * (history[choice.b].anim - history[choice.a].anim);
            newest = history[choice.b].start;
        } else {
            // None: FrameGen::Next shows the newest frame.
            const u32 index = choice.kind == Kind::Real ? choice.a
                                                        : static_cast<u32>(history.size() - 1);
            shown = history[index].anim;
            newest = history[index].start;
        }
        if (settled && last_shown) {
            if (shown < *last_shown - 1e-9) ++r.backward;
            const bool hold = shown <= *last_shown + 1e-9;
            if (InEvent(g, t)) {
                r.event_holds += hold ? 1 : 0;
            } else {
                ++r.outputs;
                r.holds += hold ? 1 : 0;
                r.blends += choice.kind == Kind::Blend ? 1 : 0;
                steps.push_back(shown - *last_shown - period);
                // The output reaches the screen one slot later: the swapchain holds one frame.
                judder.push_back(slot + period - HOST_BASE - shown);
                lag.push_back(slot + period - HOST_BASE - newest);
                delays.push_back(pacer.Delay());
            }
        }
        last_shown = shown;
        slot += period;
    }
    // The same frames presented as they come: each on the host vblank after it arrives (no
    // queue: the best case for passthrough).
    const double settle = std::max(4.0, r.engage >= 0.0 ? r.engage + 2.0 : 4.0);
    std::vector<double> pass_lag, pass_judder;
    for (const GameFrame& f : frames) {
        const double t = f.arrival - HOST_BASE;
        if (t > settle && !InEvent(g, t)) {
            pass_lag.push_back(VBlank(f.arrival) - HOST_BASE - f.start);
        }
    }
    size_t shown_count = 0;
    for (u64 k = 0;; ++k) {
        const double vblank = VBlank(HOST_BASE + settle) + k / 60.0;
        if (vblank >= end) break;
        while (shown_count < frames.size() &&
               VBlank(frames[shown_count].arrival) <= vblank + 1e-9) {
            ++shown_count;
        }
        if (shown_count == 0 || InEvent(g, vblank - HOST_BASE)) continue;
        pass_judder.push_back(vblank - HOST_BASE - frames[shown_count - 1].anim);
    }
    r.target = target;
    r.queue_clock = pacer.QueueClock();
    r.step_rms = 1000.0 * Rms(steps);
    r.step_max = 1000.0 * MaxAbs(steps);
    r.judder_fg = 1000.0 * Spread(judder);
    r.judder_pass = 1000.0 * Spread(pass_judder);
    r.lag_fg = 1000.0 * Mean(lag);
    r.lag_fg_p95 = 1000.0 * Percentile(lag, 0.95);
    r.lag_pass = 1000.0 * Mean(pass_lag);
    r.lag_pass_p95 = 1000.0 * Percentile(pass_lag, 0.95);
    r.delay_mean = 1000.0 * Mean(delays);
    r.delay_max = 1000.0 * MaxAbs(delays);
    return r;
}

/// The criteria: the rate the game should get, engaged within 3.5 s, no flapping, never a step
/// back, held frames under the case's share, even motion (step error rms), at most half the
/// judder of presenting the frames as they come, and added lag within one game frame (the pair
/// interpolation needs) plus one 30 Hz output frame.
void Judge(const Game& g, Result& r) {
    auto fail = [&](const std::string& what) { r.fails.push_back(what); };
    if (g.want > 0 && r.target != static_cast<u32>(g.want)) {
        fail(Format("rate %u, want %d", r.target, g.want));
    }
    if (g.want == -1 && r.target == 0) fail("off at the end");
    if (g.want != -2 && (r.engage < 0.0 || r.engage > 3.5)) {
        fail(Format("engaged at %.1f s", r.engage));
    }
    if (r.changes > g.max_changes) fail(Format("%u rate changes", r.changes));
    if (r.backward != 0) fail(Format("%u steps back", r.backward));
    if (r.outputs == 0) {
        if (g.want != -2) fail("no settled output");
        return;
    }
    const double hold_limit = std::max(2.0, g.max_hold_share * r.outputs);
    if (r.holds > hold_limit) fail(Format("%u held frames (limit %.0f)", r.holds, hold_limit));
    if (r.step_rms > g.max_step_rms) {
        fail(Format("step error rms %.2f ms (limit %.1f)", r.step_rms, g.max_step_rms));
    }
    if (r.judder_fg > 0.5 * r.judder_pass) {
        fail(Format("judder %.2f ms vs passthrough %.2f", r.judder_fg, r.judder_pass));
    }
    const double budget = 1000.0 * (1.0 / r.fps + 1.0 / 30.0);
    if (r.lag_fg - r.lag_pass > budget) {
        fail(Format("added lag %.1f ms (budget %.1f)", r.lag_fg - r.lag_pass, budget));
    }
}

void PrintHeader() {
    std::printf("  %-52s %5s %4s %6s %3s %4s %5s %13s %13s %24s %11s %5s\n", "game", "fps", "rate",
                "engage", "chg", "gen", "holds", "step rms/max", "judder fg/pt", "lag fg/pt (added)",
                "delay avg/mx", "clock");
}

void PrintRow(const Game& g, const Result& r) {
    std::printf("  %-52s %5.1f %4u %5.1fs %3u %3.0f%% %5u %5.2f/%6.2f %5.2f/%6.2f %5.1f/%5.1f (%+6.1f) "
                "%5.1f/%5.1f %5s  %s",
                g.name.c_str(), r.fps, r.target, r.engage, r.changes,
                r.outputs ? 100.0 * r.blends / r.outputs : 0.0, r.holds, r.step_rms, r.step_max,
                r.judder_fg, r.judder_pass, r.lag_fg, r.lag_pass, r.lag_fg - r.lag_pass,
                r.delay_mean, r.delay_max, r.queue_clock ? "queue" : "vsync",
                r.fails.empty() ? "PASS" : "FAIL");
    if (g.EventAt() >= 0.0) std::printf(" (event: %u held)", r.event_holds);
    if (r.off_slots) std::printf(" (%u vsyncs passthrough)", r.off_slots);
    for (const std::string& f : r.fails) std::printf(" [%s]", f.c_str());
    std::printf("\n");
}

std::vector<Game> LowGames() {
    return {
        {.name = "A1  20 fps locked, double buffer (swap 3)", .render = Around(0.032, 0.008),
         .buffers = 2, .swap = 3, .want = 30},
        {.name = "A2  15 fps locked, double buffer (swap 4)", .render = Around(0.045, 0.010),
         .buffers = 2, .swap = 4, .want = 30},
        {.name = "A3  20 fps locked, triple buffer, 35-50 ms render",
         .render = Around(0.0425, 0.0075), .buffers = 3, .swap = 3, .want = 30},
        {.name = "A4  18 fps unlocked", .render = Fps(18.0, 0.003), .want = 30},
        {.name = "A5  16-20 fps swinging, unlocked", .render = Swing(16.0, 20.0, 5.0, 0.002),
         .want = 30},
        {.name = "A6  20 fps locked, 15 ms host jitter", .render = Around(0.032, 0.008),
         .buffers = 2, .swap = 3, .host_jitter = 0.015, .want = 30},
        {.name = "A7  20 fps locked, 5% host spikes up to 80 ms", .render = Around(0.032, 0.008),
         .buffers = 2, .swap = 3, .spike_chance = 0.05, .spike_len = 0.080, .want = 30,
         .max_hold_share = 0.03},
        {.name = "A8  20 fps locked, 400 ms host stall", .render = Around(0.032, 0.008),
         .buffers = 2, .swap = 3, .stall_at = 15.0, .stall_len = 0.4, .want = 30,
         .max_changes = 2},
        {.name = "A9  30 cap double buffer, GPU-bound 45-60 ms",
         .render = Around(0.0525, 0.0075), .buffers = 2, .swap = 2, .want = 30},
        {.name = "A10 20 fps locked, 1 s loading pause", .render = Around(0.032, 0.008),
         .buffers = 2, .swap = 3, .pause_at = 15.0, .pause_len = 1.0, .want = 30,
         .max_changes = 2},
    };
}

std::vector<Game> HighGames() {
    return {
        {.name = "B1  45 fps unlocked", .render = Fps(45.0, 0.0015), .want = 60},
        {.name = "B2  50 fps unlocked", .render = Fps(50.0, 0.0015), .want = 60},
        {.name = "B3  48 fps unlocked, +-3 ms render", .render = Fps(48.0, 0.003), .want = 60},
        {.name = "B4  45-50 fps swinging", .render = Swing(45.0, 50.0, 6.0, 0.001), .want = 60},
        {.name = "B5  50 fps unlocked, no queue times", .render = Fps(50.0, 0.0015),
         .queue_times = false, .want = 60},
        {.name = "B6  60 cap double buffer, 14-18 ms (60/30 mix)", .render = Around(0.016, 0.002),
         .buffers = 2, .swap = 1, .want = 60},
        {.name = "B7  48 fps, 8 ms host jitter", .render = Fps(48.0, 0.0015),
         .host_jitter = 0.008, .want = 60},
        {.name = "B8  50 fps, 5% host spikes up to 40 ms", .render = Fps(50.0, 0.0015),
         .spike_chance = 0.05, .spike_len = 0.040, .want = 60, .max_hold_share = 0.03},
        {.name = "B9  50-57 fps around the 56 fps threshold",
         .render = Swing(50.0, 57.0, 8.0, 0.001), .want = -2, .max_changes = 2},
        {.name = "B10 45 fps, 300 ms host stall", .render = Fps(45.0, 0.0015), .stall_at = 15.0,
         .stall_len = 0.3, .want = 60, .max_changes = 2},
    };
}

std::vector<Game> MidGames() {
    return {
        {.name = "C1  40 fps unlocked", .render = Fps(40.0, 0.002), .want = 60},
        {.name = "C2  35 fps unlocked", .render = Fps(35.0, 0.002), .want = 60},
        {.name = "C3  38 fps unlocked, +-4 ms render", .render = Fps(38.0, 0.004), .want = 60},
        {.name = "C4  35-40 fps swinging", .render = Swing(35.0, 40.0, 6.0, 0.0015), .want = 60},
        {.name = "C5  40 fps triple buffer, 60 cap, 22-28 ms",
         .render = Around(0.025, 0.003), .buffers = 3, .swap = 1, .want = 60},
        {.name = "C6  36 fps, vsync clock only (no queue times)", .render = Fps(36.0, 0.002),
         .queue_times = false, .want = 60},
        {.name = "C7  40 fps, 10 ms host jitter", .render = Fps(40.0, 0.002),
         .host_jitter = 0.010, .want = 60},
        {.name = "C8  37 fps, 5% host spikes up to 50 ms", .render = Fps(37.0, 0.002),
         .spike_chance = 0.05, .spike_len = 0.050, .want = 60, .max_hold_share = 0.03},
        {.name = "C9  26-40 fps crossing the 28 fps threshold",
         .render = Swing(26.0, 40.0, 10.0, 0.0015), .want = -1, .max_changes = 4},
        {.name = "C10 35-40 fps swinging, mode 60", .render = Swing(35.0, 40.0, 6.0, 0.0015),
         .mode = FrameGenMode::Target60, .want = 60},
    };
}

std::vector<Game> BufferGames() {
    return {
        {.name = "D1  double buffer, 30 cap, 30-38 ms render", .render = Around(0.034, 0.004),
         .buffers = 2, .swap = 2, .want = -1},
        {.name = "D2  triple buffer, 30 cap, 30-38 ms render", .render = Around(0.034, 0.004),
         .buffers = 3, .swap = 2, .want = -1},
        {.name = "D3  dynamic vsync, 30 cap, 30-38 ms render", .render = Around(0.034, 0.004),
         .buffers = 2, .swap = 2, .dynamic = true, .want = -1},
        {.name = "D4  double buffer, 20 cap, 45-55 ms render", .render = Around(0.050, 0.005),
         .buffers = 2, .swap = 3, .want = 30},
        {.name = "D5  dynamic vsync 30, frequent misses, 6 ms jitter",
         .render = Around(0.033, 0.006), .buffers = 2, .swap = 2, .dynamic = true,
         .host_jitter = 0.006, .want = -1},
    };
}

void PacerGroup(const char* title, const std::vector<Game>& games, Tally& tally) {
    std::printf("\n%s\n", title);
    PrintHeader();
    for (const Game& g : games) {
        Result r = Simulate(g);
        Judge(g, r);
        PrintRow(g, r);
        tally.Add(g.name, r.fails);
    }
}

void PacerSuite(Tally& tally) {
    std::printf("PACER (CPU). gen: generated share of the outputs; holds: a frame shown again; step: "
                "error of each output's step in game time (ms); judder: spread of display time - "
                "game time shown, frame generation (fg) vs presenting as they come (pt); lag: "
                "input (frame start) to screen, ms; clock: content clock at the end.\n");
    PacerGroup("15-20 fps -> 30", LowGames(), tally);
    PacerGroup("45-50 fps -> 60", HighGames(), tally);
    PacerGroup("35-40 fps", MidGames(), tally);
    PacerGroup("double / triple buffer, dynamic vsync", BufferGames(), tally);
    // The same games animating by the vsync each frame is shown on (a fixed step per vsync):
    // what is left is the pacer's own error, not the one-frame shift of an input-time clock.
    std::vector<Game> at_flip = BufferGames();
    for (Game& g : at_flip) {
        g.name.replace(0, 1, "E");
        g.name += " (anim at flip)";
        g.anim_at_flip = true;
    }
    PacerGroup("double / triple buffer, dynamic vsync, game animating by its flip vsync", at_flip,
               tally);
}

/// CPU cost of the pacer: OnFrame per game frame and Pick per output.
void CpuPerf() {
    constexpr u32 FRAMES = 200000;
    using Clock = std::chrono::steady_clock;
    FrameGenPacer pacer;
    auto start = Clock::now();
    for (u32 i = 0; i < FRAMES; ++i) {
        const u64 vsync = 1000 + 3ULL * i;
        (void)pacer.OnFrame(FrameGenMode::Auto, vsync, HOST_BASE + vsync / 60.0 + 0.004 * Noise(i),
                            HOST_BASE + vsync / 60.0 - 0.012);
    }
    const double on_frame_us =
        std::chrono::duration<double, std::micro>(Clock::now() - start).count() / FRAMES;
    FrameGenPacer picker;
    std::array<FrameGenPacer::Frame, 4> history{};
    u32 count = 0;
    double slot = 0.0;
    double pick_us = 0.0;
    u32 picks = 0;
    for (u32 i = 0; i < FRAMES; ++i) {
        const u64 vsync = 1000 + 3ULL * i;
        const double arrival = HOST_BASE + vsync / 60.0 + 0.004 * Noise(i);
        (void)picker.OnFrame(FrameGenMode::Target30, vsync, arrival);
        if (count == history.size()) {
            std::rotate(history.begin(), history.begin() + 1, history.end());
            --count;
        }
        history[count] = {.content = picker.Content(), .serial = i + 1ULL, .slot = count};
        ++count;
        if (slot == 0.0) slot = arrival;
        const auto pick_start = Clock::now();
        while (slot < arrival + 0.05) {
            (void)picker.Pick(slot, std::span<const FrameGenPacer::Frame>(history.data(), count));
            slot += 1.0 / 30.0;
            ++picks;
        }
        pick_us += std::chrono::duration<double, std::micro>(Clock::now() - pick_start).count();
    }
    std::printf("\nPACER CPU: OnFrame %.3f us per game frame, Pick %.3f us per output (desktop "
                "CPU)\n",
                on_frame_us, pick_us / picks);
}

// ---------------------------------------------------------------------------------------------
// GPU: interpolation at the phases each group uses
// ---------------------------------------------------------------------------------------------

double SafeObjectX(const Picture& image) {
    try {
        return ObjectX(image);
    } catch (const std::exception&) {
        return std::nan("");
    }
}

struct ArtifactCase {
    const char* name;
    u32 width;
    u32 height;
    double dx;  ///< object motion per game frame, 1280 units
    double pan; ///< background motion per game frame, 1280 units
    std::vector<float> phases;
};

/// Pass: the object within 3 units of its place, band error (no pan) or image error (pan) well
/// under a crossfade's, under 0.4% of the pixels off by more than 40 luma (ghosts, tears).
void Artifacts(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
               DescriptorRing& ring, SamplerHeap& samplers, Runner& runner, const ArtifactCase& c,
               Tally& tally) {
    FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), c.width, c.height};
    Harness harness{device, scheduler, runner, c.width, c.height};
    u64 vsync = 600;
    double time = 50.0;
    Activate(fg, vsync, time);
    const double x_a = 300.0;
    const Picture a = Draw(Background(c.width, c.height, false, 0.0), x_a);
    const Picture b = Draw(Background(c.width, c.height, false, c.pan), x_a + c.dx);
    const u32 slot_a = harness.Ingest(fg, harness.Upload(a), vsync);
    const u32 slot_b = harness.Ingest(fg, harness.Upload(b), vsync + 3);
    std::vector<std::string> fails;
    for (const float phase : c.phases) {
        const Picture out = harness.Output(fg, Blend(slot_a, slot_b, phase));
        const Picture truth =
            Draw(Background(c.width, c.height, false, c.pan * phase), x_a + c.dx * phase);
        const Picture fade = Crossfade(a, b, phase);
        const double position = SafeObjectX(out);
        const double offset = position - (x_a + c.dx * phase);
        const double band = BandError(out, truth, x_a, x_a + c.dx);
        const double fade_band = BandError(fade, truth, x_a, x_a + c.dx);
        const double image = ImageError(out, truth);
        const double fade_image = ImageError(fade, truth);
        const double bad = BadPixels(out, truth);
        const double fade_bad = BadPixels(fade, truth);
        std::vector<std::string> here;
        if (!(std::abs(offset) < 3.0)) here.push_back(Format("object off by %.1f", offset));
        if (c.pan == 0.0 && band > 0.35 * fade_band) here.push_back("band error");
        if (c.pan != 0.0 && image > 0.5 * fade_image) here.push_back("image error");
        if (bad > 0.004) here.push_back("ghosts/tears");
        std::printf("  %-40s %4ux%-4u phase %.2f: object off %6.2f, band %6.2f (fade %6.2f), image "
                    "%5.2f (fade %5.2f), bad px %.3f%% (fade %.3f%%)  %s\n",
                    c.name, c.width, c.height, phase, offset, band, fade_band, image, fade_image,
                    100.0 * bad, 100.0 * fade_bad, here.empty() ? "PASS" : "FAIL");
        for (const std::string& h : here) fails.push_back(Format("phase %.2f ", phase) + h);
    }
    scheduler.Finish();
    tally.Add(std::string{"GPU "} + c.name, fails);
}

void StillAndCut(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                 DescriptorRing& ring, SamplerHeap& samplers, Runner& runner, u32 width,
                 u32 height, Tally& tally) {
    FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), width, height};
    Harness harness{device, scheduler, runner, width, height};
    u64 vsync = 600;
    double time = 50.0;
    Activate(fg, vsync, time);
    const Picture still = Draw(Background(width, height, false), 400.0);
    std::vector<std::string> fails;
    {
        const u32 s0 = harness.Ingest(fg, harness.Upload(still), vsync);
        const u32 s1 = harness.Ingest(fg, harness.Upload(still), vsync + 3);
        vsync += 6;
        double worst = 0.0;
        for (const float phase : {0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f}) {
            worst = std::max(worst, ImageError(harness.Output(fg, Blend(s0, s1, phase)), still));
        }
        std::printf("  %-40s %4ux%-4u still pair, worst image error %.3f  %s\n", "still pair",
                    width, height, worst, worst < 0.25 ? "PASS" : "FAIL");
        if (worst >= 0.25) fails.push_back("still pair changed");
    }
    {
        const Picture cut = Draw(Background(width, height, true), 900.0);
        const u32 s0 = harness.Ingest(fg, harness.Upload(still), vsync);
        const u32 s1 = harness.Ingest(fg, harness.Upload(cut), vsync + 3);
        vsync += 6;
        double worst = 0.0;
        for (const float phase : {0.25f, 1.0f / 3.0f, 0.45f}) {
            worst = std::max(worst, ImageError(harness.Output(fg, Blend(s0, s1, phase)), still));
        }
        for (const float phase : {0.55f, 2.0f / 3.0f, 0.75f}) {
            worst = std::max(worst, ImageError(harness.Output(fg, Blend(s0, s1, phase)), cut));
        }
        std::printf("  %-40s %4ux%-4u scene cut, worst error to the nearer frame %.3f  %s\n",
                    "scene cut", width, height, worst, worst < 0.25 ? "PASS" : "FAIL");
        if (worst >= 0.25) fails.push_back("cut blended");
    }
    scheduler.Finish();
    tally.Add("GPU still pair and cut", fails);
}

// ---------------------------------------------------------------------------------------------
// GPU: whole path on a virtual clock
// ---------------------------------------------------------------------------------------------

/// A game from the pacer cases through OnFrame -> ingest -> Next -> output -> Unpin at 1080p, the
/// object moving 480 units per second of the game's time (back and forth over 100-1000). Pass: step
/// error rms under 2.5 ms (1 px of measurement is ~1.4 ms), held frames under 1%, under 0.4% bad
/// pixels against the object drawn where it was measured, a generated share over 30%.
void VirtualRun(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                DescriptorRing& ring, SamplerHeap& samplers, Runner& runner, Game game,
                Tally& tally) {
    constexpr u32 WIDTH = 1920;
    constexpr u32 HEIGHT = 1080;
    constexpr double SPEED = 480.0;
    constexpr double RANGE = 900.0;
    game.seconds = 7.0;
    SetFrameGenMode(game.mode);
    const std::vector<GameFrame> frames = Play(game);
    FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), WIDTH, HEIGHT};
    Harness harness{device, scheduler, runner, WIDTH, HEIGHT};
    const Picture background = Background(WIDTH, HEIGHT, false);
    const double t0 = frames.front().start;
    auto place = [&](double anim) {
        const double d = std::fmod(SPEED * (anim - t0), 2.0 * RANGE);
        return 100.0 + (d < RANGE ? d : 2.0 * RANGE - d);
    };
    size_t next = 0;
    double slot = VBlank(frames.front().arrival);
    u32 rate = 0;
    double engage = -1.0;
    struct Sample {
        double x;
        double bad;
        bool blend;
        u32 target;
    };
    std::vector<Sample> samples;
    while (next < frames.size()) {
        if (frames[next].arrival <= slot) {
            const GameFrame& f = frames[next++];
            rate = fg.OnFrame(1000 + f.vsync, true, f.arrival, game.queue_times ? f.queue : 0.0);
            if (rate == 0) {
                fg.Break();
                continue;
            }
            if (engage < 0.0) engage = f.arrival;
            harness.Ingest(fg, harness.Upload(Draw(background, place(f.anim))), 1000 + f.vsync);
            continue;
        }
        if (rate == 0 || !fg.HasFrames()) {
            slot += 1.0 / 60.0;
            continue;
        }
        const u32 target = fg.Target();
        const FrameGen::Output out = fg.Next(slot);
        if (out.kind != Kind::None) {
            u64 fence_value{};
            const Picture image = harness.Output(fg, out, &fence_value);
            fg.Unpin(out, fence_value);
            if (slot - engage > 2.0) {
                const double x = SafeObjectX(image);
                const double bad = std::isnan(x) ? 1.0 : BadPixels(image, Draw(background, x));
                samples.push_back({x, bad, out.kind == Kind::Blend, target});
            }
        }
        slot += 1.0 / target;
    }
    scheduler.Finish();
    std::vector<double> steps;
    u32 holds = 0;
    u32 blends = 0;
    u32 lost = 0;
    double bad_max = 0.0;
    double bad_sum = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        if (std::isnan(s.x)) {
            ++lost;
            continue;
        }
        bad_max = std::max(bad_max, s.bad);
        bad_sum += s.bad;
        blends += s.blend ? 1 : 0;
        if (i == 0 || std::isnan(samples[i - 1].x)) continue;
        const Sample& p = samples[i - 1];
        // Steps through a turn are shorter than the speed says.
        if (s.x < 140.0 || s.x > 960.0 || p.x < 140.0 || p.x > 960.0) continue;
        const double step = std::abs(s.x - p.x);
        holds += step < 0.5 ? 1 : 0;
        steps.push_back((step - SPEED / s.target) / SPEED);
    }
    const double rms = 1000.0 * Rms(steps);
    const double worst = 1000.0 * MaxAbs(steps);
    const double gen = samples.empty() ? 0.0 : static_cast<double>(blends) / samples.size();
    std::vector<std::string> fails;
    if (engage < 0.0 || samples.size() < 30) fails.push_back("too few outputs");
    if (lost) fails.push_back(Format("object lost on %u outputs", lost));
    if (rms > 2.5) fails.push_back(Format("step error rms %.2f ms", rms));
    if (holds > std::max<size_t>(1, steps.size() / 100)) fails.push_back(Format("%u held", holds));
    if (bad_max > 0.004) fails.push_back(Format("bad pixels %.3f%%", 100.0 * bad_max));
    if (gen < 0.3) fails.push_back(Format("generated share %.0f%%", 100.0 * gen));
    std::printf("  %-52s rate %2u, %3zu outputs, gen %3.0f%%, step rms %.2f / max %5.2f ms, "
                "held %u, bad px avg %.3f%% max %.3f%%  %s\n",
                game.name.c_str(), samples.empty() ? 0 : samples.back().target, samples.size(),
                100.0 * gen, rms, worst, holds,
                samples.empty() ? 0.0 : 100.0 * bad_sum / samples.size(), 100.0 * bad_max,
                fails.empty() ? "PASS" : "FAIL");
    for (const std::string& f : fails) std::printf("      [%s]\n", f.c_str());
    tally.Add("GPU path " + game.name, fails);
}

void GpuSuite(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
              DescriptorRing& ring, SamplerHeap& samplers, Runner& runner, Tally& tally) {
    const float third = 1.0f / 3.0f;
    const float two_thirds = 2.0f / 3.0f;
    const std::vector<ArtifactCase> cases{
        {"20->30, 24/frame", 1920, 1080, 24.0, 0.0, {third, two_thirds}},
        {"20->30, 64/frame (fast)", 1920, 1080, 64.0, 0.0, {third, two_thirds}},
        {"15->30, 128/frame (very fast)", 1920, 1080, 128.0, 0.0, {0.5f}},
        {"15->30, 200/frame (extreme)", 1920, 1080, 200.0, 0.0, {0.5f}},
        {"20->30, 40/frame over a 24 pan", 1920, 1080, 40.0, 24.0, {third, two_thirds}},
        {"45->60, 12/frame", 1920, 1080, 12.0, 0.0, {0.25f, 0.5f, 0.75f}},
        {"45->60, 32/frame (fast)", 1920, 1080, 32.0, 0.0, {0.25f, 0.5f, 0.75f}},
        {"40->60, 20/frame over a 10 pan", 1920, 1080, 20.0, 10.0, {third, two_thirds}},
        {"4K 20->30, 64/frame", 3840, 2160, 64.0, 0.0, {third, two_thirds}},
        {"4K 45->60, 32/frame over a 16 pan", 3840, 2160, 32.0, 16.0, {0.25f, 0.5f, 0.75f}},
        // Camera turns: the background pans under a character that stays put.
        {"20->30, camera turn 64/frame", 1920, 1080, 0.0, 64.0, {third, two_thirds}},
        {"20->30, camera turn 128/frame", 1920, 1080, 0.0, 128.0, {third, two_thirds}},
        {"15->30, camera turn 200/frame", 1920, 1080, 0.0, 200.0, {0.5f}},
        {"20->30, 48/frame in a 96 turn", 1920, 1080, 48.0, 96.0, {third, two_thirds}},
        {"4K 20->30, camera turn 128/frame", 3840, 2160, 0.0, 128.0, {third, two_thirds}},
    };
    std::printf("\nGPU INTERPOLATION (motion in 1280-wide units per game frame; fade = crossfade "
                "of the two frames, the no-motion baseline)\n");
    SetFrameGenMode(FrameGenMode::Target30);
    for (const ArtifactCase& c : cases) {
        Artifacts(device, scheduler, compiler, ring, samplers, runner, c, tally);
    }
    StillAndCut(device, scheduler, compiler, ring, samplers, runner, 1920, 1080, tally);

    std::printf("\nGPU WHOLE PATH (1080p, ingest -> Next -> output, motion measured on the "
                "images)\n");
    const std::vector<Game> low = LowGames();
    const std::vector<Game> high = HighGames();
    const std::vector<Game> mid = MidGames();
    const std::vector<Game> buffers = BufferGames();
    for (const Game& g : {low[3], low[0], low[8], high[2], mid[2], buffers[4]}) {
        VirtualRun(device, scheduler, compiler, ring, samplers, runner, g, tally);
    }
    SetFrameGenMode(FrameGenMode::Target30);
}

// ---------------------------------------------------------------------------------------------
// GPU timing
// ---------------------------------------------------------------------------------------------

struct GpuCost {
    double ingest; ///< ms per game frame
    double compose; ///< ms per generated frame
};

GpuCost TimeGpu(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                DescriptorRing& ring, SamplerHeap& samplers, Runner& runner, u32 width,
                u32 height) {
    FrameGen fg{device, scheduler, compiler, ring, samplers, runner.Fence(), width, height};
    Harness harness{device, scheduler, runner, width, height};
    u64 vsync = 600;
    double time = 50.0;
    Activate(fg, vsync, time);
    const Picture background = Background(width, height, false);
    const std::array<ID3D12Resource*, 2> images{harness.Upload(Draw(background, 300.0)),
                                                harness.Upload(Draw(background, 340.0))};
    u32 slot_a = harness.Ingest(fg, images[0], vsync);
    u32 slot_b = harness.Ingest(fg, images[1], vsync + 3);
    vsync += 6;
    constexpr u32 COUNT = 30;
    std::vector<double> ingests, composes;
    for (u32 round = 0; round < 6; ++round) {
        scheduler.Finish();
        auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < COUNT; ++i) {
            const std::optional<u32> slot = fg.RecordIngest(images[i % 2]);
            Require(slot.has_value(), "ingest skipped");
            const u64 tick = scheduler.Flush();
            fg.Commit(*slot, tick, vsync);
            vsync += 3;
            slot_a = slot_b;
            slot_b = *slot;
        }
        scheduler.Finish();
        const double ingest = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count() /
                              COUNT;
        start = std::chrono::steady_clock::now();
        runner.Run([&](ID3D12GraphicsCommandList* list) {
            for (u32 i = 0; i < COUNT; ++i) {
                harness.RecordOutput(fg, list, Blend(slot_a, slot_b, 0.5f));
            }
        });
        const double compose = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - start)
                                   .count() /
                               COUNT;
        if (round > 0) { // the first round warms up
            ingests.push_back(ingest);
            composes.push_back(compose);
        }
    }
    return {Median(ingests), Median(composes)};
}

void GpuPerf(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
             DescriptorRing& ring, SamplerHeap& samplers, Runner& runner) {
    SetFrameGenMode(FrameGenMode::Target30);
    std::printf("GPU TIMING (desktop GPU, wall clock, median of 5 rounds of 30)\n");
    struct Size {
        const char* name;
        u32 width;
        u32 height;
        GpuCost cost{};
    };
    std::array<Size, 3> sizes{{{"720p", 1280, 720}, {"1080p", 1920, 1080}, {"4K", 3840, 2160}}};
    for (Size& s : sizes) {
        s.cost = TimeGpu(device, scheduler, compiler, ring, samplers, runner, s.width, s.height);
        std::printf("  %-6s ingest %.3f ms per game frame, compose %.3f ms per generated frame\n",
                    s.name, s.cost.ingest, s.cost.compose);
    }
    // GPU time per second: ingest on every game frame, compose on every generated output.
    std::printf("\nGPU BUDGET per kind of game (generated share from the pacer runs)\n");
    std::printf("  %-52s %5s %4s %6s %20s %20s\n", "game", "fps", "rate", "gen/s",
                "1080p ms/s (% GPU)", "4K ms/s (% GPU)");
    const std::vector<Game> low = LowGames();
    const std::vector<Game> high = HighGames();
    const std::vector<Game> mid = MidGames();
    for (const Game& g : {low[1], low[0], low[3], mid[1], mid[0], high[0], high[1]}) {
        const Result r = Simulate(g);
        const double gen = r.outputs ? r.target * static_cast<double>(r.blends) / r.outputs : 0.0;
        const double hd = sizes[1].cost.ingest * r.fps + sizes[1].cost.compose * gen;
        const double uhd = sizes[2].cost.ingest * r.fps + sizes[2].cost.compose * gen;
        std::printf("  %-52s %5.1f %4u %6.1f %10.1f (%5.1f%%) %10.1f (%5.1f%%)\n", g.name.c_str(),
                    r.fps, r.target, gen, hd, hd / 10.0, uhd, uhd / 10.0);
    }
}

} // namespace

int main(int argc, char** argv) try {
    const bool perf = argc > 1 && std::string{argv[1]} == "perf";
    // "gpu": the GPU cases alone.
    const bool gpu_only = argc > 1 && std::string{argv[1]} == "gpu";
    Common::Log::Initialize();
    Tally tally;
    if (!perf) {
        if (!gpu_only) {
            PacerSuite(tally);
            CpuPerf();
        }
        // The hardware GPU with the debug layer (the translated shaders are SM 6.4).
        Settings::values.renderer_debug = true;
    }
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    if (!perf) {
        ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
        messages->ClearStoredMessages();
        failure_messages = messages;
    }
    Scheduler scheduler{device};
    ShaderCompiler compiler;
    DescriptorRing ring{device.Get(), scheduler, 64 * 1024};
    SamplerHeap samplers{device.Get(), scheduler};
    Runner runner{device};
    if (perf) {
        GpuPerf(device, scheduler, compiler, ring, samplers, runner);
        scheduler.Finish();
        return 0;
    }
    GpuSuite(device, scheduler, compiler, ring, samplers, runner, tally);
    scheduler.Finish();
    tally.Add("debug layer", DebugErrors(messages.Get())
                                 ? std::vector<std::string>{"debug layer errors"}
                                 : std::vector<std::string>{});
    std::printf("\nSUMMARY: %u passed, %zu failed\n", tally.passed, tally.failed.size());
    for (const std::string& line : tally.failed) std::printf("  FAIL %s\n", line.c_str());
    return tally.failed.empty() ? 0 : 2;
} catch (const std::exception& e) {
    std::cerr << "ERROR frame generation suite: " << e.what() << '\n';
    DebugErrors(failure_messages.Get());
    return 1;
}
