// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <filesystem>
#include <string>

// Included by the native TCP harness. The library view and the jobs it runs are synthetic: this
// covers the portal's routes, admission and job lifecycle, not the console's content store.
void LibraryTests(int port) {
    namespace fs=std::filesystem;
    const auto parent=fs::absolute("build-uwp/test-results/network-portal");
    const auto fixture=parent/("library-"+std::to_string(GetCurrentProcessId()));
    Check(fixture.parent_path()==parent && !fs::exists(fixture),"isolated library fixture path");
    fs::create_directories(fixture/"games");
    constexpr std::uint64_t application=0x0100000000010000ULL;
    const std::string game="0100000000010000",item(64,'d');
    std::atomic<std::uint64_t> covers{};
    {
    Jadyx::NetworkPortal portal{fixture/"games"};
    portal.SetLibrary({
        .games=[] { return std::pair{std::uint64_t{7},std::vector<Jadyx::PortalGame>{{.program_id=application,.name="Game",.version="1.0.0",.removable=true}}}; },
        .cover=[&](std::uint64_t id) { ++covers; return id==application ? std::optional<std::string>{"jpeg"} : std::nullopt; },
        .contents=[&](std::uint64_t) { return std::vector<Jadyx::PortalContent>{{.id=item,.kind="update",.name="Update",.version="1.0.1",.bytes=5,.internal=true}}; },
        .progress=[] { return std::pair{std::uint64_t{3},std::uint64_t{9}}; },
    });
    Check(portal.Start("127.0.0.1",static_cast<unsigned short>(port)),"library listener");
    portal.SetUploadsAvailable(true);
    httplib::Client client("127.0.0.1",port);client.set_read_timeout(10);
    const auto origin="http://127.0.0.1:"+std::to_string(port);
    const auto pairing=client.Post("/api/v1/pair",{{"Origin",origin}},Json{{"code",portal.Status().code}}.dump(),"application/json");
    Check(Status(pairing)==200,"library test pairing");
    const auto token=Json::parse(pairing->body).at("session").get<std::string>();
    const httplib::Headers headers{{"Authorization","Bearer "+token},{"Tus-Resumable","1.0.0"},{"Origin",origin}};

    // Read-only views: ids are parsed by the routes and never reach the view as text.
    Check(Status(client.Get("/api/v1/library"))==401 && Status(client.Delete("/api/v1/library/"+game))==401,"library requires the session");
    const auto library=client.Get("/api/v1/library",headers);
    Check(Status(library)==200,"library listing");
    const auto listed=Json::parse(library->body);
    Check(listed.at("revision")==7 && listed.at("games").size()==1 && listed.at("games")[0].at("id")==game &&
          listed.at("games")[0].at("removable")==true,"library JSON carries id, revision and removable");
    const auto cover=client.Get("/api/v1/library/"+game+"/cover",headers);
    Check(Status(cover)==200 && cover->body=="jpeg" && cover->get_header_value("Content-Type")=="image/jpeg","cover bytes");
    Check(Status(client.Get("/api/v1/library/0100000000020000/cover",headers))==404,"unknown cover");
    Check(Status(client.Get("/api/v1/library/0100000000010000X/cover",headers))==404 && covers==2,"malformed id never reaches the view");
    const auto contents=client.Get("/api/v1/library/"+game+"/content",headers);
    Check(Status(contents)==200 && Json::parse(contents->body).at("contents")[0].at("id")==item,"content listing");
    Check(Json::parse(client.Get("/api/v1/status",headers)->body).at("install")==Json{{"done",3},{"total",9}},"install progress in status");

    // A content upload names its kind and game; both are checked before anything is created.
    const auto create=[&](const std::string& kind,const std::string& target,
                          const std::string& filename="patch.nsp",const std::string& length="4") {
        auto request=headers;
        request.emplace("Upload-Length",length);
        request.emplace("Upload-Metadata","filename "+httplib::detail::base64_encode(filename)+",fingerprint "+
            httplib::detail::base64_encode(std::string(64,'a'))+",kind "+httplib::detail::base64_encode(kind)+
            ",application "+httplib::detail::base64_encode(target));
        return client.Post("/api/v1/content/uploads",request);
    };
    Check(Status(create("firmware",game))==400,"content kind is update or dlc");
    Check(Status(create("update","0100000000020000"))==404,"content targets a known game");
    const auto created=create("update",game);
    Check(Status(created)==201,"content upload created");
    const auto url=created->get_header_value("Location");
    Check(url.starts_with("/api/v1/content/uploads/"),"content upload location");
    Check(Status(client.Delete("/api/v1/library/"+game,headers))==409,"no deletion while an upload is open");
    auto patch=headers;patch.emplace("Upload-Offset","0");
    Check(Status(client.Patch(url,patch,"data","application/offset+octet-stream"))==204,"content bytes");
    Check(Status(client.Post(url+"/finish",headers))==204,"content ready");
    std::string seen;
    Check(portal.FinalizeContent([&](const fs::path& path,const std::string& tag,std::stop_token) {
        seen=tag; return fs::file_size(path)==4 ? std::string{} : std::string{"disk"};
    }).empty() && seen=="update:"+game,"the installer receives the validated tag");
    Check(portal.Status().content.phase=="complete","content consumed");
    Check(Status(create("save",game,"save.zip"))==201,"save ZIP upload created");
    const auto save_url=portal.Status().content.id;
    Check(Status(client.Patch("/api/v1/content/uploads/"+save_url,patch,"data","application/offset+octet-stream"))==204,
          "save ZIP bytes");
    Check(Status(client.Post("/api/v1/content/uploads/"+save_url+"/finish",headers))==204,"save ZIP ready");
    Check(portal.FinalizeContent([&](const fs::path& path,const std::string& tag,std::stop_token) {
        seen=tag; return fs::file_size(path)==4 ? std::string{} : std::string{"disk"};
    }).empty() && seen=="save:"+game,"the save importer receives the selected game tag");
    Check(Status(create("save",game,"save.nsp"))==400,"save upload requires ZIP");
    Check(Status(create("save",game,"save.zip","4294967297"))==400,"save ZIP size is bounded");

    // Deletions queue one job; uploads stay closed from the request until the job has run.
    Check(Status(client.Delete("/api/v1/library/0100000000020000",headers))==404,"unknown game cannot be deleted");
    Check(Status(client.Delete("/api/v1/library/"+game+"/content/abc",headers))==404,"malformed content id");
    const auto queued=client.Delete("/api/v1/library/"+game,headers);
    Check(Status(queued)==202,"game deletion queued");
    const auto removal=Json::parse(queued->body).at("removal");
    Check(removal.at("phase")=="pending" && removal.at("game")==game && removal.at("content")=="","pending game deletion");
    Check(Status(client.Delete("/api/v1/library/"+game+"/content/"+item,headers))==409,"one deletion at a time");
    Check(Status(create("dlc",game))==409,"uploads closed during a deletion");
    Check(!portal.BlockForLocalOperation(),"local operations wait for the deletion");
    Jadyx::PortalRemoval ran;
    Check(portal.RunRemoval([&](const Jadyx::PortalRemoval& job) { ran=job; return std::string{}; }).empty(),"deletion runs");
    Check(ran.application==application && ran.content.empty() && ran.phase=="running" && ran.serial==removal.at("serial"),"runner receives the queued job");
    auto status=Json::parse(client.Get("/api/v1/status",headers)->body);
    Check(status.at("removal").at("phase")=="complete" && status.at("content").at("available")==true,"deletion completes and reopens uploads");
    Check(portal.RunRemoval([](const auto&) { return std::string{}; })=="unavailable","nothing runs twice");
    const auto second=client.Delete("/api/v1/library/"+game+"/content/"+item,headers);
    Check(Status(second)==202 && Json::parse(second->body).at("removal").at("content")==item,"content deletion queued");
    Check(portal.RunRemoval([](const auto&) { return std::string{"content"}; })=="content","failed deletion reports its error");
    status=Json::parse(client.Get("/api/v1/status",headers)->body);
    Check(status.at("removal").at("phase")=="failed" && status.at("removal").at("error")=="content" &&
          status.at("removal").at("serial")==removal.at("serial").get<std::uint64_t>()+1,"failure is visible to the page");
    portal.Stop();
    }
    std::error_code ignored;fs::remove_all(fixture,ignored);
    std::puts("PASS: library listing/cover/content views, update/save upload admission and tags, queued deletions and upload exclusion (synthetic view)");
}
