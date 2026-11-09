// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/write_watch.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "common/fs/path_util.h"

namespace VideoCommon::WriteWatch {
namespace {

constexpr u64 RECORD_BYTES = 128;
constexpr u64 RECORD_WORDS = RECORD_BYTES / sizeof(u32);
constexpr u64 MAX_RANGE = 8ULL * 1024 * 1024;
constexpr u64 MAX_LOG_BYTES = 128ULL * 1024 * 1024;
/// Limits and counts of a record. Its atomic counters and per-frame positions change all the time.
constexpr u32 WATCHED_WORDS = 0xE00000FEU; // words 1-7 and 29-31

std::thread worker;
std::atomic_bool stop_requested{};
std::mutex range_mutex;
u64 worker_generation{};
u64 pending_address{};
u64 pending_size{};
Reader reader_copy;

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Cpu:
        return "cpu";
    case Kind::GpuWrite:
        return "gpu";
    case Kind::Inline:
        return "inline";
    case Kind::DmaCopy:
        return "dmacopy";
    case Kind::DmaClear:
        return "dmaclear";
    case Kind::Download:
        return "download";
    case Kind::BlockWrite:
        return "block";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::Kept:
        return "kept";
    }
    return "?";
}

std::FILE* OpenLog() {
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "glitch";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    const std::string path = (dir / fmt::format("watch_{}.txt", stamp)).string();
    std::FILE* file{};
    fopen_s(&file, path.c_str(), "wb");
    return file;
}

void WatchLoop() {
    std::FILE* const file = OpenLog();
    if (!file) {
        range_end.store(0, std::memory_order_relaxed);
        return;
    }
    u64 written{};
    const auto write = [&](const std::string& line) {
        std::fwrite(line.data(), 1, line.size(), file);
        written += line.size();
    };
    write("# jadyx write watch v1: R=range S=record at start M=value changed in guest memory "
          "E=write path. Times in microseconds. Words 1-7 and 29-31 of each 128-byte record.\n");

    u64 generation{};
    u64 address{};
    std::vector<u8> previous;
    std::vector<u8> current;
    std::vector<Event> drained;
    auto last_flush = std::chrono::steady_clock::now();

    while (!stop_requested.load(std::memory_order_relaxed) && written < MAX_LOG_BYTES) {
        bool baseline{};
        {
            std::scoped_lock lock{range_mutex};
            if (generation != worker_generation) {
                generation = worker_generation;
                address = pending_address;
                previous.resize(pending_size);
                current.resize(pending_size);
                baseline = true;
            }
        }
        if (current.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }
        reader_copy(address, current);
        const u64 now = NowUs();
        if (baseline) {
            write(fmt::format("R t={} a={:x} size={}\n", now, address, current.size()));
        }
        const u64 records = current.size() / RECORD_BYTES;
        for (u64 record = 0; record < records; ++record) {
            const u8* const fresh = current.data() + record * RECORD_BYTES;
            const u8* const old = previous.data() + record * RECORD_BYTES;
            if (baseline) {
                u32 words[RECORD_WORDS];
                std::memcpy(words, fresh, RECORD_BYTES);
                if (words[3] != 0 || words[29] != 0 || words[30] != 0 || words[31] != 0) {
                    write(fmt::format("S id={} w1={:x} w2={} w3={} w4={} w5={:x} w6={:x} w7={:x} "
                                      "w29={} w30={} w31={}\n",
                                      record, words[1], words[2], words[3], words[4], words[5],
                                      words[6], words[7], words[29], words[30], words[31]));
                }
                continue;
            }
            if (std::memcmp(fresh, old, RECORD_BYTES) == 0) {
                continue;
            }
            for (u64 word = 0; word < RECORD_WORDS; ++word) {
                if (((WATCHED_WORDS >> word) & 1) == 0) {
                    continue;
                }
                u32 before;
                u32 after;
                std::memcpy(&before, old + word * sizeof(u32), sizeof(u32));
                std::memcpy(&after, fresh + word * sizeof(u32), sizeof(u32));
                if (before != after) {
                    write(fmt::format("M t={} id={} w={} old={:x} new={:x}\n", now, record, word,
                                      before, after));
                }
            }
        }
        previous.swap(current);

        const auto clock = std::chrono::steady_clock::now();
        if (clock - last_flush >= std::chrono::milliseconds{250}) {
            last_flush = clock;
            drained.clear();
            {
                std::scoped_lock lock{event_mutex};
                drained.swap(events);
            }
            for (const Event& event : drained) {
                write(fmt::format("E t={} {} a={:x} len={}\n", event.time_us, KindName(event.kind),
                                  event.address, event.size));
            }
            std::fflush(file);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    {
        std::scoped_lock lock{event_mutex};
        for (const Event& event : events) {
            write(fmt::format("E t={} {} a={:x} len={}\n", event.time_us, KindName(event.kind),
                              event.address, event.size));
        }
        events.clear();
    }
    if (written >= MAX_LOG_BYTES) {
        write("# log size limit reached\n");
    }
    range_end.store(0, std::memory_order_relaxed);
    std::fclose(file);
}

} // Anonymous namespace

void OnDispatch(const Reader& reader, u64 address, u64 size) {
    if (!Enabled() || address == 0 || size < RECORD_BYTES) {
        return;
    }
    size = std::min(size, MAX_RANGE) / RECORD_BYTES * RECORD_BYTES;
    if (range_start.load(std::memory_order_relaxed) != address ||
        range_end.load(std::memory_order_relaxed) != address + size) {
        {
            std::scoped_lock lock{range_mutex};
            if (!worker.joinable()) {
                reader_copy = reader;
            }
            pending_address = address;
            pending_size = size;
            ++worker_generation;
        }
        range_start.store(address, std::memory_order_relaxed);
        range_end.store(address + size, std::memory_order_relaxed);
        if (!worker.joinable()) {
            stop_requested.store(false, std::memory_order_relaxed);
            worker = std::thread{WatchLoop};
        }
    }
    Note(Kind::Dispatch, address, size);
}

void Stop() {
    stop_requested.store(true, std::memory_order_relaxed);
    if (worker.joinable()) {
        worker.join();
    }
    range_start.store(0, std::memory_order_relaxed);
    range_end.store(0, std::memory_order_relaxed);
    std::scoped_lock lock{range_mutex};
    reader_copy = nullptr;
    pending_address = 0;
    pending_size = 0;
}

} // namespace VideoCommon::WriteWatch
