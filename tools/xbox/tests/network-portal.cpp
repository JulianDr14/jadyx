// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/httplib.h"
#include "jadyx/storage/network_portal.h"
#include "jadyx/storage/network_portal_page.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <future>
#include <fstream>

namespace {
using Json = nlohmann::json;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int Status(const httplib::Result& result) { return result ? result->status : 0; }
} // namespace

#include "log-export-tests.h"
#include "nsp-upload-tests.h"
#include "system-upload-tests.h"
#include "library-tests.h"

int main(int argc,char** argv) {
    try {
    using namespace std::chrono_literals;
    const auto reservation = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(reservation != INVALID_SOCKET, "port reservation socket");
    sockaddr_in reserve{}; reserve.sin_family = AF_INET;
    InetPtonA(AF_INET, "127.0.0.1", &reserve.sin_addr);
    Check(::bind(reservation, reinterpret_cast<sockaddr*>(&reserve), sizeof(reserve)) == 0, "reserve port");
    int reserve_size = sizeof(reserve);
    Check(::getsockname(reservation, reinterpret_cast<sockaddr*>(&reserve), &reserve_size) == 0, "read port");
    const auto port = ntohs(reserve.sin_port);
    ::closesocket(reservation);
    Check(port > 0 && port <= 65535, "reserve a test port");
    const auto log_root=CreateLogFixture();
    Jadyx::NetworkPortal portal;
    portal.SetLogDirectory(log_root / "log");
    Check(!portal.Status().running, "off by default");
    Check(!portal.Start("0.0.0.0", static_cast<unsigned short>(port)), "reject wildcard binding");
    Check(!portal.Start("8.8.8.8", static_cast<unsigned short>(port)), "reject public binding");
    Check(portal.Start("127.0.0.1", static_cast<unsigned short>(port)), "start loopback harness");
    const auto initial = portal.Status();
    Check(initial.port == port && initial.error == Jadyx::PortalError::None, "status carries the bound port");
    Jadyx::NetworkPortal occupied;
    Check(!occupied.Start("127.0.0.1", static_cast<unsigned short>(port)) &&
          occupied.Status().error == Jadyx::PortalError::PortInUse && occupied.Status().url.empty() &&
          occupied.Status().port == port && occupied.Status().os_error == WSAEADDRINUSE,
          "occupied port fails as port in use without advertising URL");
    // A spare port takes over; the next free one above the taken port.
    Check(occupied.Start("127.0.0.1", static_cast<unsigned short>(port), 8) &&
          occupied.Status().port > port && occupied.Status().port <= port + 8 &&
          occupied.Status().url == "http://127.0.0.1:" + std::to_string(occupied.Status().port) &&
          occupied.Status().error == Jadyx::PortalError::None,
          "taken port moves on to a spare one");
    occupied.Stop();
    // A LAN address the machine does not hold reads as lost, not as a port problem, and stops
    // without trying the spare ports.
    Check(!Jadyx::PortalAddressOwned("10.254.253.252"), "fixture address is foreign");
    Check(!occupied.Start("10.254.253.252", 18090, 4) &&
          occupied.Status().error == Jadyx::PortalError::AddressLost &&
          occupied.Status().os_error == WSAEADDRNOTAVAIL && occupied.Status().port == 18090,
          "foreign address reads as lost");
    Check(Jadyx::PortalAddressOwned("127.0.0.1"), "loopback owned");
    for (const auto& address : Jadyx::PortalAddresses()) {
        Check(Jadyx::PortalAddressOwned(address.ipv4) && address.ipv4.rfind("127.", 0) != 0 &&
              address.name == (address.link == Jadyx::PortalLink::WiFi ? "Wi-Fi" : "Ethernet") &&
              address.link_local == (address.ipv4.rfind("169.254.", 0) == 0),
              "listed addresses are owned LAN addresses with a consistent link");
        std::printf("address: %s %s gateway=%d link_local=%d\n", address.name.c_str(), address.ipv4.c_str(),
                    address.gateway, address.link_local);
    }
    const auto report = Jadyx::PortalAdapterReport();
    Check(report.find("adapter '") != std::string::npos && report.find("ipv4 127.0.0.1") != std::string::npos,
          "adapter report lists every adapter");
    Check(initial.code.size() == 4 && initial.code.find_first_not_of("0123456789") == std::string::npos,
          "four random digits");
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(4);
    const auto origin = "http://127.0.0.1:" + std::to_string(port);
    httplib::Headers headers{{"Origin", origin}};
    const auto page = client.Get("/");
    if (Status(page) != 200) std::fprintf(stderr, "HTTP page status=%d transport=%d\n", Status(page), static_cast<int>(page.error()));
    // Four digits turn up in any page by chance; the page being the fixed one is what keeps the code out.
    Check(Status(page) == 200 && page->body == Jadyx::PortalWeb::Page, "page never contains pairing secret");
    Check(page->get_header_value("Cache-Control") == "no-store" &&
          !page->get_header_value("Content-Security-Policy").empty(), "page security headers");
    Check(Status(client.Get("/api/v1/status")) == 401, "anonymous status rejected");
    Check(Status(client.Get("/", {{"Host", "attacker.invalid"}})) == 403, "Host allowlist");
    Check(Status(client.Get("/", {{"Origin", "http://attacker.invalid"}})) == 403, "Origin allowlist");
    Check(Status(client.Post("/api/v1/pair", "{}", "application/json")) == 401, "pair requires Origin");
    Check(Status(client.Post("/api/v1/pair", headers, "{", "application/json")) == 401, "malformed JSON");
    Check(Status(client.Post("/api/v1/pair", headers, "[]", "application/json")) == 401, "JSON shape");
    Check(Status(client.Post("/api/v1/pair", headers, "{\"code\":0}", "application/json")) == 401, "JSON field type");
    const auto paired = client.Post("/api/v1/pair", headers, Json{{"code", initial.code}}.dump(), "application/json");
    Check(Status(paired) == 200, "correct code pairs");
    const auto token = Json::parse(paired->body).at("session").get<std::string>();
    Check(token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string::npos,
          "256 bit session");
    httplib::Headers authenticated{{"Authorization", "Bearer " + token}};
    auto status = client.Get("/api/v1/status", authenticated);
    Check(Status(status) == 200 && !Json::parse(status->body).at("uploads").get<bool>(),
          "authenticated connection gate, no upload endpoints");
    Check(status->body.find(token) == std::string::npos && !Json::parse(status->body).contains("code"),
          "status never returns secrets");
    LogExportTests(portal,client,authenticated,log_root);
    // Exercise the production update routes over real native TCP, not a mocked server.
    Check(Status(client.Post("/api/v1/update/download", "{}", "application/json"))==401,"anonymous update preparation rejected");
    Check(Status(client.Post("/api/v1/update/download", authenticated,"{}","application/json"))==409,"unverified update rejected");
    const auto update_root=std::filesystem::absolute("build-uwp/test-results/network-portal/update-"+std::to_string(GetTickCount64()));
    std::filesystem::create_directories(update_root);
    const auto update_path=update_root/"package.appx";
    const std::string update_bytes(8u<<20,'x');
    {std::ofstream output{update_path,std::ios::binary};output.write(update_bytes.data(),update_bytes.size());}
    std::mutex update_mutex;
    std::optional<Jadyx::Updates::UpdatePackage> update=Jadyx::Updates::UpdatePackage{update_path,"0.3.6.0",std::string(64,'a'),update_bytes.size()};
    portal.SetUpdatePackage([&] {std::lock_guard lock{update_mutex};return update;});
    const auto grant=[&] {
        auto result=client.Post("/api/v1/update/download",authenticated,"{}","application/json");
        Check(Status(result)==200,"grant verified update");
        const auto object=Json::parse(result->body);
        Check(object.at("filename")=="Jadyx_0.3.6.0_x64.appx","versioned update filename");
        return object.at("download").get<std::string>();
    };
    auto ticket=grant();
    auto update_response=client.Get(ticket);
    Check(Status(update_response)==200 && update_response->body==update_bytes,"stream exact verified update bytes");
    Check(update_response->get_header_value("Content-Disposition").find("0.3.6.0")!=std::string::npos,"download content disposition");
    Check(Status(client.Get(ticket))==401,"update ticket cannot replay");
    Check(Status(client.Get("/api/v1/update/package",authenticated))==401,"authenticated GET still requires a ticket");
    ticket=grant();
    {std::lock_guard lock{update_mutex};update->sha256=std::string(64,'b');}
    Check(Status(client.Get(ticket))==409,"ticket cannot deliver a replacement package");
    ticket=grant();const auto replaced_ticket=grant();
    Check(Status(client.Get(ticket))==401,"issuing another ticket revokes the previous one");
    Check(Status(client.Get(replaced_ticket))==200,"replacement ticket delivers current package");
    ticket=grant();
    std::atomic<bool> receiving{},continue_download{};
    auto read_update=std::async(std::launch::async,[&] {
        httplib::Client download("127.0.0.1",port);download.set_read_timeout(10);
        std::size_t received{};
        auto result=download.Get(ticket,[&](const char* data,std::size_t size) {
            Check(std::string_view{data,size}.find_first_not_of('x')==std::string_view::npos,"stream contents remain immutable");
            received+=size;receiving=true;
            while(!continue_download)std::this_thread::sleep_for(1ms);
            return true;
        });
        Check(Status(result)==200 && received==update_bytes.size(),"pinned streaming update completes");
    });
    const auto receive_deadline=std::chrono::steady_clock::now()+5s;
    while(!receiving){Check(std::chrono::steady_clock::now()<receive_deadline,"update stream started");std::this_thread::sleep_for(1ms);}
    Check(!DeleteFileW(update_path.c_str()) && GetLastError()==ERROR_SHARING_VIOLATION,"in-flight stream prevents package deletion");
    continue_download=true;read_update.get();
    std::puts("PASS: real TCP verified update/auth/ticket replay/package binding/reissue/pinned stream");
    Check(portal.Status().paired && portal.Status().code_expired, "code consumed after pairing");
    Check(Status(client.Post("/api/v1/pair", headers, Json{{"code", initial.code}}.dump(), "application/json")) == 401,
          "code cannot be replayed");
    Check(Status(client.Post("/api/v1/pair", headers, "{}", "application/json")) == 429,
          "global attempt limiter");
    Check(portal.RenewCode(), "renew local code");
    Check(Status(client.Post("/api/v1/pair", headers, "{}", "application/json")) == 429,
          "renewal cannot bypass rate limit");
    Check(Status(client.Get("/api/v1/status", authenticated)) == 200, "renewal preserves paired session");
    Check(Status(client.Post("/api/v1/uploads", authenticated, "{}", "application/json")) == 412,
          "upload protocol version required");
    const auto oversized=client.Post("/api/v1/pair", headers, std::string(2048, 'x'), "application/json");
    if(Status(oversized)!=413)std::fprintf(stderr,"oversized status=%d transport=%d\n",Status(oversized),static_cast<int>(oversized.error()));
    Check(Status(oversized) == 413,
          "bounded HTTP request body");
    if(argc>1 && std::string_view{argv[1]}=="--update-expiry") {
        ticket=grant(); const auto expiry=std::chrono::steady_clock::now()+121s;
        while(std::chrono::steady_clock::now()<expiry)std::this_thread::sleep_for(100ms);
        Check(Status(client.Get(ticket))==401,"expired update ticket rejected");
    }
    portal.Stop();
    Check(!portal.Status().running && portal.Status().code.empty(), "stop clears secrets");
    Check(!portal.RenewCode(), "cannot renew stopped server");
    Check(!client.Get("/"), "socket closed after Stop");
    Check(portal.Start("127.0.0.1", static_cast<unsigned short>(port)), "restart same port");
    Check(Status(client.Get("/api/v1/status", authenticated)) == 401, "old session invalid after restart");
    const auto replacement = portal.Status();
    Check(Status(client.Post("/api/v1/pair", headers, Json{{"code", replacement.code}}.dump(), "application/json")) == 200,
          "new server session pairs");
    // The transport polls at 100 ms but a live peer may legitimately pause longer.
    const auto slow=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    Check(slow!=INVALID_SOCKET,"slow peer socket");
    sockaddr_in slow_address{};slow_address.sin_family=AF_INET;slow_address.sin_port=htons(static_cast<u_short>(port));
    InetPtonA(AF_INET,"127.0.0.1",&slow_address.sin_addr);
    Check(::connect(slow,reinterpret_cast<sockaddr*>(&slow_address),sizeof(slow_address))==0,"slow peer connect");
    const std::string first="GET / HTTP/1.1\r\n";
    Check(::send(slow,first.data(),static_cast<int>(first.size()),0)==static_cast<int>(first.size()),"slow peer prefix");
    std::this_thread::sleep_for(350ms);
    const auto last="Host: 127.0.0.1:"+std::to_string(port)+"\r\nConnection: close\r\n\r\n";
    Check(::send(slow,last.data(),static_cast<int>(last.size()),0)==static_cast<int>(last.size()),"slow peer suffix");
    const DWORD slow_timeout=2000;
    ::setsockopt(slow,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&slow_timeout),sizeof(slow_timeout));
    std::array<char,4096> slow_reply{};
    const auto slow_count=::recv(slow,slow_reply.data(),static_cast<int>(slow_reply.size()),0);
    Check(slow_count>0 && std::string_view{slow_reply.data(),static_cast<size_t>(slow_count)}.find(" 200 ")!=std::string_view::npos,
          "peer pause longer than OS poll timeout survives");
    ::closesocket(slow);
    // An unfinished body must not leave Stop blocked on an idle peer.
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(socket != INVALID_SOCKET, "partial peer socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<u_short>(port));
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
    Check(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "partial peer connect");
    const auto partial = "POST /api/v1/pair HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\nOrigin: " + origin + "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{";
    ::send(socket, partial.data(), static_cast<int>(partial.size()), 0);
    std::this_thread::sleep_for(100ms);
    const auto stop_at = std::chrono::steady_clock::now();
    portal.Stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_at;
    std::printf("Partial peer shutdown: %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(stop_elapsed).count()));
    Check(stop_elapsed < 3s, "partial body shutdown stays within suspension budget");
    ::closesocket(socket);
    UploadTests(port,argc>1 && std::string_view{argv[1]}=="--large");
    SystemUploadTests(port);
    LibraryTests(port);
    std::puts("PASS: network portal native TCP pairing/origin/limits/restart/partial-body lifecycle; Xbox/AppContainer LAN gate still required");
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: uncaught %s\n",error.what());return 1; }
}
