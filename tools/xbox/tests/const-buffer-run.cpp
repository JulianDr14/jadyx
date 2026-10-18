// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// CB_DATA runs: DmaPusher's word-by-word loop and the same loop with ConstBufferRunLength must
// leave identical registers, shadow RAM, constant buffer memory and DMA state, for every packet
// mode, every shadow RAM mode, and however the packets are split across command lists.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "video_core/engines/const_buffer_run.h"

using Tegra::Engines::ConstBufferRunLength;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// Stands in for MAXWELL3D_REG_INDEX(const_buffer.buffer); the rule only needs the range.
constexpr std::uint32_t CB_FIRST = 0x8E4;
constexpr std::uint32_t NUM_REGS = 0xE00;

enum class Mode { Increasing, NonIncreasing, IncreaseOnce };

struct Packet {
    Mode mode;
    std::uint32_t method;
    std::vector<std::uint32_t> words;
};

enum class Shadow { Off, Track, Replay };

/// Maxwell3D reduced to what CB_DATA touches: registers, shadow RAM, the bound buffer and its
/// offset.
struct Engine {
    std::vector<std::uint32_t> regs = std::vector<std::uint32_t>(NUM_REGS);
    std::vector<std::uint32_t> shadow = std::vector<std::uint32_t>(NUM_REGS);
    std::vector<std::uint32_t> memory = std::vector<std::uint32_t>(1 << 16);
    Shadow control = Shadow::Off;
    std::uint32_t offset = 0;
    int calls = 0;

    std::uint32_t ShadowRam(std::uint32_t method, std::uint32_t word) { // ProcessShadowRam
        if (control == Shadow::Track) {
            return shadow[method] = word;
        }
        return control == Shadow::Replay ? shadow[method] : word;
    }
    void Store(const std::uint32_t* words, std::uint32_t amount) {
        for (std::uint32_t i = 0; i < amount; ++i) {
            memory[(offset / 4 + i) % memory.size()] = words[i];
        }
        offset += amount * 4;
    }
    void Write(std::uint32_t method, std::uint32_t word) { // CallMethod past the macro check
        const std::uint32_t value = ShadowRam(method, word);
        regs[method] = value;
        if (method >= CB_FIRST && method < CB_FIRST + 16) {
            Store(&value, 1);
        }
    }
    void CallMethod(std::uint32_t method, std::uint32_t word) {
        ++calls;
        Write(method, word);
    }
    void CallMultiMethod(std::uint32_t method, const std::uint32_t* words, std::uint32_t amount) {
        ++calls;
        if (method >= CB_FIRST && method < CB_FIRST + 16) {
            Store(words, amount); // Maxwell3D::CallMultiMethod's CB_DATA case
        } else {
            for (std::uint32_t i = 0; i < amount; ++i) {
                Write(method, words[i]);
            }
        }
    }
    void StoreRun(std::uint32_t method, const std::uint32_t* words, std::uint32_t amount,
                  bool increment_once) { // Maxwell3D::StoreConstBufferRun
        ++calls;
        if (increment_once) {
            const std::uint32_t first = ShadowRam(method, words[0]);
            regs[method] = first;
            Store(&first, 1);
            Store(words + 1, amount - 1);
            return;
        }
        std::uint32_t values[16];
        for (std::uint32_t i = 0; i < amount; ++i) {
            values[i] = ShadowRam(method + i, words[i]);
            regs[method + i] = values[i];
        }
        Store(values, amount);
    }
};

/// DmaPusher::ProcessCommands' data-word branches, with or without the run.
struct Pusher {
    Engine engine;
    bool runs;
    std::uint32_t method = 0, method_count = 0;
    bool non_incrementing = false, increment_once = false;

