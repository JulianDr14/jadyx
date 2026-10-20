// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Jadyx {

struct LogExportStatus {
    std::string phase{"idle"}, error, filename;
    std::uint64_t total{}, processed{}, compressed{}, files{}, completed_files{};
    std::uint64_t run{}; ///< Changes with every Start(): a reader bound to it never mixes runs.
    bool busy{};
};

// Fixed app-owned root only. Streams a size-bounded snapshot into a tar.zst
// outside log/, so the export never includes itself. No whole-file allocations.
class LogExport {
public:
    /// What a reader following the archive while it is written may do next.
    enum class Follow { Data, End, Gone };

    explicit LogExport(std::filesystem::path root);
    ~LogExport();
    std::string Start();
    void Cancel();
    void Stop();
    void Resume();
    LogExportStatus Status() const;
    std::filesystem::path ArchivePath() const;
    /// Opens the archive of `run` for reading, the partial one while it is still being
    /// written. Waits through scanning while `alive` holds. INVALID_HANDLE_VALUE if that run
    /// failed, was cancelled or replaced.
    void* OpenArchive(std::uint64_t run, const std::function<bool()>& alive) const;
    /// Blocks until `run` has flushed more than `offset` bytes (Data, `available` = flushed
    /// size), finished exactly at `offset` (End), or failed/was replaced/`alive` dropped (Gone).
    Follow WaitFor(std::uint64_t run, std::uint64_t offset, std::uint64_t& available,
                   const std::function<bool()>& alive) const;
private:
    void Run(std::stop_token stop);
    std::filesystem::path PartialPath() const;
    const std::filesystem::path root, directory;
    mutable std::mutex mutex;
    mutable std::condition_variable progress;
    std::mutex lifecycle;
    LogExportStatus status;
    bool enabled{true};
    std::jthread worker;
};
} // namespace Jadyx
