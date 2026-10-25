// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/uwp_file_manager.h"
#include "jadyx/i18n/localization.h"
#include "jadyx/storage/storage_path.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"

namespace Jadyx {
namespace {
using namespace winrt::Windows::Storage;
using winrt::Windows::Storage::AccessCache::StorageApplicationPermissions;
constexpr unsigned PageSize = 64;
constexpr unsigned MaxFirmwareFiles = 4096;
std::wstring BrokerPath(std::filesystem::path path) {
    return path.make_preferred().wstring();
}
bool RecoverDirectory(const std::filesystem::path& destination) { return RecoverSystemDirectory(destination); }
std::filesystem::path FirmwarePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system/Contents/registered";
}
bool ValidProductionKeys(const std::filesystem::path& file) {
    // Match Eden's text format, but reject a missing/zero/duplicated XTS header key
    // before replacing working data. Never log key material.
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
        line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!line.starts_with("header_key=")) continue;
        const auto key = std::string_view{line}.substr(11);
        return key.size() == 64 && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isxdigit(c); }) &&
               key.substr(0, 32) != key.substr(32);
    }
    return false;
}
// Tiny opt-in gate ZIP writer: stored entries, no production writer dependency.
// Sources are limited fixture files copied into LocalState, never bundled data.
void GateZip(const std::filesystem::path& path,
             const std::vector<std::pair<std::string,std::filesystem::path>>& files) {
    struct Entry {std::string name;std::uint32_t crc,size,offset;};
    std::vector<Entry> entries;std::ofstream output{path,std::ios::binary};
    const auto put=[&](std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)output.put(static_cast<char>(value>>(8*i)));};
    for(const auto& [name,source]:files){
        const auto size=std::filesystem::file_size(source);
        if(size>(16ULL<<20))throw std::runtime_error("Gate fixture too large");
        std::vector<char> bytes(static_cast<std::size_t>(size));std::ifstream input{source,std::ios::binary};
        input.read(bytes.data(),static_cast<std::streamsize>(size));if(!input)throw std::runtime_error("Gate fixture read failed");
        std::uint32_t crc=0xffffffff;
        for(const auto byte:bytes){crc^=static_cast<unsigned char>(byte);for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320U:0U);}
        crc^=0xffffffff;const auto offset=static_cast<std::uint32_t>(output.tellp());
        put(0x04034b50,4);put(20,2);put(0,2);put(0,2);put(0,4);put(crc,4);put(static_cast<std::uint32_t>(size),4);put(static_cast<std::uint32_t>(size),4);
        put(static_cast<std::uint32_t>(name.size()),2);put(0,2);output<<name;output.write(bytes.data(),static_cast<std::streamsize>(size));
        entries.push_back({name,crc,static_cast<std::uint32_t>(size),offset});
    }
    const auto central=static_cast<std::uint32_t>(output.tellp());
    for(const auto& entry:entries){
        put(0x02014b50,4);put(20,2);put(20,2);put(0,2);put(0,2);put(0,4);put(entry.crc,4);put(entry.size,4);put(entry.size,4);
        put(static_cast<std::uint32_t>(entry.name.size()),2);put(0,2);put(0,2);put(0,2);put(0,2);put(0,4);put(entry.offset,4);output<<entry.name;
    }
    const auto end=static_cast<std::uint32_t>(output.tellp());
    put(0x06054b50,4);put(0,2);put(0,2);put(static_cast<std::uint32_t>(entries.size()),2);put(static_cast<std::uint32_t>(entries.size()),2);
    put(end-central,4);put(central,4);put(0,2);output.close();if(!output)throw std::runtime_error("Gate ZIP write failed");
}
}

bool HasUsableHeaderKey() {
    auto& keys = Core::Crypto::KeyManager::Instance();
    if (!keys.HasKey(Core::Crypto::S256KeyType::Header)) return false;
    const auto key = keys.GetKey(Core::Crypto::S256KeyType::Header);
    return !std::equal(key.begin(), key.begin() + 16, key.begin() + 16);
}

