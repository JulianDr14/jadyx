// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/system_upload.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <windows.h>
#include <fileapifromapp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>

namespace Jadyx {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr std::uint64_t MiB=1ULL<<20,GiB=1ULL<<30;
std::string Lower(std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
std::string Unique(){std::array<unsigned char,16> data{};if(RAND_bytes(data.data(),static_cast<int>(data.size()))!=1)throw std::runtime_error("random");
    constexpr char hex[]="0123456789abcdef";std::string value;for(auto byte:data){value+=hex[byte>>4];value+=hex[byte&15];}return value;}
void Flush(const fs::path& file){CREATEFILE2_EXTENDED_PARAMETERS p{};p.dwSize=sizeof(p);p.dwFileAttributes=FILE_ATTRIBUTE_NORMAL;
    const auto handle=CreateFile2FromAppW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,OPEN_EXISTING,&p);
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("disk");const bool ok=FlushFileBuffers(handle)!=0;CloseHandle(handle);
    if(!ok)throw std::runtime_error("disk");}
}
struct SystemUpload::Impl {
    fs::path root,incoming,stage;
    mutable std::mutex mutex;
    std::mutex operation;
    SystemUploadStatus info;
    std::unique_ptr<NspUpload> file;
    std::unique_ptr<SystemPrepared> prepared;
    SystemImportBackend backend;
    std::stop_source cancellation;
    std::uint64_t generation{};
    std::vector<std::string> manifest;
    FileImport Kind() const{return info.kind=="keys"?FileImport::Keys:FileImport::Firmware;}
    explicit Impl(fs::path local,SystemImportBackend backend_):root{std::move(local)/"jadyx/system-upload"},incoming{root/"incoming"},backend{std::move(backend_)}{
        fs::create_directories(incoming);Recover();
    }
    std::vector<std::string> Files() const {
        std::vector<std::string> result;
        for(const auto& entry:fs::directory_iterator{incoming})if(entry.is_regular_file())result.push_back(entry.path().filename().string());
        return result;
    }
    void Save(const std::string& phase) {
        const auto files=manifest.empty()?Files():manifest;
        Json record{{"generation",generation+1},{"id",info.id},{"kind",info.kind},{"format",info.format},{"count",info.count},
            {"phase",phase},{"stage",stage.empty()?std::string{}:stage.filename().string()},{"files",files}};
        record["received"]=Json::array();for(const auto& f:info.files)record["received"].push_back({{"name",f.name},{"fingerprint",f.fingerprint},{"length",f.length}});
        auto text=record.dump();if(text.size()>1024*1024)throw std::runtime_error("file_limit");
        const auto temp=root/"state.tmp";
        {std::ofstream output{temp,std::ios::binary|std::ios::trunc};output<<text;output.close();if(!output)throw std::runtime_error("disk");}
        Flush(temp);const auto path=root/((generation+1)%2?"state1.json":"state0.json");
        std::error_code ec;fs::remove(path,ec);fs::rename(temp,path);++generation;
    }
    void Recount(){
        const auto previous=info.files;info.files.clear();info.completed=0;info.bytes=0;
        const auto current=file?file->Snapshot():UploadSnapshot{};
        for(const auto& entry:fs::directory_iterator{incoming})if(entry.is_regular_file()){
            const auto name=entry.path().filename().string();const auto length=entry.file_size();
            const auto saved=std::find_if(previous.begin(),previous.end(),[&](const auto& f){return f.name==name&&f.length==length;});
            const auto fingerprint=saved!=previous.end()?saved->fingerprint:current.name==name&&current.length==length?current.fingerprint:std::string{};
            info.files.push_back({name,fingerprint,length});++info.completed;info.bytes+=length;
        }
    }
    void RestoreInput(){
        if(stage.empty())return;
        if(info.format=="files"&&fs::exists(stage))for(const auto& name:manifest){
            if(!ValidSystemFilename(name,Kind()))throw std::runtime_error("recovery");
            if(!fs::exists(incoming/name)&&fs::exists(stage/name))fs::rename(stage/name,incoming/name);
        }
        prepared.reset();if(backend.remove_stage)backend.remove_stage(stage,Kind());stage.clear();
    }
    void Recover(){
        Json record;
        for(const auto* name:{"state0.json","state1.json"})try{
            const auto path=root/name;if(!fs::exists(path)||fs::file_size(path)>1024*1024)continue;
            std::ifstream input{path};auto next=Json::parse(input);const auto value=next.at("generation").get<std::uint64_t>();
            if(value>generation){record=std::move(next);generation=value;}
        }catch(...){}
        if(record.is_null())return;
        try{
            const auto id=record.at("id").get<std::string>(),kind=record.at("kind").get<std::string>(),format=record.at("format").get<std::string>();
            const auto count=record.at("count").get<unsigned>();
            if(id.size()!=32||id.find_first_not_of("0123456789abcdef")!=std::string::npos||
                (kind!="keys"&&kind!="firmware")||(format!="zip"&&format!="files")||!count||count>(kind=="keys"?2U:4096U))throw std::runtime_error("recovery");
            info.id=id;info.kind=kind;info.format=format;info.count=count;info.phase="receiving";info.busy=true;
            if(record.value("phase","")=="complete"){info.phase="complete";info.busy=false;return;}
            manifest=record.at("files").get<std::vector<std::string>>();
            if(record.contains("received"))for(const auto& f:record["received"]){
                if(info.files.size()>=4096)throw std::runtime_error("recovery");
                info.files.push_back({f.at("name").get<std::string>(),f.at("fingerprint").get<std::string>(),f.at("length").get<std::uint64_t>()});
            }
            const auto filename=record.value("stage",std::string{});
            if(!filename.empty()){
                if(!backend.destination)throw std::runtime_error("recovery");
                const auto destination=backend.destination(Kind());
                const auto prefix=destination.filename().string()+".import-";
                if(!filename.starts_with(prefix)||filename.size()!=prefix.size()+32||filename.substr(prefix.size()).find_first_not_of("0123456789abcdef")!=std::string::npos)
                    throw std::runtime_error("recovery");
                stage=destination.parent_path()/filename;
                if(!fs::exists(stage)&&record.value("phase","")=="publishing")throw std::runtime_error("check_installation");
                if(info.format=="files"&&fs::exists(stage))for(const auto& value:record.at("files")){
                    const auto name=value.get<std::string>();if(!ValidSystemFilename(name,Kind()))throw std::runtime_error("recovery");
                    if(!fs::exists(incoming/name)&&fs::exists(stage/name))fs::rename(stage/name,incoming/name);
                }
                if(backend.remove_stage)backend.remove_stage(stage,Kind());stage.clear();
            }
            file=std::make_unique<NspUpload>(incoming,true,root/"transfer");Recount();
            if(record.value("phase","")=="complete"){info.phase="complete";info.busy=false;}
        }catch(...){info.phase="failed";info.error="recovery";info.busy=true;}
    }
};
SystemUpload::SystemUpload(fs::path local,SystemImportBackend backend):impl{std::make_unique<Impl>(std::move(local),std::move(backend))}{}
SystemUpload::~SystemUpload(){Cancel();}
SystemUploadStatus SystemUpload::Snapshot() const {std::lock_guard lock{impl->mutex};auto result=impl->info;if(impl->file)result.file=impl->file->Snapshot();return result;}
void SystemUpload::SetAvailable(bool enabled){std::lock_guard lock{impl->mutex};impl->info.available=enabled;if(impl->file)impl->file->SetAvailable(enabled);}
bool SystemUpload::BlockForLocalOperation(){
    // A batch left receiving or failed touches nothing outside its transfer directory, so only
    // bytes in flight and the extract/review/publish phases hold local operations back.
    std::unique_lock operation{impl->operation,std::try_to_lock};if(!operation.owns_lock())return false;
    std::lock_guard lock{impl->mutex};const auto& phase=impl->info.phase;
    if(!phase.empty()&&phase!="receiving"&&phase!="failed"&&phase!="complete")return false;
    if(impl->file&&!impl->file->BlockForLocalOperation())return false;
    impl->info.available=false;return true;}
