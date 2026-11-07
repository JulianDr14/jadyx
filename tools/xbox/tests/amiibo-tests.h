// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>

// Included by the native TCP harness. The store is synthetic: this covers the routes, their session
// and what they answer. amiibo-store.cpp covers the store itself.
void AmiiboTests(int port) {
    namespace fs=std::filesystem;
    const auto parent=fs::absolute("build-uwp/test-results/network-portal");
    const auto fixture=parent/("amiibo-"+std::to_string(GetCurrentProcessId()));
    Check(fixture.parent_path()==parent && !fs::exists(fixture),"isolated amiibo fixture path");
    fs::create_directories(fixture/"games");
    const std::string secret(160,'k');
    std::map<std::string,std::string> files;
    std::string key;
    bool last_replace{};
    {
    Jadyx::NetworkPortal portal{fixture/"games"};
    portal.SetAmiibo({
        .list=[&] { std::vector<Jadyx::PortalAmiiboFile> list; for (const auto& [name,bytes]:files) list.push_back({name,bytes.size()}); return list; },
        .has_key=[&] { return !key.empty(); },
        .add=[&](const std::string& name,const std::string& bytes,bool replace) -> std::string {
            last_replace=replace;
            if (name.empty()) return "name";
            if (bytes.size()!=540) return "not_amiibo";
            if (files.contains(name) && !replace) return "exists";
            files[name]=bytes; return {};
        },
        .remove=[&](const std::string& name) -> std::string { return files.erase(name) ? "" : "missing"; },
        .set_key=[&](const std::string& bytes) -> std::string { if (bytes.size()!=160) return "not_key"; key=bytes; return {}; },
        .remove_key=[&]() -> std::string { key.clear(); return {}; },
    });
    Check(portal.Start("127.0.0.1",static_cast<unsigned short>(port)),"amiibo listener");
    httplib::Client client("127.0.0.1",port);client.set_read_timeout(10);
    const auto origin="http://127.0.0.1:"+std::to_string(port);
    const auto pairing=client.Post("/api/v1/pair",{{"Origin",origin}},Json{{"code",portal.Status().code}}.dump(),"application/json");
    Check(Status(pairing)==200,"amiibo test pairing");
    const httplib::Headers headers{{"Authorization","Bearer "+Json::parse(pairing->body).at("session").get<std::string>()},{"Origin",origin}};
    const auto binary="application/octet-stream";
    const std::string tag(540,'t');

    Check(Status(client.Get("/api/v1/amiibo"))==401 && Status(client.Post("/api/v1/amiibo/files?name=a.bin",tag,binary))==401 &&
          Status(client.Delete("/api/v1/amiibo/files?name=a.bin"))==401 && Status(client.Post("/api/v1/amiibo/key",secret,binary))==401 &&
          Status(client.Delete("/api/v1/amiibo/key"))==401 && files.empty(),"amiibo routes require the session");
    Check(Json::parse(client.Get("/api/v1/status",headers)->body).at("amiibo")==true,"status offers amiibo");

    // A name with spaces and accents arrives decoded.
    Check(Status(client.Post("/api/v1/amiibo/files?name=figura%20%C3%A1.bin",headers,tag,binary))==201 && files.contains("figura \xC3\xA1.bin") && !last_replace,"file uploaded");
    const auto word=[](const auto& result) { return Json::parse(result->body).at("error").get<std::string>(); };
    const auto taken=client.Post("/api/v1/amiibo/files?name=figura%20%C3%A1.bin",headers,tag,binary);
    Check(Status(taken)==409 && word(taken)=="exists","a taken name asks first");
    Check(Status(client.Post("/api/v1/amiibo/files?name=figura%20%C3%A1.bin&replace=1",headers,tag,binary))==201 && last_replace,"replace is explicit");
    const auto wrong=client.Post("/api/v1/amiibo/files?name=b.bin",headers,std::string(16,'x'),binary);
    Check(Status(wrong)==400 && word(wrong)=="not_amiibo" && files.size()==1,"not an amiibo");
    const auto nameless=client.Post("/api/v1/amiibo/files",headers,tag,binary);
    Check(Status(nameless)==400 && word(nameless)=="name","name required");
    Check(Status(client.Post("/api/v1/amiibo/files?name=c.bin",headers,std::string(4097,'x'),binary))==413 && files.size()==1,"body limit");

    const auto missing=client.Post("/api/v1/amiibo/key",headers,std::string(10,'x'),binary);
    Check(Status(missing)==400 && word(missing)=="not_key" && key.empty(),"not a key");
    Check(Status(client.Post("/api/v1/amiibo/key",headers,secret,binary))==204 && key==secret,"key installed");

    // The listing says the key is there and never carries it, on any route.
    const auto listed=client.Get("/api/v1/amiibo",headers);
    Check(Status(listed)==200 && listed->body.find(secret)==std::string::npos,"listing without the key");
    const auto list=Json::parse(listed->body);
    Check(list.at("key")==true && list.at("files").size()==1 && list.at("files")[0].at("bytes")==540 && list.at("files")[0].size()==2,"listing: name, size and key flag only");
    const auto read_key=client.Get("/api/v1/amiibo/key",headers);
    Check(!read_key || read_key->body.find(secret)==std::string::npos,"the key can't be read back");
    Check(Status(read_key)!=200,"no route reads the key");

    Check(Status(client.Delete("/api/v1/amiibo/files?name=nope.bin",headers))==404,"unknown file");
    Check(Status(client.Delete("/api/v1/amiibo/files?name=figura%20%C3%A1.bin",headers))==204 && files.empty(),"file removed");
    Check(Status(client.Delete("/api/v1/amiibo/key",headers))==204 && key.empty(),"key removed");
    portal.Stop();
    }
    // Without a store the routes say so.
    {
    Jadyx::NetworkPortal portal{fixture/"games"};
    Check(portal.Start("127.0.0.1",static_cast<unsigned short>(port)),"plain listener");
    httplib::Client client("127.0.0.1",port);client.set_read_timeout(10);
    const auto origin="http://127.0.0.1:"+std::to_string(port);
    const auto pairing=client.Post("/api/v1/pair",{{"Origin",origin}},Json{{"code",portal.Status().code}}.dump(),"application/json");
    const httplib::Headers headers{{"Authorization","Bearer "+Json::parse(pairing->body).at("session").get<std::string>()},{"Origin",origin}};
    Check(Status(client.Get("/api/v1/amiibo",headers))==503 && Status(client.Post("/api/v1/amiibo/key",headers,secret,"application/octet-stream"))==503,"no store: 503");
    Check(Json::parse(client.Get("/api/v1/status",headers)->body).at("amiibo")==false,"status does not offer amiibo");
    portal.Stop();
    }
    fs::remove_all(fixture);
    std::puts("PASS: amiibo routes: session required, per-file answers, explicit replace, body limit, key never read back, 503 without a store (synthetic store)");
}
