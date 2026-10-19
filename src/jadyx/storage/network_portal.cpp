// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/network_portal.h"
#include "jadyx/storage/network_portal_page.h"

// Match the httplib configuration used by the rest of common (including SSL feature macros).
#include "common/httplib.h"
#include <iphlpapi.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace Jadyx {
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

// Keep short OS polling intervals for cancellation, but tolerate 30s idle LAN retransmits.
// Windows WSAPoll may not wake on local shutdown, so a 30s kernel timeout is not suitable.
class PortalStream final : public httplib::Stream {
public:
    PortalStream(httplib::Stream& source_, const std::atomic<bool>& stopped_) : source{source_}, stopped{stopped_} {}
    bool is_readable() const override { return !stopped && !read_closed && source.is_readable(); }
    bool wait_readable() const override { return Wait(false); }
    bool wait_writable() const override { return Wait(true); }
    bool is_peer_alive() const override { return !stopped && source.is_peer_alive(); }
    ssize_t read(char* bytes, size_t size) override {
        if (read_closed) return -1;
        if (!is_readable() && !Wait(false)) return -1;
        return stopped ? -1 : source.read(bytes, size);
    }
    ssize_t write(const char* bytes, size_t size) override {
        if (!Wait(true)) return -1;
        return stopped ? -1 : source.write(bytes, size);
    }
    void get_remote_ip_and_port(std::string& ip, int& port) const override { source.get_remote_ip_and_port(ip, port); }
    void get_local_ip_and_port(std::string& ip, int& port) const override { source.get_local_ip_and_port(ip, port); }
    socket_t socket() const override { return source.socket(); }
    time_t duration() const override { return source.duration(); }
    void set_read_timeout(time_t, time_t = 0) override {} // Only our bounded, cancellable waits.
    void CloseAfterResponse() { read_closed=true; }
    bool Closing() const { return read_closed; }
private:
    bool Wait(bool write) const {
        if (!write && read_closed) return false;
        const auto until=Clock::now()+std::chrono::seconds{30};
        while (!stopped) {
            if (write ? source.wait_writable() : (source.is_readable() || source.wait_readable())) return !stopped;
            if (Clock::now() >= until) return false;
        }
        return false;
    }
    httplib::Stream& source;
    const std::atomic<bool>& stopped;
    bool read_closed{};
};

// Request handlers execute synchronously on one worker. This scoped slot lets the response
// hook disable httplib 0.46.0's drain after committing Connection: close on an early rejection.
thread_local PortalStream* current_portal_stream{};
struct PortalStreamScope {
    PortalStream* previous{current_portal_stream};
    explicit PortalStreamScope(PortalStream& stream) { current_portal_stream=&stream; }
    ~PortalStreamScope() { current_portal_stream=previous; }
};

// httplib's destructor does not close a socket bound before listen_after_bind starts. Keep
// ownership even if creating the listener/thread pool throws after a successful bind.
class PortalServer final : public httplib::Server {
public:
    void CancelClients() {
        std::lock_guard lock{clients_mutex};
        stopping = true;
        for (const auto socket : clients) ::shutdown(socket, SD_BOTH);
    }

    ~PortalServer() override {
        const auto socket = svr_sock_.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET) { ::shutdown(socket, SD_BOTH); ::closesocket(socket); }
    }

protected:
    bool process_and_close_socket(socket_t socket) override {
        {
            std::lock_guard lock{clients_mutex};
            if (stopping) { ::closesocket(socket); return false; }
            clients.insert(socket);
        }
        bool result = false;
        try {
            std::string remote, local;
            int remote_port{}, local_port{};
            httplib::detail::get_remote_ip_and_port(socket, remote, remote_port);
            httplib::detail::get_local_ip_and_port(socket, local, local_port);
            // Same plain-HTTP processing path as httplib 0.46.0. Own the final close so the
            // cancellation registry cannot refer to a recycled descriptor (including on error).
            result = httplib::detail::process_server_socket(svr_sock_, socket,
                keep_alive_max_count_, keep_alive_timeout_sec_, read_timeout_sec_, read_timeout_usec_,
                write_timeout_sec_, write_timeout_usec_,
                [&](httplib::Stream& stream, bool close, bool& closed) {
                    PortalStream cancellable{stream, stopping};
                    PortalStreamScope scope{cancellable};
                    const bool handled=process_request(cancellable, remote, remote_port, local, local_port, close, closed, nullptr);
                    closed |= cancellable.Closing();
                    return handled;
                });
        } catch (...) {}
        {
            std::lock_guard lock{clients_mutex};
            clients.erase(socket);
            ::shutdown(socket, SD_BOTH);
            ::closesocket(socket);
        }
        return result;
    }

