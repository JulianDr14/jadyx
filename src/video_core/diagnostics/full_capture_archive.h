// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VideoCommon::FullCapture {

// Caller serializes access. Standard-library-only so the real writer can be tested standalone.
class Archive {
public:
    static constexpr std::uint64_t MAX_BYTES = 2ULL << 30;
    // Streamed to disk, not retained in memory. Real-game two-frame captures
    // exceed the old homebrew-sized metadata budget even with snapshot reuse.
    static constexpr std::uint64_t MAX_EVENTS = 1000000;
    static constexpr std::uint64_t MAX_TEXT = 512ULL << 20;

    static std::string Quote(std::string_view value) {
        std::string out{"\""};
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
            else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
        return out + '"';
    }

    bool Begin(const std::filesystem::path& path) {
        if (active || std::filesystem::exists(path)) return false;
        std::filesystem::create_directories(path);
        root = path;
        blobs.open(root / "blobs.bin", std::ios::binary | std::ios::in | std::ios::out |
                                        std::ios::trunc);
        index.open(root / "events.jsonl", std::ios::binary | std::ios::trunc);
        bytes = events = omissions = errors = text = 0;
        known.clear();
        start = std::chrono::steady_clock::now();
        active = blobs.good() && index.good();
        if (!active) { blobs.close(); index.close(); return false; }
        // An interrupted capture has no final summary and must be rejected by the analyzer.
        Event("begin", "{\"schema\":1,\"synchronous\":true,\"fps_valid\":false,"
                       "\"byte_limit\":2147483648,\"chunk_bytes\":65536,\"max_staging_bytes\":16777216}");
        return true;
    }

