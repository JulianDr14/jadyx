// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/system_import.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#include <archive.h>
#include <archive_entry.h>
#include <openssl/rand.h>
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/fssystem/fssystem_crypto_configuration.h"
#include "core/file_sys/fssystem/fssystem_compression_configuration.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "jadyx/i18n/localization.h"

namespace Jadyx {
namespace {
namespace fs=std::filesystem;
constexpr std::uint64_t MiB=1ULL<<20, GiB=1ULL<<30;
void Require(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
void CheckStop(std::stop_token stop) { Require(!stop.stop_requested(),"interrupted"); }
std::string Lower(std::string s) { for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s; }
std::string Utf8(const fs::path& path) { const auto value=path.u8string();return {value.begin(),value.end()}; }
void Internal(const fs::path& path) {
    const fs::path local{std::wstring{winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()}};
    const auto relative=path.lexically_normal().lexically_relative(local.lexically_normal());
    Require(!relative.empty()&&!relative.is_absolute(),"destination");
    for(const auto& part:relative)Require(part!=L"..","destination");
}
bool StagePath(const fs::path& path,FileImport kind) {
    const auto destination=SystemDestination(kind);
    return path.parent_path()==destination.parent_path() &&
        path.filename().wstring().starts_with(destination.filename().wstring()+L".import-");
}
using KeyMap=std::map<std::string,std::string>;
KeyMap ReadKeys(const fs::path& path,bool title=false) {
    Require(fs::is_regular_file(path)&&fs::file_size(path)<=4*MiB,"keys");
    std::ifstream input{path};Require(input.good(),"disk");
    KeyMap keys;std::string line;
    while(std::getline(input,line)) {
        Require(line.size()<=4096,"keys");
        if(line.starts_with("\xef\xbb\xbf"))line.erase(0,3);
        if(const auto at=line.find_first_of("#;");at!=std::string::npos)line.resize(at);
        line.erase(std::remove_if(line.begin(),line.end(),[](unsigned char c){return std::isspace(c)!=0;}),line.end());
        if(line.empty())continue;
        const auto at=line.find('=');Require(at!=std::string::npos,"keys");
        auto name=Lower(line.substr(0,at)),value=Lower(line.substr(at+1));
        Require(!name.empty()&&!value.empty()&&value.size()%2==0&&
            value.find_first_not_of("0123456789abcdef")==std::string::npos,"keys");
        if(title)Require(name.size()==32&&name.find_first_not_of("0123456789abcdef")==std::string::npos&&value.size()==32,"keys");
        Require(keys.emplace(std::move(name),std::move(value)).second,"keys");
    }
    Require(!input.bad()&&!keys.empty(),"keys");return keys;
}
bool Nonzero(const std::string& value) { return value.find_first_not_of('0')!=std::string::npos; }
std::set<int> Generations(const KeyMap& keys) {
    std::set<int> result;
    for(const auto& [name,value]:keys) {
        std::string suffix;
        for(const auto prefix:{"master_key_","key_area_key_application_","key_area_key_ocean_","key_area_key_system_"})
            if(name.starts_with(prefix))suffix=name.substr(std::char_traits<char>::length(prefix));
        if(suffix.size()!=2||suffix.find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
        Require(value.size()==32,"keys");
        if(Nonzero(value))result.insert(std::stoi(suffix,nullptr,16));
    }
    return result;
}
struct FirmwareInfo {
    std::array<unsigned,5> version{};
    std::string display;
    int generation{-1};
    std::set<std::pair<int,int>> areas;
    unsigned removed{};
};
// The bundled libarchive intentionally excludes platform filename openers.
// Supply bounded seekable input, as the mod importer already does.
struct ZipSource {
    std::ifstream file;
    std::array<char,65536> buffer;
    static la_ssize_t Read(archive*,void* data,const void** out) {
        auto& source=*static_cast<ZipSource*>(data);*out=source.buffer.data();
        source.file.read(source.buffer.data(),source.buffer.size());
        return source.file.bad()?-1:static_cast<la_ssize_t>(source.file.gcount());
    }
    static la_int64_t Seek(archive*,void* data,la_int64_t offset,int whence) {
        auto& source=*static_cast<ZipSource*>(data);source.file.clear();
        source.file.seekg(offset,whence==SEEK_SET?std::ios::beg:whence==SEEK_CUR?std::ios::cur:std::ios::end);
        return source.file?static_cast<la_int64_t>(source.file.tellg()):ARCHIVE_FATAL;
    }
};
FirmwareInfo InspectFirmware(const fs::path& folder,bool strict,SystemProgress progress,std::stop_token stop) {
    FirmwareInfo info;bool system=false,mii=false,update=false;unsigned count{};
    std::set<u64> listed;
    // Titles outside 0x010000000000xxxx wait for the meta: only the NCAs that stay count.
    struct Outside{fs::path path;int generation;std::optional<std::pair<int,int>> area;};
    std::map<u64,std::vector<Outside>> outside;
    const auto keep=[&info](int generation,const std::optional<std::pair<int,int>>& area){
        info.generation=(std::max)(info.generation,generation);if(area)info.areas.insert(*area);};
    if(!fs::exists(folder)){Require(!strict,"firmware_incomplete");return info;}
    FileSys::RealVfsFilesystem vfs;
    for(const auto& file:fs::directory_iterator{folder}) {
        CheckStop(stop);
        if(!file.is_regular_file()||Lower(Utf8(file.path().extension()))!=".nca")continue;
        Require(++count<=4096,"firmware_limit");
        const auto raw=vfs.OpenFile(Utf8(file.path()),FileSys::OpenMode::Read);
        if(!raw||raw->GetSize()<0xC00){Require(!strict,"firmware_incomplete");continue;}
        FileSys::NcaReader header;
        if(R_FAILED(header.Initialize(raw,FileSys::GetCryptoConfiguration(),FileSys::GetNcaCompressionConfiguration()))){Require(!strict,"firmware_keys");continue;}
        FileSys::NCA nca{raw};
        if(nca.GetStatus()!=Loader::ResultStatus::Success){Require(!strict,"firmware_keys");continue;}
        // A meta NCA always holds a PFS0. Opening without one means the keys read the header but not
        // the content (e.g. a wrong key_area_key_application_00): the SystemUpdate meta is lost and
        // system titles outside 0x010000000000xxxx would be taken for games.
        if(nca.GetType()==FileSys::NCAContentType::Meta&&nca.GetSubdirectories().empty()) {
            LOG_WARNING(Frontend, "Firmware import: the meta NCA '{}' ({:016X}, key generation {}) could not be decrypted",
                        Utf8(file.path().filename()), nca.GetTitleId(), header.GetKeyGeneration());
            Require(!strict,"firmware_keys");continue;
        }
        // Firmware also ships a few system titles outside 0x010000000000xxxx (e.g. 01008BB00013C000);
        // those are allowed only when the SystemUpdate meta lists them.
        const int generation=static_cast<int>((std::max)(nca.GetKeyGeneration(),u8{1})-1);
        std::optional<std::pair<int,int>> area;
        if(header.GetKeyIndex()<3)area=std::pair{static_cast<int>((std::max)(header.GetKeyGeneration(),u8{1})-1),static_cast<int>(header.GetKeyIndex())};
        if((nca.GetTitleId()>>16)!=(0x0100000000000000ULL>>16))outside[nca.GetTitleId()].push_back({file.path(),generation,area});
        else keep(generation,area);
        if(nca.GetTitleId()==0x0100000000000816ULL&&nca.GetType()==FileSys::NCAContentType::Meta&&
           !nca.GetSubdirectories().empty())
            if(const auto cnmt=nca.GetSubdirectories()[0]->GetFile("SystemUpdate_0100000000000816.cnmt")) {
                update=true;
                for(const auto& record:FileSys::CNMT{cnmt}.GetMetaRecords())listed.insert(record.title_id);
            }
        if(nca.GetTitleId()==0x0100000000000809ULL&&nca.GetType()==FileSys::NCAContentType::Data) {
            const auto romfs=nca.GetRomFS()?FileSys::ExtractRomFS(nca.GetRomFS()):nullptr;
            const auto version=romfs?romfs->GetFile("file"):nullptr;
            if(!version||version->GetSize()!=256){Require(!strict,"firmware_version");continue;}
            const auto bytes=version->ReadBytes(256);
            Require(bytes.size()==256&&bytes[8]=='N'&&bytes[9]=='X'&&bytes[10]==0,"firmware_version");
            info.version={bytes[0],bytes[1],bytes[2],bytes[4],bytes[5]};
            info.display=std::to_string(bytes[0])+"."+std::to_string(bytes[1])+"."+std::to_string(bytes[2]);
            if(bytes[4]||bytes[5])info.display+=" (rev "+std::to_string(bytes[4])+"."+std::to_string(bytes[5])+")";
            Require(!system,"firmware_duplicate");system=true;
        }
        mii|=nca.GetTitleId()==0x0100000000001009ULL&&nca.GetType()==FileSys::NCAContentType::Program;
        if(progress)progress(count,0);
    }
    // Players keep their dumps together, so a game NCA often sits next to the firmware. A staged
    // import is a copy: leaving those files out installs the firmware and loses nothing, instead
    // of refusing the whole package. Titles the SystemUpdate meta lists are real system content
    // and stay; the installed folder is only inspected, never trimmed.
    for(const auto& [title,files]:outside) {
        if((update&&listed.contains(title))||!strict) {
            for(const auto& file:files)keep(file.generation,file.area);
            continue;
        }
        for(const auto& file:files) {
            LOG_INFO(Frontend, "Firmware import: leaving out the game NCA {:016X} ('{}')", title,
                     Utf8(file.path.filename()));
            // A file left behind would be published inside the firmware folder.
            std::error_code error;Require(fs::remove(file.path,error)&&!error,"disk");++info.removed;
        }
    }
    Require(!strict||(system&&mii),"firmware_incomplete");return info;
}
}

std::filesystem::path SystemDestination(FileImport kind) {
    return kind==FileImport::Keys?Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir):
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir)/"system/Contents/registered";
}
bool RecoverSystemDirectory(const fs::path& destination) {
    Internal(destination);
    const auto previous=fs::path{destination}.concat(L".previous");
    if(!fs::exists(previous))return false;
    if(!fs::exists(destination)){fs::rename(previous,destination);return true;}
    fs::remove_all(previous);return false;
}
fs::path CreateSystemStage(FileImport kind) {
    const auto destination=SystemDestination(kind);Internal(destination);
    RecoverSystemDirectory(destination);fs::create_directories(destination.parent_path());
    std::array<unsigned char,16> random{};Require(RAND_bytes(random.data(),static_cast<int>(random.size()))==1,"random");
    constexpr char hex[]="0123456789abcdef";std::string id;
    for(auto c:random){id+=hex[c>>4];id+=hex[c&15];}
    auto stage=fs::path{destination}.concat(L".import-").concat(std::wstring{id.begin(),id.end()});
    Require(fs::create_directory(stage),"disk");return stage;
}
void RemoveSystemStage(const fs::path& stage,FileImport kind) noexcept {
    try{if(!stage.empty()&&StagePath(stage,kind)){Internal(stage);fs::remove_all(stage);}}catch(...){}
}
void ExtractSystemZip(const fs::path& zip,const fs::path& stage,FileImport kind,SystemProgress progress,std::stop_token stop) {
    Require(StagePath(stage,kind),"destination");Internal(stage);
    Require(fs::file_size(zip)<=(kind==FileImport::Keys?16*MiB:4*GiB),"archive_limit");
    const auto max_bytes=kind==FileImport::Keys?8*MiB:8*GiB;
    auto* archive=archive_read_new();Require(archive!=nullptr,"archive");
    SCOPE_EXIT{archive_read_free(archive);};
    archive_read_support_filter_none(archive);archive_read_support_format_zip_seekable(archive);
    ZipSource source{std::ifstream{zip,std::ios::binary},{}};Require(source.file.good(),"disk");
    archive_read_set_callback_data(archive,&source);
    archive_read_set_read_callback(archive,ZipSource::Read);
    archive_read_set_seek_callback(archive,ZipSource::Seek);
    Require(archive_read_open1(archive)==ARCHIVE_OK,"archive");
    archive_entry* entry{};std::set<std::string> names;std::uint64_t total{};unsigned seen{},files{};
    std::vector<char> buffer(1<<20);
    for(;;) {
        CheckStop(stop);const auto status=archive_read_next_header(archive,&entry);
        if(status==ARCHIVE_EOF)break;
        Require(status==ARCHIVE_OK&&(archive_format(archive)&ARCHIVE_FORMAT_BASE_MASK)==ARCHIVE_FORMAT_ZIP,"archive");
        Require(++seen<=8192,"archive_limit");
        Require(!archive_entry_symlink(entry)&&!archive_entry_hardlink(entry)&&archive_entry_is_encrypted(entry)==0,"archive_unsafe");
        const auto* encoded=archive_entry_pathname_utf8(entry);Require(encoded!=nullptr,"archive_unsafe");
        std::string path{encoded};std::replace(path.begin(),path.end(),'\\','/');
        Require(!path.empty()&&path.front()!='/'&&path.find(':')==std::string::npos,"archive_unsafe");
        if(path.back()=='/')path.pop_back();
        std::size_t start{};
        for(std::size_t end;;start=end+1){end=path.find('/',start);const auto part=path.substr(start,end-start);
            Require(!part.empty()&&part!="."&&part!="..","archive_unsafe");if(end==std::string::npos)break;}
        if(archive_entry_filetype(entry)==AE_IFDIR)continue;
        Require(archive_entry_filetype(entry)==AE_IFREG,"archive_unsafe");
        const auto slash=path.find_last_of('/');const auto name=Lower(path.substr(slash==std::string::npos?0:slash+1));
        Require(ValidSystemFilename(name,kind),"file");
        Require(names.insert(name).second,"firmware_duplicate");
        Require(++files<=(kind==FileImport::Keys?2U:4096U),"archive_limit");
        const auto size=archive_entry_size(entry);
        Require(size>0&&static_cast<std::uint64_t>(size)<=max_bytes-total,"archive_limit");
        Require(kind!=FileImport::Keys||size<=static_cast<la_int64_t>(4*MiB),"keys_limit");
        const auto target=stage/fs::path{std::u8string{name.begin(),name.end()}};
        Require(!fs::exists(target),"firmware_duplicate");
        std::ofstream output{target,std::ios::binary};Require(output.good(),"disk");std::uint64_t written{};
        for(;;){CheckStop(stop);const auto count=archive_read_data(archive,buffer.data(),buffer.size());
            Require(count>=0,"archive");if(!count)break;
            const auto bytes=static_cast<std::uint64_t>(count);
            Require(bytes<=static_cast<std::uint64_t>(size)-written&&bytes<=max_bytes-total,"archive_limit");
            Require(fs::space(stage).available>bytes+64*MiB,"space");
            output.write(buffer.data(),static_cast<std::streamsize>(count));Require(output.good(),"disk");written+=bytes;total+=bytes;}
        Require(written==static_cast<std::uint64_t>(size),"archive");output.close();Require(output.good(),"disk");
        if(progress)progress(files,0);
    }
    Require(files>0&&archive_read_has_encrypted_entries(archive)<=0,"archive");
}
PreparedSystemImport::PreparedSystemImport(fs::path stage_,FileImport kind_,SystemReview review_) :
    stage{std::move(stage_)},kind{kind_},review{std::move(review_)} {}
PreparedSystemImport::~PreparedSystemImport(){RemoveSystemStage(stage,kind);}
void PreparedSystemImport::Commit(std::stop_token stop) {
    CheckStop(stop);Require(StagePath(stage,kind),"destination");
    const auto destination=SystemDestination(kind);RecoverSystemDirectory(destination);
    const auto previous=fs::path{destination}.concat(L".previous");const bool exists=fs::exists(destination);
    if(exists)fs::rename(destination,previous);
    bool published=false;
    try{
        auto broker_stage=stage;broker_stage.make_preferred();
        auto folder=winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(broker_stage.wstring()).get();
        folder.RenameAsync(destination.filename().wstring(),winrt::Windows::Storage::NameCollisionOption::FailIfExists).get();
        published=true;
        if(kind==FileImport::Keys)Core::Crypto::KeyManager::Instance().ReloadKeys();
    }catch(...){
        if(published)fs::rename(destination,stage);
        if(exists)fs::rename(previous,destination);
        if(kind==FileImport::Keys)try{Core::Crypto::KeyManager::Instance().ReloadKeys();}catch(...){}
        throw;
    }
    stage.clear();
    std::error_code error;if(exists)fs::remove_all(previous,error);
}
std::unique_ptr<PreparedSystemImport> InspectSystemStage(fs::path stage,FileImport kind,SystemProgress progress,std::stop_token stop) {
    Require(StagePath(stage,kind),"destination");Internal(stage);CheckStop(stop);SystemReview review;
    FirmwareInfo installed;
    try{installed=InspectFirmware(SystemDestination(FileImport::Firmware),false,{},stop);}catch(...){CheckStop(stop);review.warnings.push_back("installed_unknown");}
    review.installed_firmware=installed.display;review.required_generation=installed.generation;
    try{const auto generations=Generations(ReadKeys(SystemDestination(FileImport::Keys)/"prod.keys"));
        review.installed_generation=generations.empty()?-1:*generations.rbegin();}catch(...){}
    if(kind==FileImport::Firmware) {
        const auto incoming=InspectFirmware(stage,true,progress,stop);
        review.incoming_firmware=incoming.display;review.required_generation=incoming.generation;
        review.incoming_generation=review.installed_generation;
        if(!installed.display.empty()&&incoming.version<installed.version)review.warnings.push_back("firmware_downgrade");
        if(incoming.removed)review.warnings.push_back("firmware_games_removed");
    }else{
        review.incoming_firmware=installed.display;
        const auto destination=SystemDestination(kind);
        // Match USB semantics: preserve generated/console files and an omitted optional file.
        if(fs::exists(destination))for(const auto& file:fs::directory_iterator{destination}) {
            CheckStop(stop);if(file.is_regular_file()&&!fs::exists(stage/file.path().filename()))fs::copy_file(file.path(),stage/file.path().filename());
        }
        auto keys=ReadKeys(stage/"prod.keys");
        const auto header=keys.find("header_key");Require(header!=keys.end()&&header->second.size()==64&&
            Nonzero(header->second)&&header->second.substr(0,32)!=header->second.substr(32),"keys_header");
        if(fs::exists(stage/"title.keys"))ReadKeys(stage/"title.keys",true);
        std::set<int> old;try{old=Generations(ReadKeys(destination/"prod.keys"));}catch(...){}
        const auto incoming=Generations(keys);
        review.installed_generation=old.empty()?-1:*old.rbegin();review.incoming_generation=incoming.empty()?-1:*incoming.rbegin();
        if(incoming.empty())review.warnings.push_back("keys_unknown");
        if(!old.empty()&&!std::includes(incoming.begin(),incoming.end(),old.begin(),old.end()))review.warnings.push_back("keys_downgrade");
        bool coverage=true;
        if(installed.generation>=0) {
            // Compare candidate material to keys which actually opened installed NCAs.
            auto& current=Core::Crypto::KeyManager::Instance();
            constexpr char hex[]="0123456789abcdef";std::string current_header;
            for(auto value:current.GetKey(Core::Crypto::S256KeyType::Header)){current_header+=hex[value>>4];current_header+=hex[value&15];}
            coverage=header->second==current_header;
            KeyMap effective;
            for(const auto* name:{"prod.keys_autogenerated","prod.keys","console.keys_autogenerated","console.keys"})
                if(fs::exists(stage/name))for(const auto& [key,value]:ReadKeys(stage/name))effective[key]=value;
            const auto decode=[&](const std::string& name){Core::Crypto::Key128 result{};const auto found=effective.find(name);
                if(found==effective.end())return result;Require(found->second.size()==32,"keys");
                for(std::size_t i=0;i<result.size();++i)result[i]=static_cast<u8>(std::stoi(found->second.substr(i*2,2),nullptr,16));return result;};
            for(const auto& [generation,index]:installed.areas) {
                constexpr std::array<const char*,3> area_names{"application","ocean","system"};
                const std::string suffix{hex[generation>>4],hex[generation&15]};
                const auto master=decode("master_key_"+suffix),source=decode(std::string{"key_area_key_"}+area_names[index]+"_source");
                const auto kek=decode("aes_kek_generation_source"),seed=decode("aes_key_generation_source");
                const auto candidate=master!=Core::Crypto::Key128{}&&source!=Core::Crypto::Key128{}&&kek!=Core::Crypto::Key128{}?
                    Core::Crypto::GenerateKeyEncryptionKey(source,master,kek,seed):decode(std::string{"key_area_key_"}+area_names[index]+"_"+suffix);
                if(candidate!=current.GetKey(Core::Crypto::S128KeyType::KeyArea,static_cast<u64>(generation),static_cast<u64>(index)))coverage=false;
            }
        }
        if(!coverage)review.warnings.push_back("keys_firmware_old");
    }
    // All prepared data must reach disk before publication can retire the old directory.
    for(const auto& entry:fs::directory_iterator{stage})if(entry.is_regular_file()){
        CheckStop(stop);CREATEFILE2_EXTENDED_PARAMETERS parameters{};parameters.dwSize=sizeof(parameters);parameters.dwFileAttributes=FILE_ATTRIBUTE_NORMAL;
        const auto handle=CreateFile2FromAppW(entry.path().c_str(),GENERIC_WRITE,FILE_SHARE_READ,OPEN_EXISTING,&parameters);
        Require(handle!=INVALID_HANDLE_VALUE,"disk");const bool flushed=FlushFileBuffers(handle)!=0;CloseHandle(handle);Require(flushed,"disk");
    }
    CheckStop(stop);return std::make_unique<PreparedSystemImport>(std::move(stage),kind,std::move(review));
}
std::wstring DescribeSystemReview(const SystemReview& review) {
    using namespace I18n;
    const auto wide=[](const std::string& s){return std::wstring{winrt::to_hstring(s)};};
    auto result=Format(Text::SystemImportVersions,{wide(review.installed_firmware.empty()?"—":review.installed_firmware),
        wide(review.incoming_firmware.empty()?"—":review.incoming_firmware),review.installed_generation<0?L"—":std::to_wstring(review.installed_generation),
        review.incoming_generation<0?L"—":std::to_wstring(review.incoming_generation)});
    for(const auto& warning:review.warnings){result+=L"\n";result+=Tr(warning=="firmware_downgrade"?Text::SystemFirmwareDowngrade:
        warning=="keys_downgrade"?Text::SystemKeysDowngrade:warning=="keys_firmware_old"?Text::SystemKeysFirmwareWarning:
        warning=="firmware_games_removed"?Text::SystemFirmwareGamesRemoved:Text::SystemVersionUnknown);}
    return result;
}
SystemImportBackend DefaultSystemImportBackend(){return {SystemDestination,CreateSystemStage,RemoveSystemStage,ExtractSystemZip,InspectSystemStage};}
}
