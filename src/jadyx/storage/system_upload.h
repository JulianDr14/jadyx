// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "jadyx/storage/nsp_upload.h"
#include "jadyx/storage/system_import.h"
namespace Jadyx {
struct SystemUploadedFile { std::string name,fingerprint; std::uint64_t length{}; };
struct SystemUploadStatus {
    std::string id,kind,format,phase{"idle"},error;
    unsigned count{},completed{},processed{};
    std::uint64_t bytes{};
    bool busy{},available{};
    UploadSnapshot file;
    SystemReview review;
    std::vector<SystemUploadedFile> files;
};
class SystemUpload {
public:
    explicit SystemUpload(std::filesystem::path local,SystemImportBackend backend = {});
    ~SystemUpload();
    SystemUploadStatus Snapshot() const;
    void SetAvailable(bool enabled);
    bool BlockForLocalOperation();   ///< Excludes the batch unless bytes or a phase are in flight.
    UploadResult Create(std::string kind,std::string format,unsigned count);
    UploadResult BeginFile(const std::string& id,std::string name,std::string fingerprint,std::uint64_t length);
    UploadResult Append(const std::string& id,std::uint64_t offset,std::uint64_t length,const UploadReader& reader);
    UploadResult FinishFile(const std::string& id);
    UploadResult Prepare(const std::string& id);
    UploadResult Confirm(const std::string& id,bool warnings_accepted);
    UploadResult Discard(const std::string& id);
    void Cancel();
    void Suspend();
    void Resume();
    // Library coordinator only: joins metadata before invoking shared core readers/publication.
    std::string RunPending(std::stop_token stop);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
