// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>
#include <concepts>
#include <utility>

namespace Jadyx {
enum class FileImport { Keys, Firmware };
struct SystemReview {
    std::string installed_firmware, incoming_firmware;
    int installed_generation{-1}, incoming_generation{-1}, required_generation{-1};
    std::vector<std::string> warnings;
};
struct SystemProgress {
    std::function<void(unsigned, unsigned)> counts;
    std::function<void(std::string_view)> current_file;
    SystemProgress() = default;
    template <typename Callback>
        requires std::invocable<Callback, unsigned, unsigned>
    SystemProgress(Callback callback) : counts{std::move(callback)} {}
    explicit operator bool() const { return static_cast<bool>(counts); }
    void operator()(unsigned done, unsigned total) const {
        if (counts)
            counts(done, total);
    }
    void File(std::string_view name) const {
        if (current_file)
            current_file(name);
    }
};
struct InstalledFirmwareStatus {
    bool ready{};
    std::string version;
};
InstalledFirmwareStatus ReadInstalledFirmwareStatus();
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
}
