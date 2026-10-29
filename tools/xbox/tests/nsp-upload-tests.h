// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <array>
#include <fstream>
#include <vector>
#include <future>
#include <winioctl.h>
#include "jadyx/storage/nsp_upload.h"

// Included by the native TCP harness; validation callback is deliberately synthetic.
// Real CNMT/NCA validation is a separate UWP object + manual gate with the user's own dump.
void UploadTests(int port, bool large) {
    namespace fs=std::filesystem;
    using Jadyx::NspUpload;
    const auto parent=fs::absolute("build-uwp/test-results/network-portal");
    const auto fixture=parent/("fixture-"+std::to_string(GetCurrentProcessId()));
    Check(fixture.parent_path()==parent && !fs::exists(fixture),"isolated fixture path");
    fs::create_directories(fixture);
    const auto games=fixture/"games";
    const auto staging=fixture/"jadyx/transfers/nsp";
    std::vector<char> pattern(1 << 20);
    for (size_t i=0;i<pattern.size();++i) pattern[i]=static_cast<char>((i*31+(i >> 7)) & 255);
    const auto producer=[&](std::uint64_t size) -> Jadyx::UploadReader {
        return [&,size](const Jadyx::UploadSink& sink) {
            for (std::uint64_t sent=0;sent<size;) {
                const auto count=static_cast<size_t>((std::min)(size-sent,std::uint64_t{pattern.size()}));
                if (!sink(pattern.data(),count)) return false;
                sent+=count;
            }
            return true;
        };
    };
    auto store=std::make_unique<NspUpload>(games);
    store->SetAvailable(true);
    const std::string fingerprint(64,'a');
    Check(store->Create("payload.bin",fingerprint,1).status==400,"default upload mode keeps NSP restriction");
    Check(store->Create("../outside.nsp",fingerprint,32ULL << 20).status==400,"path traversal refused");
    Check(store->Create("CON.nsp",fingerprint,32ULL << 20).status==400,"Windows device name refused");
    Check(store->Create("one.nsp",fingerprint,NspUpload::MaxLength+1).status==400,"length bounded");
    Check(store->Create("one.nsp",fingerprint,32ULL << 20).status==201,"create transfer");
    auto id=store->Snapshot().id;
    Check(!store->BlockForLocalOperation(),"active file excludes gameplay/local import");
    Check(store->Create("two.nsp",fingerprint,32).status==409,"one transfer active");
    Check(store->Append(id,0,16ULL << 20,[&](const auto& sink){sink(pattern.data(),pattern.size());return false;}).status==400,
        "disconnect rolls back unconfirmed bytes");
    Check(store->Snapshot().offset==0 && fs::file_size(staging/"payload.part")==0,"rollback truncates partial request");
    Check(store->Append(id,0,16ULL << 20,producer(16ULL << 20)).status==204,"first confirmed chunk");
    const auto measured=store->Snapshot().performance;
    Check(measured.confirmed_offset==16ULL << 20 && measured.bytes==16ULL << 20 && measured.reader_calls>0 &&
          measured.total_us>=measured.write_us+measured.flush_us+measured.checkpoint_us,"last acknowledged block phase timings");
    Check(store->Append(id,0,1,producer(1)).status==409,"offset conflict cannot overwrite confirmed bytes");
    Check(store->Append(id,16ULL << 20,NspUpload::MaxChunk+1,producer(1)).status==413,"bounded request");
    Check(store->Ready(id).status==409,"incomplete file cannot finalize");
    fs::create_directory(staging/"checkpoint.tmp");
    Check(store->Append(id,16ULL << 20,16ULL << 20,producer(16ULL << 20)).status==507,"checkpoint I/O failure");
    Check(store->Snapshot().offset==16ULL << 20 && fs::file_size(staging/"payload.part")==16ULL << 20,"failed checkpoint cannot acknowledge data");
    Check(store->Snapshot().performance.confirmed_offset==16ULL << 20,"failed block cannot publish performance as acknowledged");
    fs::remove(staging/"checkpoint.tmp");
    // Simulate death after extra data reached disk but before a new checkpoint was published.
    { std::ofstream trailing{staging/"payload.part",std::ios::binary|std::ios::app}; trailing.write("unconfirmed",11); }
    store.reset();
    store=std::make_unique<NspUpload>(games);
    Check(store->Snapshot().offset==(16ULL << 20) && fs::file_size(staging/"payload.part")==16ULL << 20,"restart truncates unconfirmed tail");
    store->SetAvailable(true);
    Check(store->Append(id,16ULL << 20,16ULL << 20,producer(16ULL << 20)).status==204,"resume confirmed offset");
    Check(store->Ready(id).status==204,"completed stream enters validation");
    Check(store->Finalize([](const auto&,auto){return std::string{"base"};})=="base" && !fs::exists(games/"one.nsp"),"reject invalid package without publishing");
    { std::ofstream conflict{games/"existing.nsp"}; conflict<<"preserve"; }
    Check(store->Rename(id,"existing.nsp").status==409 && fs::file_size(games/"existing.nsp")==8,"collision never overwrites existing file");
    Check(store->Rename(id,"renamed.nsp").status==204,"rename preserves received data");
    Check(store->Ready(id).status==204,"retry validation after rename");
    std::promise<void> validating;
    auto began=validating.get_future();
    std::string canceled;
    std::jthread validator{[&]{canceled=store->Finalize([&](const auto&,std::stop_token stop){
        validating.set_value();while(!stop.stop_requested())std::this_thread::yield();return std::string{"interrupted"};
    });}};
    Check(began.wait_for(std::chrono::seconds{2})==std::future_status::ready,"validation begins");
    store->Cancel();validator.join();
    Check(canceled=="interrupted" && !fs::exists(games/"renamed.nsp"),"cancel validation preserves partial");
    store->Resume();store->SetAvailable(true);
    Check(store->Ready(id).status==204,"retry canceled validation");
    const auto verify=[&](const fs::path& path,std::stop_token stop) {
        std::ifstream input{path,std::ios::binary};
        std::vector<char> bytes(pattern.size());
        std::uint64_t read{};
        while (input) {
            if(stop.stop_requested())return std::string{"interrupted"};
            input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
            const auto count=static_cast<size_t>(input.gcount());
            Check(std::equal(bytes.begin(),bytes.begin()+count,pattern.begin()),"streamed bytes identical");
            read+=count;
        }
        Check(read==fs::file_size(path),"verify every published byte");
        return std::string{};
    };
    Check(store->Finalize(verify).empty() && fs::file_size(games/"renamed.nsp")==32ULL << 20,"publish by rename");
    Check(!fs::exists(staging/"payload.part") && !store->Snapshot().busy,"no second full copy");
    store.reset();
    // Remove the completion record: emulate death just after rename, before that checkpoint.
    for(const auto* checkpoint:{"checkpoint0.json","checkpoint1.json"}){
        const auto path=staging/checkpoint;std::ifstream input{path};
        if(input){const auto record=Json::parse(input);input.close();if(record.at("phase")=="complete")fs::remove(path);}
    }
    store=std::make_unique<NspUpload>(games);
    Check(store->Snapshot().phase=="complete","recover publication by stable FileId");
    store->SetAvailable(true);
    Check(store->Create("renamed.nsp",fingerprint,12).status==409,"create destination conflict");
    Check(store->Discard(id).status==204 && fs::exists(games/"renamed.nsp"),"discard journal preserves installed game");
    // Corrupt the newest checkpoint and recover the older independent generation.
    Check(store->Create("fallback.nsp",fingerprint,32ULL << 20).status==201,"create fallback fixture");
    id=store->Snapshot().id;
    Check(store->Append(id,0,16ULL << 20,producer(16ULL << 20)).status==204,"checkpoint fixture");
    store.reset();
    { std::ofstream broken{staging/"checkpoint0.json",std::ios::trunc}; broken<<"{"; }
    store=std::make_unique<NspUpload>(games);
    Check(store->Snapshot().offset==0 && fs::file_size(staging/"payload.part")==0,"broken latest checkpoint falls back safely");
    Check(store->Discard(id).status==204,"discard fallback fixture");
    const auto generic_games=fixture/"generic-games";
    const auto generic_staging=fixture/"generic-staging";
    auto generic=std::make_unique<NspUpload>(generic_games,false,generic_staging,false,false,true);
    generic->SetAvailable(true);
    Check(generic->Create("payload.bin",fingerprint,pattern.size()).status==201,"portal mode accepts arbitrary extensions");
    const auto generic_id=generic->Snapshot().id;
    const auto half=pattern.size()/2;
    Check(generic->Append(generic_id,0,half,[&](const auto& sink){return sink(pattern.data(),half);}).status==204,
        "generic game file confirms first chunk");
    generic.reset();
    generic=std::make_unique<NspUpload>(generic_games,false,generic_staging,false,false,true);
    generic->SetAvailable(true);
    Check(generic->Snapshot().offset==half,"generic extension survives restart recovery");
    Check(generic->Append(generic_id,half,pattern.size()-half,[&](const auto& sink){
        return sink(pattern.data()+half,pattern.size()-half);
    }).status==204,"generic game file resumes at confirmed offset");
    Check(generic->Rename(generic_id,"published.bundle").status==204,"generic extension survives rename");
    Check(generic->Ready(generic_id).status==204,"generic file enters publish phase without format validation");
    Check(generic->Finalize().empty() && fs::file_size(generic_games/"published.bundle")==pattern.size(),
        "generic file publishes without NSP content validator");
    {
        std::ifstream input{generic_games/"published.bundle",std::ios::binary};
        std::vector<char> actual(pattern.size());
        input.read(actual.data(),static_cast<std::streamsize>(actual.size()));
        Check(input.gcount()==static_cast<std::streamsize>(actual.size()) && actual==pattern,
            "generic upload preserves every byte");
    }
    // A sparse prefix exercises real 64-bit file positioning without a heavy 5 GiB test by default.
    // --large below additionally transfers and compares all five GiB through TCP.
    constexpr std::uint64_t high_offset=(4ULL << 30)+(1ULL << 20);
    store->SetAvailable(true);
    Check(store->Create("large-offset.nsp",fingerprint,high_offset+pattern.size()).status==201,"64-bit fixture");
    id=store->Snapshot().id;store.reset();
    const auto sparse=CreateFileW((staging/"payload.part").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(sparse!=INVALID_HANDLE_VALUE,"open sparse fixture");
    DWORD returned{};
    Check(DeviceIoControl(sparse,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)!=0,"sparse test volume");
    LARGE_INTEGER high{};high.QuadPart=static_cast<LONGLONG>(high_offset);
    Check(SetFilePointerEx(sparse,high,nullptr,FILE_BEGIN) && SetEndOfFile(sparse),"sparse prefix above 4GiB");
    CloseHandle(sparse);
    { std::ifstream input{staging/"checkpoint1.json"}; auto record=Json::parse(input);input.close();
      record["offset"]=high_offset;std::ofstream output{staging/"checkpoint1.json",std::ios::trunc};output<<record.dump(); }
    store=std::make_unique<NspUpload>(games);store->SetAvailable(true);
    Check(store->Snapshot().offset==high_offset,"recover uint64 offset");
    Check(store->Append(id,high_offset,pattern.size(),producer(pattern.size())).status==204,"write above 4GiB");
    Check(store->Ready(id).status==204,"large offset complete");
    Check(store->Finalize([&](const auto& path,auto){
        std::ifstream input{path,std::ios::binary};input.seekg(static_cast<std::streamoff>(high_offset));
        std::vector<char> tail(pattern.size());input.read(tail.data(),static_cast<std::streamsize>(tail.size()));
        Check(tail==pattern,"large offset tail bytes");return std::string{};
    }).empty(),"publish sparse >4GiB fixture");
    Check(store->Discard(id).status==204,"clear large offset journal");
    store.reset();
    // HTTP-level creation, HEAD, streaming PATCH, offsets, cancellation, finish/validation.
    Jadyx::NetworkPortal portal{fixture/"remote/games"};
    Check(portal.Start("127.0.0.1",static_cast<unsigned short>(port)),"upload listener");
    portal.SetUploadsAvailable(true);
    httplib::Client client("127.0.0.1",port);client.set_keep_alive(true);client.set_read_timeout(60);
    const auto origin="http://127.0.0.1:"+std::to_string(port);
    const auto pairing=client.Post("/api/v1/pair",{{"Origin",origin}},Json{{"code",portal.Status().code}}.dump(),"application/json");
    Check(Status(pairing)==200,"upload test pairing");
    const auto token=Json::parse(pairing->body).at("session").get<std::string>();
    httplib::Headers headers{{"Authorization","Bearer "+token},{"Tus-Resumable","1.0.0"},{"Origin",origin}};
    const auto total=large ? (5ULL << 30) : (32ULL << 20);
    auto creation=headers;
    creation.emplace("Upload-Length",std::to_string(total));
    creation.emplace("Upload-Metadata","filename "+httplib::detail::base64_encode("transport.nsp")+",fingerprint "+httplib::detail::base64_encode(fingerprint));
    const auto created=client.Post("/api/v1/uploads",creation);
    Check(Status(created)==201,"tus HTTP creation");
    const auto url=created->get_header_value("Location");
    Check(Status(client.Head(url,headers))==200,"tus HEAD");
    // A rejected PATCH must close immediately even if its declared body never arrives.
    const auto rejected=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in rejected_address{};rejected_address.sin_family=AF_INET;rejected_address.sin_port=htons(static_cast<u_short>(port));
    InetPtonA(AF_INET,"127.0.0.1",&rejected_address.sin_addr);
    Check(::connect(rejected,reinterpret_cast<sockaddr*>(&rejected_address),sizeof(rejected_address))==0,"early rejection peer");
    const DWORD receive_timeout=2000;
    ::setsockopt(rejected,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&receive_timeout),sizeof(receive_timeout));
    const auto request="PATCH "+url+" HTTP/1.1\r\nHost: 127.0.0.1:"+std::to_string(port)+
        "\r\nAuthorization: Bearer "+token+"\r\nTus-Resumable: 1.0.0\r\nUpload-Offset: 1\r\nContent-Type: application/offset+octet-stream\r\nContent-Length: 16777216\r\n\r\n";
    ::send(rejected,request.data(),static_cast<int>(request.size()),0);
    std::string reply;std::array<char,2048> reply_buffer{};int read_reply{};
    do{read_reply=::recv(rejected,reply_buffer.data(),static_cast<int>(reply_buffer.size()),0);if(read_reply>0)reply.append(reply_buffer.data(),static_cast<size_t>(read_reply));}while(read_reply>0 && reply.size()<4096);
    Check(read_reply==0 && reply.find(" 409 ")!=std::string::npos,"early rejection closes without draining an absent body");
    ::closesocket(rejected);
    const auto at=std::chrono::steady_clock::now();
    for(std::uint64_t offset=0;offset<total;){
        const auto count=(std::min)(total-offset,16ULL << 20);
        auto patch=headers;patch.emplace("Upload-Offset",std::to_string(offset));
        auto bad_offset=patch;
        if(offset==0){bad_offset.erase("Upload-Offset");bad_offset.emplace("Upload-Offset","1");
            Check(Status(client.Patch(url,bad_offset,"x","application/offset+octet-stream"))==409,"HTTP wrong offset");}
        const auto result=client.Patch(url,patch,static_cast<size_t>(count),
            [&](size_t position,size_t length,httplib::DataSink& sink){
                while(length){const auto n=(std::min)(length,pattern.size()-(position%pattern.size()));
                    if(!sink.write(pattern.data()+(position%pattern.size()),n))return false;
                    position+=n;length-=n;}
                return true;
            },"application/offset+octet-stream");
        Check(Status(result)==204,"HTTP streaming patch");offset+=count;
        Check(result->get_header_value("Upload-Offset")==std::to_string(offset),"HTTP confirmed offset");
    }
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-at).count();
    const auto stats=client.Get("/api/v1/status",headers);
    Check(Status(stats)==200,"performance status");
    const auto profile=Json::parse(stats->body).at("transfer").at("performance");
    Check(profile.at("confirmed_offset")==total && profile.at("bytes")==16ULL << 20,"performance JSON matches acknowledged block");
    std::printf("Last block profile: %s\n",profile.dump().c_str());
    Check(Status(client.Post(url+"/finish",headers))==204 && portal.Status().transfer.phase=="ready","HTTP finish waits for coordinator");
    Check(portal.FinalizeGame(verify).empty(),"coordinator validates then publishes HTTP upload");
    std::printf("TCP loopback %llu MiB, %.2f s, %.2f MiB/s (desktop synthetic, not Xbox/LAN certification)\n",
        static_cast<unsigned long long>(total >> 20),seconds,(total/1048576.0)/seconds);
    portal.Stop();
    Check(fs::file_size(fixture/"remote/games/transport.nsp")==total,"64-bit final size");
    // Recursive cleanup is confined to the newly created fixture under the known test directory.
    Check(fixture.parent_path()==parent && fixture.filename().string().starts_with("fixture-"),"cleanup confined to fixture");
    fs::remove_all(fixture);
    std::puts("PASS: upload disconnect/restart/checkpoints/collision/publish/TCP byte equality; real NSP validation and Series performance pending");
}
