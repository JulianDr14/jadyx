// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace Jadyx {
enum class FileImport { Keys, Firmware };
struct SystemReview {
    std::string installed_firmware, incoming_firmware;
    int installed_generation{-1}, incoming_generation{-1}, required_generation{-1};
    std::vector<std::string> warnings;
};
using SystemProgress = std::function<void(unsigned,unsigned)>;
std::filesystem::path SystemDestination(FileImport kind);
bool RecoverSystemDirectory(const std::filesystem::path& destination);
// Fresh sibling of the destination. Extract/copy directly here, never into live data.
std::filesystem::path CreateSystemStage(FileImport kind);
void RemoveSystemStage(const std::filesystem::path& stage, FileImport kind) noexcept;
inline bool ValidSystemFilename(std::string name, FileImport kind) {
    for(auto& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if(kind==FileImport::Keys)return name=="prod.keys"||name=="title.keys";
    if(name.size()!=36&&name.size()!=41)return false;
    return name.substr(0,32).find_first_not_of("0123456789abcdef")==std::string::npos&&
        (name.substr(32)==".nca"||name.substr(32)==".cnmt.nca");
}
void ExtractSystemZip(const std::filesystem::path& zip, const std::filesystem::path& stage,
                      FileImport kind, SystemProgress progress, std::stop_token stop);
class SystemPrepared {
public:
    virtual ~SystemPrepared()=default;
    virtual const SystemReview& Review() const=0;
    virtual void Commit(std::stop_token stop)=0;
};
class PreparedSystemImport final : public SystemPrepared {
public:
    PreparedSystemImport(std::filesystem::path stage, FileImport kind, SystemReview review);
    ~PreparedSystemImport() override;
    const SystemReview& Review() const override { return review; }
    void Commit(std::stop_token stop) override;
private:
    std::filesystem::path stage;
    FileImport kind;
    SystemReview review;
};
// Core/KeyManager readers: only after metadata worker has joined and guest is stopped.
std::unique_ptr<PreparedSystemImport> InspectSystemStage(std::filesystem::path stage,
    FileImport kind, SystemProgress progress, std::stop_token stop);
std::wstring DescribeSystemReview(const SystemReview& review);
struct SystemImportBackend {
    std::function<std::filesystem::path(FileImport)> destination,create_stage;
    std::function<void(const std::filesystem::path&,FileImport)> remove_stage;
    std::function<void(const std::filesystem::path&,const std::filesystem::path&,FileImport,SystemProgress,std::stop_token)> extract;
    std::function<std::unique_ptr<SystemPrepared>(std::filesystem::path,FileImport,SystemProgress,std::stop_token)> inspect;
};
SystemImportBackend DefaultSystemImportBackend();
struct InstalledSystemVersions {
    std::string firmware; ///< Empty when unknown: no firmware, or not read yet.
    int key_generation{-1};
};
/// The cached versions (memory, then a small file); opens nothing else, so it is safe anytime.
InstalledSystemVersions ReadInstalledSystemVersions();
/// Drops the cached versions. Every keys or firmware import calls it once it is published.
void InvalidateSystemVersions() noexcept;
/// Reads the versions again only when the cache is missing or the files differ from what it was
/// read from (e.g. copied by hand). Opens NCAs: call only where system data is recovered before a
/// metadata worker starts.
void RefreshSystemVersions(std::stop_token stop) noexcept;
}
