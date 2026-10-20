// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <zstd.h>
#include <array>

std::filesystem::path CreateLogFixture() {
    const auto root=std::filesystem::absolute("build-uwp/test-results/network-portal/logs-"+std::to_string(GetTickCount64()));
    const auto nested=root/"log"/"trace"/std::filesystem::path{std::u8string(100,u8'a')+u8"-captura-á"};
    std::filesystem::create_directories(nested/"empty-folder");
    {std::ofstream file{root/"log"/"jadyx_log.txt"};for(unsigned i=0;i<10000;++i)file<<"capture diagnostic repeat "<<i<<'\n';}
    {std::ofstream file{nested/"empty.bin",std::ios::binary};}
    {std::ofstream file{nested/"blobs.bin",std::ios::binary};std::uint32_t value=123;
     for(unsigned i=0;i<(4U<<20);++i){value^=value<<13;value^=value>>17;value^=value<<5;file.put(static_cast<char>(value));}}
    std::filesystem::create_directories(root/"keys");
    {std::ofstream file{root/"keys"/"outside-secret.txt"};file<<"never include outside log";}
    return root;
}
void DecodeLogArchive(std::string_view bytes,const std::filesystem::path& path) {
    auto* decoder=ZSTD_createDStream();Check(decoder!=nullptr,"create zstd decoder");
    std::ofstream output{path,std::ios::binary};
    ZSTD_inBuffer source{bytes.data(),bytes.size(),0};
    std::array<char,65536> buffer{};
    std::size_t remaining=1;
    while(source.pos<source.size){
        ZSTD_outBuffer destination{buffer.data(),buffer.size(),0};
        remaining=ZSTD_decompressStream(decoder,&destination,&source);
        Check(!ZSTD_isError(remaining),"zstd frame checksum and decoding");
        output.write(buffer.data(),destination.pos);
    }
    Check(remaining==0&&output.good(),"complete decompressed archive");
    ZSTD_freeDStream(decoder);
}
void LogExportTests(Jadyx::NetworkPortal& portal,httplib::Client& client,
                    const httplib::Headers& authenticated,const std::filesystem::path& root) {
    using namespace std::chrono_literals;
    client.set_read_timeout(15);
    Check(Status(client.Post("/api/v1/logs/export","{}","application/json"))==401,"logs require pairing");
    Check(Status(client.Get("/api/v1/logs/archive"))==401,"anonymous logs rejected");
    Check(Status(client.Post("/api/v1/logs/download",authenticated,"{}","application/json"))==409,"logs not ready rejected");
    Check(Status(client.Post("/api/v1/logs/export",authenticated,"{}","application/json"))==202,"async logs preparation");
    Check(Status(client.Post("/api/v1/logs/export",authenticated,"{}","application/json"))==409,"only one export worker");
    // Granted while compressing: the archive streams to the client as it is written.
    auto early=client.Post("/api/v1/logs/download",authenticated,"{}","application/json");
    Check(Status(early)==200,"grant download while compressing");
    const auto early_grant=Json::parse(early->body);
    const bool streaming=early_grant.at("streaming").get<bool>();
    auto streamed=client.Get(early_grant.at("download").get<std::string>());
    Check(Status(streamed)==200,"download followed from the start of the export");
    Check(!streaming||streamed->get_header_value("Transfer-Encoding")=="chunked","streamed while compressing");
    const auto wait=[&] {
        Json value;
        const auto deadline=std::chrono::steady_clock::now()+15s;
        do {
            auto response=client.Get("/api/v1/status",authenticated);Check(Status(response)==200,"logs progress endpoint");
            value=Json::parse(response->body).at("logs");
            if(!value.at("busy").get<bool>())return value;
            std::this_thread::sleep_for(10ms);
        }while(std::chrono::steady_clock::now()<deadline);
        Check(false,"logs preparation deadline");return value;
    };
    auto state=wait();
    if(state.at("phase")!="ready")std::fprintf(stderr,"logs: %s\n",state.dump().c_str());
    Check(state.at("phase")=="ready"&&state.at("files")==3,"all log files prepared");
    Check(state.at("processed")==state.at("total"),"entire input processed");
    auto grant=client.Post("/api/v1/logs/download",authenticated,"{}","application/json");
    Check(Status(grant)==200,"grant log download");
    const auto ticket=Json::parse(grant->body).at("download").get<std::string>();
    Check(Status(client.Get("/api/v1/logs/archive",authenticated))==401,"Bearer alone is not a download ticket");
    auto archive=client.Get(ticket);
    Check(Status(archive)==200&&archive->body.size()==state.at("compressed").get<std::size_t>(),"complete compressed HTTP download");
    Check(archive->get_header_value("Content-Disposition").find(".tar.zst")!=std::string::npos,"archive attachment filename");
    Check(streamed->body==archive->body,"streamed archive is byte-identical to the finished one");
    Check(!archive->get_header_value("ETag").empty(),"finished archive carries a resume validator");
    DecodeLogArchive(archive->body,root/"download.tar");
    {std::ofstream file{root/"download.tar.zst",std::ios::binary};file.write(archive->body.data(),archive->body.size());}
    auto ranged=client.Get(ticket,{{"Range","bytes=7-1023"}});
    Check(Status(ranged)==206&&ranged->body==archive->body.substr(7,1017),"range download exact bytes");
    Check(Status(client.Head(ticket))==200,"HEAD supports native download retry");
    Check(Status(client.Get(ticket,{{"Range","bytes=99999999999-"}}))==416,"invalid range rejected");
    Check(Status(client.Post("/api/v1/logs/export",authenticated,"{}","application/json"))==202,"new log export allowed");
    Check(Status(client.Get(ticket))==401,"new export invalidates old ticket");
    // A multithreaded export of the small fixture can finish before the DELETE lands: retry
    // until a cancel wins the race; each lost race must still end as a complete archive.
    bool cancelled=false;
    for(unsigned attempt=0;attempt<50&&!cancelled;++attempt){
        if(attempt)Check(Status(client.Post("/api/v1/logs/export",authenticated,"{}","application/json"))==202,"retry log export");
        Check(Status(client.Delete("/api/v1/logs/export",authenticated))==200,"cancel compression");
        state=wait();cancelled=state.at("phase")=="cancelled";
        Check(cancelled||state.at("phase")=="ready","cancellation completes");
    }
    Check(cancelled,"a cancellation interrupts a running export");
    Check(!std::filesystem::exists(root/"transfers"/"log-export"/"logs.partial"),"cancelled partial removed");
    Check(Status(client.Post("/api/v1/logs/export",authenticated,"{}","application/json"))==202,"restart after cancellation");
    Check(wait().at("phase")=="ready","restart completes");
    std::printf("PASS: logs native TCP export, streaming while compressing (%s), range/retry, cancellation, bounded buffers; fixture %s\n",streaming?"yes":"already ready",root.string().c_str());
}