    bool Active() const { return active; }
    void Event(std::string_view kind, std::string_view detail) {
        if (!active) return;
        if (events >= MAX_EVENTS || kind.size() + detail.size() + 256 > MAX_TEXT - text) {
            ++omissions;
            return;
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        const std::string line = "{\"seq\":" + std::to_string(events++) +
            ",\"us\":" + std::to_string(us) + ",\"kind\":" + Quote(kind) +
            ",\"detail\":" + std::string(detail) + "}\n";
        index << line;
        text += line.size();
        if (!index) ++errors;
    }

    void Omit(std::string_view reason, std::string_view detail) {
        if (!active) return;
        ++omissions;
        Event("omitted", "{\"reason\":" + Quote(reason) + ",\"context\":" +
                          std::string(detail) + "}");
    }

    void Data(std::string_view kind, std::string_view detail, std::span<const std::uint8_t> data,
              std::size_t row_bytes = 0, std::size_t row_pitch = 0, std::size_t rows = 1) {
        if (!active) return;
        if (!row_bytes) row_bytes = data.size();
        if (events >= MAX_EVENTS) { ++omissions; return; }
        if (!row_pitch) row_pitch = row_bytes;
        if (!rows || !row_bytes || row_bytes > row_pitch || row_bytes > data.size() ||
            rows - 1 > (data.size() - row_bytes) / row_pitch ||
            rows > MAX_BYTES / row_bytes) {
            Omit("invalid_or_oversize_data", detail);
            return;
        }
        const std::uint64_t size = row_bytes * rows;
        // Content-addressed chunks also share unchanged portions of successive full MRTs.
        // No pixel/sample selection and no lossy compression; each event describes all bytes.
        std::array<std::uint8_t, 65536> chunk{};
        std::size_t used{};
        std::string segments{"["};
        bool reused = true;
        const auto flush = [&]() {
            std::uint64_t offset{}, hash{};
            bool shared{};
            if (!StoreChunk(std::span<const std::uint8_t>{chunk}.first(used), offset, hash, shared))
                return false;
            if (segments.size() != 1) segments += ',';
            segments += "{\"offset\":" + std::to_string(offset) + ",\"size\":" +
                std::to_string(used) + ",\"fnv64w\":\"" + std::to_string(hash) + "\"}";
            reused &= shared;
            used = 0;
            return true;
        };
        for (std::size_t row = 0; row < rows; ++row) {
            for (std::size_t at = 0; at < row_bytes;) {
                const auto n = std::min(chunk.size() - used, row_bytes - at);
                std::memcpy(chunk.data() + used, data.data() + row * row_pitch + at, n);
                used += n;
                at += n;
                if (used == chunk.size() && !flush()) { Omit("chunk_write_or_budget", detail); return; }
            }
        }
        if (used && !flush()) { Omit("chunk_write_or_budget", detail); return; }
        segments += ']';
        Event(kind, "{\"blob_size\":" + std::to_string(size) + ",\"segments\":" + segments +
            ",\"reused\":" + (reused ? "true" : "false") +
            ",\"context\":" + std::string(detail) + "}");
    }

    bool StoreChunk(std::span<const std::uint8_t> data, std::uint64_t& offset,
                    std::uint64_t& hash, bool& reused) {
        const auto size = data.size();
        hash = Hash(data);
        // A hash collision must never substitute different bytes.
        const std::string key = std::to_string(size) + ":" + std::to_string(hash);
        offset = bytes;
        reused = false;
        if (const auto it = known.find(key); it != known.end()) {
            std::array<char, 65536> chunk{};
            // Repeated render targets hit this path for almost every chunk; flush only new writes.
            if (unflushed) { blobs.flush(); unflushed = false; }
            for (const auto candidate : it->second) {
                blobs.clear();
                blobs.seekg(static_cast<std::streamoff>(candidate));
                blobs.read(chunk.data(), static_cast<std::streamsize>(size));
                const bool equal = blobs.good() && std::equal(chunk.begin(), chunk.begin() + size,
                    data.begin(), [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
                if (equal) { offset = candidate; reused = true; break; }
            }
        }
        if (!reused) {
            if (size > MAX_BYTES - bytes) return false;
            blobs.clear();
            blobs.seekp(static_cast<std::streamoff>(bytes));
            blobs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
            if (!blobs) { ++errors; return false; }
            unflushed = true;
            known[key].push_back(bytes);
            bytes += size;
        }
        return true;
    }

    void End(bool complete) {
        if (!active) return;
        Event("end", "{\"intervals_complete\":" + std::string(complete ? "true" : "false") + "}");
        index.flush();
        blobs.flush();
        if (!index || !blobs) ++errors;
        std::ofstream summary(root / "summary.json", std::ios::binary | std::ios::trunc);
        summary << "{\"schema\":1,\"intervals_complete\":" << (complete ? "true" : "false")
                << ",\"events\":" << events << ",\"unique_bytes\":" << bytes
                << ",\"omissions\":" << omissions << ",\"errors\":" << errors
                << ",\"data_complete\":" << (complete && !omissions && !errors ? "true" : "false")
                << ",\"synchronous\":true,\"fps_valid\":false}\n";
        summary.flush();
        active = false;
        blobs.close();
        index.close();
        known.clear();
    }

private:
    // FNV-1a over little-endian 64-bit words, then the tail bytes: the byte-wise form
    // dominated capture time once multi-GiB of repeated targets went through it.
    static std::uint64_t Hash(std::span<const std::uint8_t> data) {
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        std::size_t at = 0;
        for (; at + 8 <= data.size(); at += 8) {
            std::uint64_t word{};
            for (std::size_t i = 0; i < 8; ++i) word |= std::uint64_t{data[at + i]} << (i * 8);
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        for (; at < data.size(); ++at) hash = (hash ^ data[at]) * 0x100000001b3ULL;
        return hash;
    }

    bool active{};
    bool unflushed{};
    std::filesystem::path root;
    std::fstream blobs;
    std::ofstream index;
    std::chrono::steady_clock::time_point start;
    std::uint64_t bytes{}, events{}, omissions{}, errors{}, text{};
    std::unordered_map<std::string, std::vector<std::uint64_t>> known;
};

} // namespace VideoCommon::FullCapture
