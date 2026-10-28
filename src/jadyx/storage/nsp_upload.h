// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace Jadyx {
struct UploadPerformance {
    // Last acknowledged block only; not persisted and never used for resume decisions.
    std::uint64_t confirmed_offset{}, bytes{}, total_us{}, write_us{}, flush_us{}, checkpoint_us{}, reader_calls{};
};
struct UploadSnapshot {
    std::string id, name, fingerprint, phase{"idle"}, error;
    std::string tag; ///< Caller-defined destination (content uploads: "update:<application>").
    std::string destination{"internal"}; ///< Where the game is published: "internal" or "developer" (D:).
    std::uint64_t length{}, offset{}, received{}, free_bytes{}, developer_free_bytes{};
    bool busy{}, available{}, developer_available{};
    UploadPerformance performance;
};
struct UploadResult {
    int status{204};
    std::string error;
};
using UploadSink = std::function<bool(const char*, std::size_t)>;
using UploadReader = std::function<bool(const UploadSink&)>;
using NspValidator = std::function<std::string(const std::filesystem::path&, std::stop_token)>;

/// One bounded sequential upload; the browser queues files. No core/WinRT state is touched.
/// Two durable checkpoints preserve confirmed offsets across a process restart.
/// A consumed upload is never published under `games`: its validator installs the payload
/// elsewhere (update/DLC into internal storage) and the payload is deleted once it succeeds.
/// A `developer` upload may instead publish a game in Dev Mode's storage on D:; its payload is
/// written there too, so the move that publishes it never crosses volumes.
class NspUpload {
public:
    static constexpr std::uint64_t MaxLength = 1ULL << 40;
    static constexpr std::uint64_t MaxChunk = 64ULL << 20;
    explicit NspUpload(std::filesystem::path games, bool system_files = false,
                       std::filesystem::path temporary = {}, bool consume = false, bool developer = false,
                       bool allow_any_game_format = false);
    ~NspUpload();
    UploadSnapshot Snapshot() const;
    bool SetAvailable(bool enabled); ///< false refuses new input; never cancels an active file.
    bool BlockForLocalOperation();   ///< Atomically excludes uploads if no file is active.
    void Cancel();                  ///< Interrupt finalization; call before stopping sockets.
    void Suspend();                 ///< After joining sockets: retain the last confirmed offset.
    void Resume();                  ///< After previous operations joined.
    UploadResult Create(std::string name, std::string fingerprint, std::uint64_t length, std::string tag = {},
                        bool developer = false);
    UploadResult Append(const std::string& id, std::uint64_t offset, std::uint64_t length, const UploadReader& reader);
    UploadResult Discard(const std::string& id);
    UploadResult Pause(const std::string& id);
    UploadResult Ready(const std::string& id);
    UploadResult Rename(const std::string& id, std::string name);
    /// Only the library coordinator calls this, after its metadata worker has joined.
    std::string Finalize(const NspValidator& validate = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Jadyx