FileSetupStatus ReadFileSetupStatus() {
    const auto keys = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    const bool restored_keys = RecoverDirectory(keys);
    RecoverDirectory(FirmwarePath());
    if (restored_keys) Core::Crypto::KeyManager::Instance().ReloadKeys();
    FileSetupStatus result;
    result.keys_ready = HasUsableHeaderKey();
    std::error_code error;
    for (std::filesystem::directory_iterator it{FirmwarePath(), error}, end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == ".nca") ++result.firmware_files;
    }
    result.versions = ReadInstalledSystemVersions();
    for (const auto& entry : StorageApplicationPermissions::FutureAccessList().Entries()) {
        const auto token = winrt::to_string(entry.Token);
        if (!Jadyx::IsGameToken(token)) continue;
        auto name = std::filesystem::path{std::wstring{entry.Metadata}}.filename().wstring();
        result.sources.push_back({token, name.empty() ? std::wstring{I18n::Tr(I18n::Text::UsbFolder)} : name});
    }
    return result;
}
void ForgetGameFolder(std::string_view token) {
    if (!Jadyx::IsGameToken(token)) throw std::invalid_argument("not a game folder token");
    StorageApplicationPermissions::FutureAccessList().Remove(winrt::to_hstring(token));
}

std::string ImportSystemFiles(const StorageFolder& source, FileImport kind,
                             FileImportProgress& progress, std::stop_token stop) {
    std::filesystem::path stage_path,zip_path;
    progress.decision=0;progress.confirmation=false;
    try {
        const bool keys=kind==FileImport::Keys;
        if(!keys&&!HasUsableHeaderKey())return I18n::ToUtf8(I18n::Tr(I18n::Text::KeysFirst));
        stage_path=CreateSystemStage(kind);
        auto stage=StorageFolder::GetFolderFromPathAsync(BrokerPath(stage_path)).get();
        std::vector<StorageFile> files,archives;
        for(unsigned start=0;;start+=PageSize){
            auto page=source.GetFilesAsync(Search::CommonFileQuery::DefaultQuery,start,PageSize).get();
            for(const auto& file:page){
                auto name=winrt::to_string(file.Name());
                std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                if(ValidSystemFilename(name,kind))files.push_back(file);
                else if(name.ends_with(".zip"))archives.push_back(file);
                if(files.size()>(keys?2U:MaxFirmwareFiles)||archives.size()>1)throw std::runtime_error("file_limit");
            }
            if(stop.stop_requested())throw std::runtime_error("interrupted");
            if(page.Size()<PageSize)break;
            if(start>=16384)throw std::runtime_error("file_limit");
        }
        if(files.empty()&&archives.size()==1){
            const auto size=archives.front().GetBasicPropertiesAsync().get().Size();
            if(size>(keys?16ULL<<20:4ULL<<30))throw std::runtime_error("archive_limit");
            if(std::filesystem::space(stage_path).available<size+(64ULL<<20))throw std::runtime_error("space");
            zip_path=std::filesystem::path{stage_path}.concat(L".zip");
            auto parent=StorageFolder::GetFolderFromPathAsync(BrokerPath(zip_path.parent_path())).get();
            archives.front().CopyAsync(parent,zip_path.filename().wstring(),NameCollisionOption::FailIfExists).get();
            ExtractSystemZip(zip_path,stage_path,kind,[&](unsigned done,unsigned total){progress.completed=done;progress.total=total;},stop);
            std::filesystem::remove(zip_path);zip_path.clear();
        }else{
            if(files.empty()||!archives.empty())throw std::runtime_error("file");
            progress.total=static_cast<unsigned>(files.size());std::uint64_t total{};
            for(const auto& file:files){
                if(stop.stop_requested())throw std::runtime_error("interrupted");
                const auto bytes=file.GetBasicPropertiesAsync().get().Size();
                if(!bytes||(keys&&bytes>(4ULL<<20))||bytes>(8ULL<<30)-total)throw std::runtime_error("archive_limit");
                if(std::filesystem::space(stage_path).available<bytes+(64ULL<<20))throw std::runtime_error("space");
                std::wstring name{file.Name()};for(auto& c:name)c=std::towlower(c);
                auto copied=file.CopyAsync(stage,name,NameCollisionOption::FailIfExists).get();
                if(copied.GetBasicPropertiesAsync().get().Size()!=bytes)throw std::runtime_error("disk");
                total+=bytes;progress.completed.fetch_add(1);
            }
        }
        auto prepared=InspectSystemStage(stage_path,kind,[&](unsigned done,unsigned total){progress.completed=done;progress.total=total;},stop);
        if(!prepared->Review().warnings.empty()){
            {std::lock_guard lock{progress.review_mutex};progress.review_text=DescribeSystemReview(prepared->Review());}
            progress.confirmation=true;
            std::stop_callback cancel{stop,[&]{progress.decision=2;progress.decision.notify_all();}};
            while(progress.decision.load()==0)progress.decision.wait(0);
            progress.confirmation=false;
            if(progress.decision.load()!=1||stop.stop_requested())throw std::runtime_error("interrupted");
        }
        prepared->Commit(stop);stage_path.clear();return {};
    }catch(...){
        progress.confirmation=false;RemoveSystemStage(stage_path,kind);
        if(!zip_path.empty()){std::error_code ec;std::filesystem::remove(zip_path,ec);}
        return I18n::ToUtf8(I18n::Tr(I18n::Text::ImportFailedKept));
    }
}
bool RunFileManagerGate(const std::function<void(std::string)>& diagnostic) {
    using namespace winrt::Windows::Storage;
    const auto old_keys = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    const auto old_nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    SCOPE_EXIT {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, old_keys);
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, old_nand);
        Core::Crypto::KeyManager::Instance().ReloadKeys();
    };
    std::string step = "prepare fixture";
    try {
        auto local = ApplicationData::Current().LocalFolder();
        auto fixture = local.CreateFolderAsync(L"file-manager-gate", CreationCollisionOption::GenerateUniqueName).get();
        // The opt-in gate leaves its tiny fixture for inspection; never changes live data.
        auto key_source = StorageFolder::GetFolderFromPathAsync(BrokerPath(old_keys)).get();
        auto firmware_source = fixture.CreateFolderAsync(L"source-firmware").get();
        FileSys::RealVfsFilesystem vfs;
        unsigned required = 0;
        for (const auto& file : std::filesystem::directory_iterator{FirmwarePath()}) {
            if (file.path().extension() != ".nca") continue;
            const auto path = file.path().u8string();
            FileSys::NCA nca{vfs.OpenFile(std::string{reinterpret_cast<const char*>(path.data()), path.size()}, FileSys::OpenMode::Read)};
            if (nca.GetStatus() != Loader::ResultStatus::Success) continue;
            if ((nca.GetTitleId() == 0x0100000000000809ULL && nca.GetType() == FileSys::NCAContentType::Data) ||
                (nca.GetTitleId() == 0x0100000000001009ULL && nca.GetType() == FileSys::NCAContentType::Program)) {
                auto source = StorageFile::GetFileFromPathAsync(BrokerPath(file.path())).get();
                source.CopyAsync(firmware_source).get();
                ++required;
            }
        }
        if (required < 2) throw std::runtime_error("Gate needs the user's installed firmware fixture.");
        const auto fixture_path = std::filesystem::path{std::wstring{fixture.Path()}};
        std::filesystem::create_directories(fixture_path / "keys");
        std::filesystem::create_directories(fixture_path / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, fixture_path / "keys");
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, fixture_path / "nand");
        FileImportProgress progress;
        auto require = [](bool ok, const char* error) { if (!ok) throw std::runtime_error(error); };
        step = "keys import";
        const auto keys_error = ImportSystemFiles(key_source, FileImport::Keys, progress, {});
        if (!keys_error.empty()) throw std::runtime_error(keys_error);
        require(HasUsableHeaderKey(), "Imported keys not reloaded");
        step = "firmware import";
        const auto firmware_error = ImportSystemFiles(firmware_source, FileImport::Firmware, progress, {});
        if (!firmware_error.empty()) throw std::runtime_error(firmware_error);
        const auto before = ReadFileSetupStatus();
        require(before.keys_ready && before.firmware_files == required, "Setup status mismatch");
        step = "shared ZIP reader and review";
        const auto check_zip=[&](const std::filesystem::path& zip,FileImport kind,bool valid){
            const auto stage=CreateSystemStage(kind);SCOPE_EXIT{RemoveSystemStage(stage,kind);};
            bool accepted=false;
            try{ExtractSystemZip(zip,stage,kind,{},{});auto prepared=InspectSystemStage(stage,kind,{},{});
                require(!prepared->Review().incoming_firmware.empty(),"SystemVersion missing in review");accepted=true;
            }catch(...){if(valid)throw;}
            require(accepted==valid,"Unsafe/damaged ZIP accepted");
        };
        const auto zip=fixture_path/"fixture.zip";
        std::vector<std::pair<std::string,std::filesystem::path>> archive_files;
        archive_files.emplace_back("nested/prod.keys",fixture_path/"keys/prod.keys");
        GateZip(zip,archive_files);check_zip(zip,FileImport::Keys,true);
        archive_files.clear();
        for(const auto& file:std::filesystem::directory_iterator{FirmwarePath()})
            archive_files.emplace_back("nested/"+file.path().filename().string(),file.path());
        GateZip(zip,archive_files);check_zip(zip,FileImport::Firmware,true);
        std::filesystem::resize_file(zip,std::filesystem::file_size(zip)-22);check_zip(zip,FileImport::Firmware,false);
        archive_files={{"../prod.keys",fixture_path/"keys/prod.keys"}};
        GateZip(zip,archive_files);check_zip(zip,FileImport::Keys,false);
        archive_files={{"a/prod.keys",fixture_path/"keys/prod.keys"},{"b/PROD.KEYS",fixture_path/"keys/prod.keys"}};
        GateZip(zip,archive_files);check_zip(zip,FileImport::Keys,false);
        std::filesystem::remove(zip);
        require(ReadFileSetupStatus().firmware_files==required&&HasUsableHeaderKey(),"Preview changed installed data");
        step="key downgrade and compatibility review";
        {
            const auto stage=CreateSystemStage(FileImport::Keys);SCOPE_EXIT{RemoveSystemStage(stage,FileImport::Keys);};
            // Syntactically valid but deliberately unrelated header material.
            // Only preview this private fixture; never commit it.
            {std::ofstream candidate{stage/"prod.keys"};candidate<<"header_key="<<std::string(32,'1')<<std::string(32,'2')<<'\n';}
            const auto prepared=InspectSystemStage(stage,FileImport::Keys,{},{});
            const auto& review=prepared->Review();
            require(std::find(review.warnings.begin(),review.warnings.end(),"keys_firmware_old")!=review.warnings.end(),"Incompatible key material not warned");
            if(review.installed_generation>=0)
                require(std::find(review.warnings.begin(),review.warnings.end(),"keys_downgrade")!=review.warnings.end(),"Lost key generations not warned");
        }
        require(HasUsableHeaderKey()&&ValidProductionKeys(fixture_path/"keys/prod.keys"),"Key preview mutated installed material");
        auto bad = fixture.CreateFolderAsync(L"invalid-keys").get();
        auto invalid = bad.CreateFileAsync(L"prod.keys").get();
        FileIO::WriteTextAsync(invalid, L"header_key=0000\n").get();
        require(!ImportSystemFiles(bad, FileImport::Keys, progress, {}).empty(), "Invalid keys accepted");
        require(ValidProductionKeys(fixture_path / "keys/prod.keys"), "Working keys replaced by invalid input");
        std::stop_source cancellation;
        cancellation.request_stop();
        require(!ImportSystemFiles(firmware_source, FileImport::Firmware, progress, cancellation.get_token()).empty(), "Cancelled import committed");
        require(ReadFileSetupStatus().firmware_files == required, "Cancellation changed installed firmware");
        const auto firmware = FirmwarePath();
        const auto previous = firmware.parent_path() / L"registered.previous";
        std::filesystem::rename(firmware, previous);
        require(ReadFileSetupStatus().firmware_files == required, "Interrupted publication recovery failed");
        diagnostic("File manager gate PASS: broker copies, real NCA/SystemVersion validation, nested ZIP keys/firmware, truncated/traversal/duplicate ZIP rejection, key downgrade/compatibility review, key reload, invalid input, cancellation and interrupted publication recovery; live data preserved");
        return true;
    } catch (const std::exception& e) {
        diagnostic("File manager gate FAIL " + step + ": " + std::string{e.what()});
    } catch (const winrt::hresult_error& e) {
        diagnostic("File manager gate FAIL " + step + " HRESULT=" + std::to_string(e.code().value));
    }
    return false;
}
} // namespace Jadyx