private:
    std::mutex clients_mutex;
    std::set<socket_t> clients;
    std::atomic<bool> stopping{};
};



bool EqualSecret(std::string_view a, std::string_view b) {
    return a.size() == b.size() && !a.empty() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string RandomSession() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto value : bytes) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}

std::string RandomCode() {
    // Rejection sampling avoids modulo bias in the eight decimal digits.
    std::string result;
    while (result.size() < 8) {
        unsigned char byte{};
        if (RAND_bytes(&byte, 1) != 1) return {};
        if (byte < 250) result += static_cast<char>('0' + byte % 10);
    }
    return result;
}

void Reply(httplib::Response& response, int status, const Json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

bool Unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto result=std::from_chars(text.data(), text.data()+text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data()+text.size();
}

bool Decode64(const std::string& encoded, std::string& decoded) {
    if (encoded.empty() || encoded.size() > 1024 || encoded.size() % 4) return false;
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned accumulator{}, bits{};
    decoded.clear();
    bool padding=false;
    for (const char c : encoded) {
        if (c == '=') { padding=true; continue; }
        const auto digit=alphabet.find(c);
        if (padding || digit == std::string_view::npos) return false;
        accumulator=(accumulator << 6) | static_cast<unsigned>(digit);
        bits+=6;
        if (bits >= 8) { bits-=8; decoded+=static_cast<char>((accumulator >> bits) & 255); }
    }
    return httplib::detail::base64_encode(decoded) == encoded;
}

Json UploadJson(const UploadSnapshot& info) {
    return {{"id",info.id}, {"name",info.name}, {"fingerprint",info.fingerprint},
        {"phase",info.phase}, {"error",info.error}, {"length",info.length}, {"offset",info.offset},
        {"received",info.received}, {"free_bytes",info.free_bytes}, {"busy",info.busy}, {"available",info.available}};
}

bool LocalIpv4(const std::string& address) {
    in_addr ip{};
    if (InetPtonA(AF_INET, address.c_str(), &ip) != 1) return false;
    const auto v = ntohl(ip.s_addr);
    return (v >> 24) == 10 || (v >> 20) == 0xac1 || (v >> 16) == 0xc0a8 ||
           (v >> 16) == 0xa9fe || (v >> 24) == 127; // loopback used only by the native harness
}
} // namespace

std::vector<PortalAddress> PortalAddresses() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> data(size);
    ULONG result{};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(data.data()), &size);
        if (result != ERROR_BUFFER_OVERFLOW) break;
        if (size > 1024 * 1024) return {};
        data.resize(size);
    }
    if (result != NO_ERROR) return {};
    std::vector<PortalAddress> preferred, other;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(data.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp ||
            (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211)) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char address[INET_ADDRSTRLEN]{};
            auto* ip = &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            if (!InetNtopA(AF_INET, ip, address, sizeof(address)) || !LocalIpv4(address)) continue;
            // Friendly adapter names never become HTML or a command; the local UI renders text.
            std::string name = a->IfType == IF_TYPE_IEEE80211 ? "Wi-Fi" : "Ethernet";
            (a->FirstGatewayAddress ? preferred : other).push_back({std::move(name), address});
        }
    }
    preferred.insert(preferred.end(), other.begin(), other.end());
    return preferred;
}

struct NetworkPortal::Impl {
    std::mutex lifecycle;
    mutable std::mutex mutex;
    PortalStatus status;
    Clock::time_point expires{}, attempts_window{};
    unsigned attempts{};
    std::string session;
    std::string authority;
    std::unique_ptr<PortalServer> server;
    bool exclusive_socket{};
    std::jthread listener;
    std::unique_ptr<NspUpload> upload;
    bool storage_failed{};

    void Shutdown() {
        if (upload) upload->Cancel();
        {
            std::lock_guard lock{mutex};
            status.running = status.paired = false;
            status.code.clear(); status.url.clear(); session.clear();
        }
        if (server) { server->stop(); server->CancelClients(); server->decommission(); }
        if (listener.joinable()) listener.join();
        server.reset();
        if (upload) upload->Suspend();
    }