    void Begin(const Packet& packet) {
        method = packet.method;
        method_count = static_cast<std::uint32_t>(packet.words.size());
        non_incrementing = packet.mode == Mode::NonIncreasing;
        increment_once = packet.mode == Mode::IncreaseOnce;
    }
    void Process(const std::uint32_t* words, std::size_t size) {
        for (std::size_t index = 0; index < size && method_count;) {
            if (non_incrementing) {
                const auto amount = static_cast<std::uint32_t>(
                    std::min<std::size_t>(method_count, size - index));
                engine.CallMultiMethod(method, words + index, amount);
                method_count -= amount;
                index += amount;
                continue;
            }
            if (runs) {
                const std::uint32_t run = ConstBufferRunLength(CB_FIRST, method, method_count,
                                                               size - index, increment_once);
                if (run != 0) {
                    engine.StoreRun(method, words + index, run, increment_once);
                    if (increment_once) {
                        ++method;
                        non_incrementing = true;
                    } else {
                        method += run;
                    }
                    method_count -= run;
                    index += run;
                    continue;
                }
            }
            engine.CallMethod(method, words[index]);
            method += non_incrementing ? 0 : 1;
            non_incrementing |= increment_once;
            --method_count;
            ++index;
        }
    }
};

void Same(const Pusher& a, const Pusher& b, const char* what) {
    Check(a.engine.regs == b.engine.regs, what);
    Check(a.engine.shadow == b.engine.shadow, what);
    Check(a.engine.memory == b.engine.memory, what);
    Check(a.engine.offset == b.engine.offset, what);
    Check(a.method == b.method && a.method_count == b.method_count &&
              a.non_incrementing == b.non_incrementing,
          what);
}
} // namespace

int main() {
    // The length rule on its own.
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST, 16, 16, false) == 16, "full sixteen");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST + 10, 16, 64, false) == 6, "stops at CB end");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST, 16, 5, false) == 5, "stops at list end");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST, 1, 8, false) == 0, "one word is no run");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST + 15, 4, 4, false) == 0, "last register alone");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST - 1, 16, 16, false) == 0, "before CB_DATA");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST + 16, 16, 16, false) == 0, "after CB_DATA");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST, 300, 300, true) == 300, "IncreaseOnce whole");
    Check(ConstBufferRunLength(CB_FIRST, CB_FIRST + 15, 300, 300, true) == 0,
          "IncreaseOnce leaving CB_DATA");

    // Equivalence on random streams.
    std::mt19937 rng{1234};
    int word_calls = 0, run_calls = 0;
    for (int stream = 0; stream < 20000; ++stream) {
        Pusher words{.runs = false}, runs{.runs = true};
        for (auto& value : words.engine.shadow) {
            value = rng();
        }
        runs.engine.shadow = words.engine.shadow;
        for (int p = 0; p < 6; ++p) {
            // Games switch shadow modes between packets; Replay substitutes saved words.
            words.engine.control = runs.engine.control = static_cast<Shadow>(rng() % 3);
            Packet packet;
            packet.mode = static_cast<Mode>(rng() % 3);
            packet.method = CB_FIRST - 4 + rng() % 24; // around CB_DATA, both edges
            packet.words.resize(1 + rng() % 40);
            for (auto& word : packet.words) {
                word = rng();
            }
            words.Begin(packet);
            runs.Begin(packet);
            // Split the packet over several command lists, as long pushbuffers are.
            std::size_t at = 0;
            while (at < packet.words.size()) {
                const std::size_t chunk = std::min<std::size_t>(
                    packet.words.size() - at, 1 + rng() % 24);
                words.Process(packet.words.data() + at, chunk);
                runs.Process(packet.words.data() + at, chunk);
                Same(words, runs, "word-by-word and runs differ");
                at += chunk;
            }
        }
        word_calls += words.engine.calls;
        run_calls += runs.engine.calls;
    }
    std::printf("PASS const-buffer-run: 20000 streams identical in every shadow mode; engine calls %d -> %d (%.1fx "
                "fewer)\n",
                word_calls, run_calls, double(word_calls) / run_calls);

    // The packet games send most: CB_DATA[0..15] incrementing, sixteen words.
    Pusher words{.runs = false}, runs{.runs = true};
    const Packet sixteen{Mode::Increasing, CB_FIRST, std::vector<std::uint32_t>(16, 7)};
    words.Begin(sixteen);
    runs.Begin(sixteen);
    words.Process(sixteen.words.data(), 16);
    runs.Process(sixteen.words.data(), 16);
    Same(words, runs, "sixteen-word packet");
    std::printf("16-word incrementing CB_DATA packet: %d method calls -> %d block write\n",
                words.engine.calls, runs.engine.calls);
    return 0;
}