UploadResult SystemUpload::Create(std::string kind,std::string format,unsigned count){
    if((kind!="keys"&&kind!="firmware")||(format!="zip"&&format!="files")||!count||count>(format=="zip"?1U:kind=="keys"?2U:4096U))return {400,"file"};
    std::unique_lock operation{impl->operation,std::try_to_lock};if(!operation.owns_lock())return {409,"busy"};
    std::lock_guard lock{impl->mutex};if(!impl->info.available||impl->info.busy)return {409,"busy"};
    try{
        impl->prepared.reset();impl->file.reset();
        for(const auto& name:{"state0.json","state1.json","state.tmp"}){std::error_code ec;fs::remove(impl->root/name,ec);}
        // All paths are fixed children of our own private transfer directory.
        fs::remove_all(impl->incoming);fs::create_directories(impl->incoming);fs::remove_all(impl->root/"transfer");
        impl->cancellation=std::stop_source{};impl->generation=0;impl->manifest.clear();
        impl->info={.id=Unique(),.kind=std::move(kind),.format=std::move(format),.phase="receiving",.count=count,.busy=true,.available=true};
        impl->Save("receiving");impl->file=std::make_unique<NspUpload>(impl->incoming,true,impl->root/"transfer");impl->file->SetAvailable(true);
        return {201,{}};
    }catch(...){impl->info.phase="failed";impl->info.error="disk";return {507,"disk"};}
}
UploadResult SystemUpload::BeginFile(const std::string& id,std::string name,std::string fingerprint,std::uint64_t length){
    std::unique_lock operation{impl->operation,std::try_to_lock};if(!operation.owns_lock())return {409,"busy"};
    std::lock_guard lock{impl->mutex};auto& s=impl->info;
    if(id!=s.id||s.id.empty())return {404,"upload"};
    if(!s.available||s.phase!="receiving"||!impl->file)return {409,"busy"};
    name=Lower(std::move(name));
    if(s.completed>=s.count)return {409,"file_limit"};
    if(s.format=="zip"?!name.ends_with(".zip"):!ValidSystemFilename(name,impl->Kind()))return {400,"file"};
    const auto limit=s.format=="zip"?(s.kind=="keys"?16*MiB:4*GiB):(s.kind=="keys"?4*MiB:8*GiB);
    if(!length||length>limit||s.bytes>8*GiB-length)return {413,"archive_limit"};
    return impl->file->Create(std::move(name),std::move(fingerprint),length);
}
UploadResult SystemUpload::Append(const std::string& id,std::uint64_t offset,std::uint64_t length,const UploadReader& reader){
    std::unique_lock operation{impl->operation,std::try_to_lock};if(!operation.owns_lock())return {409,"busy"};
    NspUpload* file;
    {std::lock_guard lock{impl->mutex};if(impl->info.phase!="receiving"||!impl->info.available||!impl->file)return {409,"busy"};file=impl->file.get();}
    return file->Append(id,offset,length,reader);
}
UploadResult SystemUpload::FinishFile(const std::string& id){
    std::unique_lock operation{impl->operation,std::try_to_lock};if(!operation.owns_lock())return {409,"busy"};
    NspUpload* file;{std::lock_guard lock{impl->mutex};if(impl->info.phase!="receiving"||!impl->file)return {409,"busy"};file=impl->file.get();}
    const auto ready=file->Ready(id);if(ready.status!=204)return ready;
    const auto error=file->Finalize([](const auto&,auto stop){return stop.stop_requested()?std::string{"interrupted"}:std::string{};});
    if(!error.empty())return {400,error};
    std::lock_guard lock{impl->mutex};impl->Recount();
    try{impl->Save("receiving");return {};}catch(...){impl->info.phase="failed";impl->info.error="disk";return {507,"disk"};}
}
UploadResult SystemUpload::Prepare(const std::string& id){
    std::lock_guard lock{impl->mutex};auto& s=impl->info;
    if(id!=s.id||id.empty())return {404,"upload"};
    if(!s.available)return {409,"busy"};
    if(s.phase!="receiving"||s.completed!=s.count)return {409,"incomplete"};
    if(impl->file&&impl->file->Snapshot().phase!="complete")return {409,"incomplete"};
    s.phase="prepare_pending";s.error.clear();return {202,{}};
}
UploadResult SystemUpload::Confirm(const std::string& id,bool accepted){
    std::lock_guard lock{impl->mutex};auto& s=impl->info;
    if(id!=s.id||id.empty())return {404,"upload"};
    if(!s.available||s.phase!="review"||!impl->prepared)return {409,"busy"};
    if(!s.review.warnings.empty()&&!accepted)return {409,"confirm_warnings"};
    s.phase="commit_pending";return {202,{}};
}
void SystemUpload::Cancel(){impl->cancellation.request_stop();std::lock_guard lock{impl->mutex};if(impl->file)impl->file->Cancel();}
void SystemUpload::Suspend(){Cancel();std::lock_guard operation{impl->operation};std::lock_guard lock{impl->mutex};
    if(impl->file)impl->file->Suspend();impl->info.available=false;impl->info.busy=false;
    if(!impl->info.id.empty()&&impl->info.phase!="complete")try{
        impl->RestoreInput();impl->Recount();impl->info.phase="receiving";impl->Save("receiving");
    }catch(...){impl->info.phase="failed";impl->info.error="recovery";}}