    void Routes() {
        server->new_task_queue = [] { return new httplib::ThreadPool(4, 4, 8); };
        server->set_tcp_nodelay(true);
        server->set_socket_options([this](socket_t socket) {
            const BOOL exclusive = TRUE;
            exclusive_socket = ::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0;
        });
        server->set_payload_max_length(NspUpload::MaxChunk);
        // PortalStream extends these short OS polls into a cancellable 30s idle wait.
        server->set_read_timeout(0, 100000);
        server->set_write_timeout(0, 100000);
        server->set_keep_alive_timeout(2);
        server->set_keep_alive_max_count(10000);
        server->set_post_routing_handler([](const auto&, auto& response) {
            if (response.get_header_value("Connection") == "close" && current_portal_stream)
                current_portal_stream->CloseAfterResponse();
        });
        server->set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
            {"Referrer-Policy", "no-referrer"}, {"X-Frame-Options", "DENY"},
            {"Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}});
        server->set_pre_routing_handler([this](const auto& request, auto& response) {
            std::lock_guard lock{mutex};
            if (!status.running) { Reply(response, 503, {{"error", "stopped"}}); return httplib::Server::HandlerResponse::Handled; }
            if (request.get_header_value_count("Host") != 1 || request.get_header_value("Host") != authority ||
                request.get_header_value_count("Origin") > 1 ||
                (request.has_header("Origin") && request.get_header_value("Origin") != "http://" + authority)) {
                Reply(response, 403, {{"error", "origin"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            const bool patch=request.method == "PATCH";
            std::uint64_t length{};
            if (request.has_header("Transfer-Encoding") || request.has_header("Content-Encoding") ||
                (request.has_header("Content-Length") &&
                 (!Unsigned(request.get_header_value("Content-Length"), length) || length > (patch ? NspUpload::MaxChunk :
                     request.path == "/api/v1/pair" ? 1024 : 4096)))) {
                response.set_header("Connection", "close");
                Reply(response, 413, {{"error", "chunk"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            if (request.path == "/" && request.method == "GET") return httplib::Server::HandlerResponse::Unhandled;
            if (request.path == "/api/v1/pair" && request.method == "POST" &&
                request.get_header_value("Origin") == "http://" + authority)
                return httplib::Server::HandlerResponse::Unhandled;
            if (request.get_header_value_count("Authorization") != 1 ||
                !EqualSecret(request.get_header_value("Authorization"), "Bearer " + session) || session.empty()) {
                Reply(response, 401, {{"error", "session"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server->Get("/", [](const auto&, auto& response) {
            response.set_content(PortalWeb::Page.data(), PortalWeb::Page.size(), "text/html; charset=utf-8");
        });
        server->Post("/api/v1/pair", [this](const auto& request, auto& response) {
            const auto body = Json::parse(request.body, nullptr, false);
            std::lock_guard lock{mutex};
            const auto now = Clock::now();
            if (now - attempts_window >= std::chrono::minutes{1}) { attempts_window = now; attempts = 0; }
            if (attempts >= 5) {
                response.set_header("Retry-After", "60");
                Reply(response, 429, {{"error", "attempts"}}); return;
            }
            ++attempts;
            if (!status.running || body.is_discarded() || !body.is_object() ||
                !body.contains("code") || !body["code"].is_string() ||
                now >= expires || !EqualSecret(body["code"].get<std::string>(), status.code)) {
                Reply(response, 401, {{"error", "code"}}); return;
            }
            auto next = RandomSession();
            if (next.empty()) { Reply(response, 503, {{"error", "random"}}); return; }
            // A code pairs one browser once. Existing sessions are invalidated on re-pairing.
            session = std::move(next);
            status.paired = true;
            expires = now;
            Reply(response, 200, {{"session", session}});
        });
        server->Get("/api/v1/status", [this](const auto&, auto& response) {
            Reply(response, 200, {{"running", true}, {"stage", upload ? "nsp" : "connection-test"},
                {"uploads", bool(upload)}, {"transfer", upload ? UploadJson(upload->Snapshot()) : Json::object()}});
        });
        UploadRoutes();
    }

    void UploadRoutes() {
        // Auth/Host/Origin are checked before these routes. No server-supplied destination path
        // is ever accepted from the browser; ids are matched against the one current record.
        const auto version=[](const auto& request, auto& response) {
            response.set_header("Tus-Resumable", "1.0.0");
            response.set_header("Tus-Version", "1.0.0");
            if (request.get_header_value("Tus-Resumable") == "1.0.0") return true;
            Reply(response, 412, {{"error", "version"}}); return false;
        };
        const auto result=[](auto& response, const UploadResult& value) {
            response.status=value.status;
            if (!value.error.empty()) Reply(response, value.status, {{"error",value.error}});
        };
        server->Options("/api/v1/uploads", [](const auto&, auto& response) {
            response.status=204;
            response.set_header("Tus-Version", "1.0.0");
            response.set_header("Tus-Extension", "creation,termination");
            response.set_header("Tus-Max-Size",std::to_string(NspUpload::MaxLength));
        });
        server->Post("/api/v1/uploads", [this,version,result](const auto& request, auto& response) {
            if (!version(request,response)) return;
            if (!upload) { Reply(response,503,{{"error","unavailable"}}); return; }
            std::uint64_t length{};
            if (!Unsigned(request.get_header_value("Upload-Length"),length) || request.has_header("Upload-Defer-Length") ||
                (request.has_header("Content-Length") && request.get_header_value("Content-Length") != "0")) { Reply(response,400,{{"error","length"}}); return; }
            if (length > NspUpload::MaxLength) { Reply(response,413,{{"error","length"}}); return; }
            const auto metadata=request.get_header_value("Upload-Metadata");
            std::string name,fingerprint;
            std::size_t begin{};
            bool valid=true;
            while (begin < metadata.size()) {
                const auto end=metadata.find(',',begin);
                auto item=metadata.substr(begin,end == std::string::npos ? end : end-begin);
                while (!item.empty() && item.front() == ' ') item.erase(0,1);
                const auto separator=item.find(' ');
                if (separator == std::string::npos) { valid=false; break; }
                std::string decoded;
                if (!Decode64(item.substr(separator+1),decoded)) { valid=false; break; }
                const auto key=item.substr(0,separator);
                if (key == "filename" && name.empty()) name=std::move(decoded);
                else if (key == "fingerprint" && fingerprint.empty()) fingerprint=std::move(decoded);
                else { valid=false; break; }
                if (end == std::string::npos) break;
                begin=end+1;
            }
            if (!valid) { Reply(response,400,{{"error","metadata"}}); return; }
            const auto created=upload->Create(std::move(name),std::move(fingerprint),length);
            result(response,created);
            if (created.status == 201) {
                response.set_header("Location","/api/v1/uploads/"+upload->Snapshot().id);
                response.set_header("Upload-Offset","0");
            }
        });
        server->Get("/api/v1/uploads/([0-9a-f]{32})", [this,version](const auto& request, auto& response) {
            // httplib dispatches HEAD through its GET route with the body suppressed.
            if (!version(request,response)) return;
            if (!upload) { Reply(response,404,{{"error","upload"}}); return; }
            const auto info=upload->Snapshot();
            if (info.id != request.matches[1].str() || info.phase == "complete") { Reply(response,404,{{"error","upload"}}); return; }
            response.set_header("Upload-Offset",std::to_string(info.offset));
            response.set_header("Upload-Length",std::to_string(info.length));
            response.status=200;
        });
        server->Patch("/api/v1/uploads/([0-9a-f]{32})",
            [this,version,result](const httplib::Request& request, httplib::Response& response, const httplib::ContentReader& reader) {
                if (!version(request,response)) return;
                if (!upload) { Reply(response,404,{{"error","upload"}}); return; }
                std::uint64_t offset{},length{};
                if (request.get_header_value("Content-Type") != "application/offset+octet-stream") { Reply(response,415,{{"error","type"}}); return; }
                if (!Unsigned(request.get_header_value("Upload-Offset"),offset) ||
                    !Unsigned(request.get_header_value("Content-Length"),length)) { Reply(response,400,{{"error","offset"}}); return; }
                const auto appended=upload->Append(request.matches[1].str(),offset,length,
                    [&](const UploadSink& sink) { return reader([&](const char* bytes, std::size_t count) { return sink(bytes,count); }); });
                result(response,appended);
                response.set_header("Upload-Offset",std::to_string(upload->Snapshot().offset));
            });
        server->Delete("/api/v1/uploads/([0-9a-f]{32})", [this,version,result](const auto& request, auto& response) {
            if (!version(request,response)) return;
            if (!upload) { Reply(response,404,{{"error","upload"}}); return; }
            result(response,upload->Discard(request.matches[1].str()));
        });
        server->Post("/api/v1/uploads/([0-9a-f]{32})/(finish|pause|rename)",
            [this,result](const auto& request, auto& response) {
                if (!upload) { Reply(response,404,{{"error","upload"}}); return; }
                const auto id=request.matches[1].str(),action=request.matches[2].str();
                if (action == "finish") result(response,upload->Ready(id));
                else if (action == "pause") result(response,upload->Pause(id));
                else {
                    const auto json=Json::parse(request.body,nullptr,false);
                    if (!json.is_object() || !json.contains("name") || !json["name"].is_string()) { Reply(response,400,{{"error","file"}}); return; }
                    result(response,upload->Rename(id,json["name"].template get<std::string>()));
                }
            });
    }
};

NetworkPortal::NetworkPortal(std::filesystem::path games) : impl{std::make_unique<Impl>()} {
    if (!games.empty()) {
        try { impl->upload=std::make_unique<NspUpload>(std::move(games)); }
        catch (...) { impl->storage_failed=true; }
    }
}
NetworkPortal::~NetworkPortal() { Stop(); }

bool NetworkPortal::Start(const std::string& ipv4, unsigned short port) {
    std::lock_guard lifecycle_lock{impl->lifecycle};
    impl->Shutdown();
    try {
    if (impl->storage_failed) {
        std::lock_guard lock{impl->mutex}; impl->status.error=PortalError::Storage; return false;
    }
    if (impl->upload) impl->upload->Resume();
    if (!LocalIpv4(ipv4) || port == 0) {
        std::lock_guard lock{impl->mutex}; impl->status.error = PortalError::NoAddress; return false;
    }
    auto code = RandomCode();
    if (code.empty()) {
        std::lock_guard lock{impl->mutex}; impl->status.error = PortalError::Random; return false;
    }
    impl->server = std::make_unique<PortalServer>();
    impl->exclusive_socket = false;
    impl->authority = ipv4 + ":" + std::to_string(port);
    impl->Routes();
    if (!impl->server->bind_to_port(ipv4, port) || !impl->exclusive_socket) {
        impl->server.reset();
        std::lock_guard lock{impl->mutex}; impl->status.error = PortalError::Bind; return false;
    }
    {
        std::lock_guard lock{impl->mutex};
        impl->status = {.running = true, .url = "http://" + impl->authority, .code = std::move(code)};
        impl->expires = Clock::now() + std::chrono::minutes{5};
        impl->attempts_window = Clock::now(); impl->attempts = 0;
    }
    // Listener, handlers and their state outlive every request. Stop joins before any reset.
    impl->listener = std::jthread([this] {
        bool ok = false;
        try { ok = impl->server->listen_after_bind(); } catch (...) {}
        // Also release Start's ready wait if listener/thread-pool initialization failed.
        impl->server->decommission();
        std::lock_guard lock{impl->mutex};
        if (!ok && impl->status.running) impl->status.error = PortalError::Listen;
        impl->status.running = false;
    });
    impl->server->wait_until_ready();
    return Status().running;
    } catch (...) {
        impl->Shutdown();
        std::lock_guard lock{impl->mutex};
        impl->status.error = PortalError::Listen;
        return false;
    }
}

void NetworkPortal::Stop() {
    std::lock_guard lifecycle_lock{impl->lifecycle};
    impl->Shutdown();
}

bool NetworkPortal::RenewCode() {
    std::lock_guard lifecycle_lock{impl->lifecycle};
    auto code = RandomCode();
    std::lock_guard lock{impl->mutex};
    if (!impl->status.running || code.empty()) return false;
    impl->status.code = std::move(code);
    impl->expires = Clock::now() + std::chrono::minutes{5};
    // Do not reset the attempt limiter: renewing must not defeat brute-force protection.
    return true;
}

PortalStatus NetworkPortal::Status() const {
    PortalStatus result;
    {
        std::lock_guard lock{impl->mutex};
        result=impl->status;
        result.code_expired=Clock::now() >= impl->expires;
    }
    if (impl->upload) result.transfer=impl->upload->Snapshot();
    return result;
}

void NetworkPortal::SetUploadsAvailable(bool enabled) { if (impl->upload) impl->upload->SetAvailable(enabled); }
bool NetworkPortal::BlockForLocalOperation() { return !impl->upload || impl->upload->BlockForLocalOperation(); }
std::string NetworkPortal::FinalizeGame(const NspValidator& validate) {
    if (!impl->upload) return "unavailable";
    const auto error=impl->upload->Finalize(validate);
    if (error == "interrupted" && Status().running) impl->upload->Resume();
    return error;
}
void NetworkPortal::CancelTransfer() { if (impl->upload) impl->upload->Cancel(); }

} // namespace Jadyx
