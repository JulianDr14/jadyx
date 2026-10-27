// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/system_upload.h"

namespace {
void SystemUploadTests(int port) {
    namespace fs = std::filesystem;
    const auto root = fs::absolute("build-uwp/test-results/network-portal/system-fixture");
    fs::remove_all(root); fs::create_directories(root);
    const auto live = root / "keys";
    fs::create_directories(live);
    { std::ofstream out{live/"marker"}; out << "previous"; }
    unsigned publications{}, extractions{};
    bool reject_keys{};
    struct Prepared final : Jadyx::SystemPrepared {
        fs::path stage,live;
        unsigned& publications;
        Jadyx::SystemReview review;
        Prepared(fs::path s,fs::path l,unsigned& p):stage{std::move(s)},live{std::move(l)},publications{p} {
            review.installed_generation=20;review.incoming_generation=19;
            review.warnings={"keys_downgrade","keys_firmware_old"};
        }
        const Jadyx::SystemReview& Review() const override {return review;}
        void Commit(std::stop_token stop) override {
            if(stop.stop_requested())throw std::runtime_error("interrupted");
            fs::rename(live,live.string()+".previous");fs::rename(stage,live);++publications;
        }
    };
    Jadyx::SystemImportBackend backend;
    backend.destination=[&](auto){return live;};
    backend.create_stage=[&](auto){const auto p=root/"keys.import-0123456789abcdef0123456789abcdef";fs::create_directories(p);return p;};
    backend.remove_stage=[](const auto& p,auto){if(!p.empty())fs::remove_all(p);};
    backend.extract=[&](const auto&,const auto& stage,auto,auto progress,auto stop){
        Check(!stop.stop_requested(),"ZIP backend cancellation token");
        ++extractions;std::ofstream out{stage/"prod.keys"};out<<"synthetic";progress(1,1);
    };
    backend.inspect=[&](auto stage,auto,auto,auto)->std::unique_ptr<Jadyx::SystemPrepared>{
        Check(fs::exists(stage/"prod.keys"),"only staged files reach validation");
        if(reject_keys)throw std::runtime_error("firmware_keys");
        return std::make_unique<Prepared>(std::move(stage),live,publications);
    };
    const std::string payload="synthetic, never real keys";
    const std::string fingerprint(64,'d');
    std::string job,file;
    {
        Jadyx::SystemUpload upload{root,backend};upload.SetAvailable(true);
        Check(upload.Snapshot().phase=="idle"&&upload.BlockForLocalOperation(),"no batch does not hold local operations");
        upload.SetAvailable(true);
        Check(upload.Create("keys","files",3).status==400,"key batch count bounded");
        Check(upload.Create("keys","files",1).status==201,"create key batch");job=upload.Snapshot().id;
        Check(upload.BeginFile(job,"../prod.keys","fixture",payload.size()).status==400,"reject key traversal");
        Check(upload.BeginFile(job,"prod.keys",fingerprint,payload.size()).status==201,"create loose key");file=upload.Snapshot().file.id;
        Check(upload.Append(file,0,8,[&](const auto& sink){return sink(payload.data(),8);}).status==204,"ack partial key block");
        upload.Suspend();
    }
    {
        Jadyx::SystemUpload upload{root,backend};upload.Resume();upload.SetAvailable(true);
        Check(upload.Snapshot().file.offset==8,"recover confirmed system-file offset");
        Check(upload.Append(file,8,payload.size()-8,[&](const auto& sink){return sink(payload.data()+8,payload.size()-8);}).status==204,"resume system-file bytes");
        Check(upload.FinishFile(file).status==204,"finish loose file");
        Check(upload.Snapshot().files.at(0).fingerprint==fingerprint,"retain completed file identity");
        Check(upload.Prepare(job).status==202&&upload.RunPending({}).empty(),"prepare batch");
        Check(upload.Snapshot().phase=="review"&&publications==0&&fs::exists(live/"marker"),"review never publishes");
        Check(upload.Confirm(job,false).status==409,"warnings require explicit acceptance");
        upload.Suspend();
    }
    {
        Jadyx::SystemUpload upload{root,backend};upload.Resume();upload.SetAvailable(true);
        Check(upload.Snapshot().completed==1&&upload.Snapshot().phase=="receiving","restart restores raw staged input for new review");
        Check(upload.Prepare(job).status==202&&upload.RunPending({}).empty(),"repeat review after restart");
        Check(upload.Confirm(job,true).status==202&&upload.RunPending({}).empty(),"accepted review publishes");
        Check(publications==1&&upload.Snapshot().phase=="complete"&&fs::exists(live/"prod.keys"),"success only after publication");
        fs::remove_all(root/"keys.previous");
        Check(upload.Create("firmware","zip",1).status==201,"create firmware ZIP batch");job=upload.Snapshot().id;
        Check(upload.BeginFile(job,"firmware.zip",fingerprint,payload.size()).status==201,"ZIP source accepted");file=upload.Snapshot().file.id;
        Check(upload.Append(file,0,payload.size(),[&](const auto& sink){return sink(payload.data(),payload.size());}).status==204,"ZIP source streamed");
        Check(upload.FinishFile(file).status==204&&upload.Prepare(job).status==202&&upload.RunPending({}).empty(),"ZIP extraction precedes review");
        Check(extractions==1&&publications==1,"ZIP review does not publish");
        Check(!upload.BlockForLocalOperation(),"review excludes local operations");
        Check(upload.Discard(job).status==204&&fs::exists(live/"prod.keys")&&publications==1,"cancel leaves installed data intact");
        reject_keys=true;
        Check(upload.Create("firmware","zip",1).status==201,"create rejected firmware batch");job=upload.Snapshot().id;
        Check(upload.BeginFile(job,"firmware.zip",fingerprint,payload.size()).status==201,"rejected ZIP accepted");file=upload.Snapshot().file.id;
        Check(upload.Append(file,0,payload.size(),[&](const auto& sink){return sink(payload.data(),payload.size());}).status==204,"rejected ZIP streamed");
        Check(upload.FinishFile(file).status==204&&upload.Prepare(job).status==202&&upload.RunPending({})=="firmware_keys","validation failure reported");
        Check(upload.BlockForLocalOperation(),"failed batch does not hold local operations");
        Check(upload.Snapshot().phase=="failed"&&upload.Discard(job).status==204,"failed batch stays discardable");
        upload.Suspend();
    }
    {
        Jadyx::SystemUpload upload{root,backend};upload.Resume();upload.SetAvailable(true);
        Check(upload.Create("firmware","zip",1).status==201,"create parked firmware batch");job=upload.Snapshot().id;
        Check(upload.BeginFile(job,"firmware.zip",fingerprint,payload.size()).status==201,"parked ZIP accepted");file=upload.Snapshot().file.id;
        Check(upload.Append(file,0,payload.size(),[&](const auto& sink){return sink(payload.data(),payload.size());}).status==204&&upload.FinishFile(file).status==204,"parked ZIP received");
        upload.Suspend();
    }
    {
        Jadyx::SystemUpload upload{root,backend};upload.Resume();upload.SetAvailable(true);
        Check(upload.Snapshot().busy&&upload.Snapshot().phase=="receiving","restart recovers the parked batch");
        Check(upload.BlockForLocalOperation(),"parked batch does not hold local operations");
        Check(upload.Prepare(job).status==409,"a local operation holds preparation back");
        upload.SetAvailable(true);
        Check(upload.Discard(job).status==204,"parked batch discarded");
        reject_keys=false;
    }
    {
        Jadyx::NetworkPortal portal{root/"games",backend};
        Check(portal.Start("127.0.0.1",static_cast<unsigned short>(port)),"system API starts");portal.SetUploadsAvailable(true);
        httplib::Client client{"127.0.0.1",port};
        httplib::Headers headers{{"Origin","http://127.0.0.1:"+std::to_string(port)},{"Tus-Resumable","1.0.0"}};
        auto paired=client.Post("/api/v1/pair",headers,Json{{"code",portal.Status().code}}.dump(),"application/json");
        Check(Status(paired)==200,"system API pairing");headers.emplace("Authorization","Bearer "+Json::parse(paired->body).at("session").get<std::string>());
        auto created=client.Post("/api/v1/system/jobs",headers,Json{{"kind","keys"},{"format","files"},{"count",1}}.dump(),"application/json");
        Check(Status(created)==201,"system API creates batch");job=Json::parse(created->body).at("id").get<std::string>();
        Check(Status(client.Post("/api/v1/uploads",headers,"","application/octet-stream"))==409,"system batch excludes NSP admission");
        auto source=client.Post("/api/v1/system/jobs/"+job+"/files",headers,Json{{"name","prod.keys"},{"fingerprint",fingerprint},{"length",payload.size()}}.dump(),"application/json");
        Check(Status(source)==201,"system API creates file");const auto location=source->get_header_value("Location");
        auto patch_headers=headers;patch_headers.emplace("Upload-Offset","0");
        Check(Status(client.Patch(location,patch_headers,payload,"application/offset+octet-stream"))==204,"system API streams bytes");
        Check(Status(client.Post(location+"/finish",headers,"","application/json"))==204,"system API finishes source");
        Check(Status(client.Post("/api/v1/system/jobs/"+job+"/prepare",headers,"","application/json"))==202&&portal.RunSystemImport({}).empty(),"system API coordinator prepares");
        Check(Status(client.Post("/api/v1/system/jobs/"+job+"/confirm",headers,"{}","application/json"))==409,"HTTP warning bypass rejected");
        Check(Status(client.Post("/api/v1/system/jobs/"+job+"/confirm",headers,"{\"accept_warnings\":true}","application/json"))==202&&portal.RunSystemImport({}).empty(),"system HTTP confirmation publishes");
        Check(portal.Status().system.phase=="complete"&&publications==2,"system API reports committed completion");portal.Stop();
    }
    fs::remove_all(root);
    std::puts("PASS: system batches and authenticated HTTP, streamed resume/restart, ZIP dispatch, explicit warnings, NSP exclusion, parked and failed batches not blocking play, publication and discard (synthetic backend)");
}
}