void SystemUpload::Resume(){std::lock_guard operation{impl->operation};std::lock_guard lock{impl->mutex};
    impl->cancellation=std::stop_source{};if(impl->file)impl->file->Resume();
    impl->info.busy=!impl->info.id.empty()&&impl->info.phase!="complete";}
UploadResult SystemUpload::Discard(const std::string& id){
    {std::lock_guard lock{impl->mutex};if(id!=impl->info.id)return {404,"upload"};}
    Cancel();std::unique_lock operation{impl->operation,std::try_to_lock};if(!operation.owns_lock())return {409,"busy"};
    std::lock_guard lock{impl->mutex};impl->prepared.reset();if(impl->backend.remove_stage)impl->backend.remove_stage(impl->stage,impl->Kind());impl->stage.clear();impl->file.reset();
    try{for(const auto& name:{"incoming","transfer","state0.json","state1.json","state.tmp"})fs::remove_all(impl->root/name);
        fs::create_directories(impl->incoming);const bool available=impl->info.available;impl->info={};impl->info.available=available;return {};
    }catch(...){return {507,"disk"};}
}
std::string SystemUpload::RunPending(std::stop_token stop){
    std::lock_guard operation{impl->operation};std::stop_callback cancel{stop,[&]{Cancel();}};
    std::string phase;FileImport kind;
    {std::lock_guard lock{impl->mutex};phase=impl->info.phase;kind=impl->Kind();if(phase!="prepare_pending"&&phase!="commit_pending")return "busy";
        impl->info.phase=phase=="prepare_pending"?"validating":"publishing";}
    try{
        const auto token=impl->cancellation.get_token();
        if(token.stop_requested())throw std::runtime_error("interrupted");
        if(phase=="prepare_pending"){
            const auto progress=[&](unsigned done,unsigned){std::lock_guard lock{impl->mutex};impl->info.processed=done;};
            if(!impl->backend.create_stage||!impl->backend.inspect||!impl->backend.extract)throw std::runtime_error("unavailable");
            impl->stage=impl->backend.create_stage(kind);
            {std::lock_guard lock{impl->mutex};impl->manifest=impl->Files();impl->Save("preparing");}
            if(impl->info.format=="zip"){
                {std::lock_guard lock{impl->mutex};impl->info.phase="extracting";}
                const auto files=impl->Files();if(files.size()!=1)throw std::runtime_error("incomplete");
                impl->backend.extract(impl->incoming/files.front(),impl->stage,kind,progress,token);
            }else{
                for(const auto& name:impl->Files()){if(token.stop_requested())throw std::runtime_error("interrupted");fs::rename(impl->incoming/name,impl->stage/name);}
            }
            {std::lock_guard lock{impl->mutex};impl->info.phase="validating";}
            auto prepared=impl->backend.inspect(impl->stage,kind,progress,token);
            std::lock_guard lock{impl->mutex};impl->info.review=prepared->Review();impl->prepared=std::move(prepared);impl->info.phase="review";
        }else{
            {std::lock_guard lock{impl->mutex};impl->Save("publishing");}
            if(!impl->prepared)throw std::runtime_error("recovery");
            impl->prepared->Commit(token);
            std::lock_guard lock{impl->mutex};impl->prepared.reset();impl->stage.clear();impl->info.phase="complete";impl->info.busy=false;
            // Publication has succeeded. Cleanup/checkpoint failures must never turn it
            // into a failed installation or invite a second publication on this run.
            try{impl->Save("complete");}catch(...){}
            impl->file.reset();
            std::error_code cleanup;
            fs::remove_all(impl->incoming,cleanup);fs::create_directories(impl->incoming,cleanup);fs::remove_all(impl->root/"transfer",cleanup);
        }
        return {};
    }catch(const std::exception& error){std::lock_guard lock{impl->mutex};impl->info.phase="failed";
        const std::string code=error.what();
        // Only machine error identifiers leave the worker; filesystem/WinRT
        // diagnostics may contain private paths and are not portal responses.
        impl->info.error=!code.empty()&&code.size()<=64&&code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_")==std::string::npos?code:"disk";
        return impl->info.error;}
    catch(...){std::lock_guard lock{impl->mutex};impl->info.phase="failed";impl->info.error="disk";return "disk";}
}
}
